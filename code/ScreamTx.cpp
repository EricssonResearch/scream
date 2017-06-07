#include "RtpQueue.h"
#include "ScreamTx.h"
#include "ScreamRx.h"
#include <cstdint>
#include <cmath>
#include <string.h>
#include <iostream>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

using namespace std;

// === Some good to have features, SCReAM works also
//     with these disabled
// Fast start can resume if little or no congestion detected
static const bool kEnableConsecutiveFastStart = true;
// Packet pacing reduces jitter
static const bool kEnablePacketPacing = true;
static const float kPacketPacingHeadRoom = 1.5f;


// Rate update interval
static const uint64_t kRateAdjustInterval_us = 200000; // 200ms

// ==== Less important tuning parameters ====
// Min pacing interval and min pacing rate
static const float kMinPaceInterval = 0.000f;
static const float kMinimumBandwidth = 5000.0f; // bps
// Initial MSS, this is set quite low in order to make it possible to
//  use SCReAM with audio only
static const int kInitMss = 100;

// Min and max queue delay target
static const float kQueueDelayTargetMax = 0.3f; //ms

// Congestion window validation
static const float kBytesInFlightHistInterval_us = 1000000; // Time (s) between stores 1s
static const float kMaxBytesInFlightHeadRoom = 1.1f;
// Queue delay trend and shared bottleneck detection
static const uint64_t kQueueDelayFractionHistInterval_us = 50000; // 50ms
// video rate estimation update period
static const uint64_t kRateUpdateInterval_us = 50000;  // 50ms

// Packet reordering margin (us)
static const uint64_t kReorderTime_us = 20000;
static const uint64_t kMinRtpQueueDiscardInterval_us = 1000000;

// Update interval for base delay history
static const uint64_t kBaseDelayUpdateInterval_us = 10000000;

// Min CWND in MSS
static int kMinCwndMss = 3;

ScreamTx::ScreamTx(float lossBeta_,
    float queueDelayTargetMin_,
    bool enableSbd_,
    float gainUp_,
    float gainDown_,
    int cwnd_
    )
    : sRttSh_us(0),
    lossBeta(lossBeta_),
    queueDelayTargetMin(queueDelayTargetMin_),
    enableSbd(enableSbd_),
    gainUp(gainUp_),
    gainDown(gainDown_),
    sRtt_us(0),
    ackedOwd(0),
    baseOwd(UINT32_MAX),
    queueDelay(0.0),
    queueDelayFractionAvg(0.0),
    queueDelayTrend(0.0),
    queueDelayTarget(queueDelayTargetMin),
    queueDelaySbdVar(0.0),
    queueDelaySbdMean(0.0),
    queueDelaySbdMeanSh(0.0),

    bytesNewlyAcked(0),
    mss(kInitMss),
    cwnd(kInitMss * 2),
    cwndMin(kInitMss * 2),
    lastBytesInFlightT_us(0),
    bytesInFlightMaxLo(0),
    bytesInFlightMaxHi(0),

    lossEvent(false),
    wasLossEvent(false),
    lossEventRate(0.0),

    inFastStart(true),

    //pacingBitrate(0.0),
    paceInterval_us(0),
    paceInterval(0.0),

    isInitialized(false),
    lastSRttUpdateT_us(0),
    lastBaseOwdAddT_us(0),
    baseOwdResetT_us(0),
    lastAddToQueueDelayFractionHistT_us(0),
    lastLossEventT_us(0),
    lastTransmitT_us(0),
    nextTransmitT_us(0),
    lastRateUpdateT_us(0),
    accBytesInFlightMax(0),
    nAccBytesInFlightMax(0),
    rateTransmitted(0),
    queueDelayTrendMem(0.0f),
    lastCwndUpdateT_us(0),
    bytesInFlight(0),
    lastBaseDelayRefreshT_us(0),
    maxRate(0.0f)

{
    if (cwnd_ == 0) {
        cwnd = kInitMss * 2;
    }
    else {
        cwnd = cwnd_;
    }

    for (int n = 0; n < kBaseOwdHistSize; n++)
        baseOwdHist[n] = UINT32_MAX;
    baseOwdHistPtr = 0;
    for (int n = 0; n < kQueueDelayFractionHistSize; n++)
        queueDelayFractionHist[n] = 0.0f;
    queueDelayFractionHistPtr = 0;
    for (int n = 0; n < kQueueDelayNormHistSize; n++)
        queueDelayNormHist[n] = 0.0f;
    queueDelayNormHistPtr = 0;
    for (int n = 0; n < kBytesInFlightHistSize; n++) {
        bytesInFlightHistLo[n] = 0;
        bytesInFlightHistHi[n] = 0;
    }
    bytesInFlightHistPtr = 0;
    nStreams = 0;
    for (int n = 0; n < kMaxStreams; n++)
        streams[n] = NULL;

    queueDelayMax = 0.0f;
    queueDelayMinAvg = 0.0f;
    queueDelayMin = 1000.0;
}

ScreamTx::~ScreamTx() {
    for (int n = 0; n < nStreams; n++)
        delete streams[n];
}

/*
* Register new stream
*/
void ScreamTx::registerNewStream(RtpQueueIface *rtpQueue,
    uint32_t ssrc,
    float priority,
    float minBitrate,
    float startBitrate,
    float maxBitrate,
    float rampUpSpeed,
    float maxRtpQueueDelay,
    float txQueueSizeFactor,
    float queueDelayGuard,
    float lossEventRateScale) {
    Stream *stream = new Stream(this,
        rtpQueue,
        ssrc,
        priority,
        minBitrate,
        startBitrate,
        maxBitrate,
        rampUpSpeed,
        maxRtpQueueDelay,
        txQueueSizeFactor,
        queueDelayGuard,
        lossEventRateScale);
    streams[nStreams++] = stream;
}

void ScreamTx::updateBitrateStream(uint32_t ssrc,
                         float minBitrate,
                         float maxBitrate) {
    Stream *stream = getStream(ssrc);
    stream->minBitrate = minBitrate;
    stream->maxBitrate = maxBitrate;
}

RtpQueueIface * ScreamTx::getStreamQueue(uint32_t ssrc) {
    Stream* stream = getStream(ssrc);
    return stream->rtpQueue;
}


/*
* New media frame
*/
void ScreamTx::newMediaFrame(uint64_t time_us, uint32_t ssrc, int bytesRtp) {
    if (!isInitialized) initialize(time_us);

    Stream *stream = getStream(ssrc);
    stream->updateTargetBitrate(time_us);
    if (time_us - lastBaseDelayRefreshT_us < sRtt_us * 2) {
        /*
        * _Very_ long periods of congestion can cause the base delay to increase
        * with the effect that the queue delay is estimated wrong, therefore we seek to
        * refresh the whole thing by deliberately allowing the network queue to drain
        * Clear the RTP queue for 2 RTTs, this will allow the queue to drain so that we
        * get a good estimate for the min queue delay.
        * This funtion is executed very seldom so it should not affect overall experience too much
        */
        stream->rtpQueue->clear();
    }
    else {
        stream->bytesRtp += bytesRtp;
        /*
        * Need to update MSS here, otherwise it will be nearly impossible to
        * transmit video packets, this because of the small initial MSS
        * which is necessary to make SCReAM work with audio only
        */
        int sizeOfNextRtp = stream->rtpQueue->sizeOfNextRtp();
        mss = std::max(mss, sizeOfNextRtp);
        cwndMin = 2 * mss;
        cwnd = max(cwnd, cwndMin);
    }
}

/*
* Determine active streams
*/
void ScreamTx::determineActiveStreams(uint64_t time_us) {
    float surplusBitrate = 0.0f;
    float sumPrio = 0.0;
    bool streamSetInactive = false;
    for (int n = 0; n < nStreams; n++) {
        if (time_us - streams[n]->lastFrameT_us > 1000000 && streams[n]->isActive) {
            streams[n]->isActive = false;
            surplusBitrate += streams[n]->targetBitrate;
            streams[n]->targetBitrate = streams[n]->minBitrate;
            streamSetInactive = true;
        }
        else {
            sumPrio += streams[n]->targetPriority;
        }
    }
    if (streamSetInactive) {
        for (int n = 0; n < nStreams; n++) {
            if (streams[n]->isActive) {
                streams[n]->targetBitrate = std::min(streams[n]->maxBitrate,
                    streams[n]->targetBitrate +
                    surplusBitrate*streams[n]->targetPriority / sumPrio);
            }
        }
    }
}

/*
* Determine if OK to transmit RTP packet
*/
float ScreamTx::isOkToTransmit(uint64_t time_us, uint32_t &ssrc) {
    if (!isInitialized) initialize(time_us);
    /*
    * Update rateTransmitted and rateAcked if time for it
    * This is used in video rate computation
    * The update interval is doubled at very low bitrates,
    * the reason is that the feedback interval is very low then and
    * a longer window is needed to avoid aliasing effects
    */
    uint64_t tmp = kRateUpdateInterval_us;
    float rateAcked = 0.0f;
    for (int n = 0; n < nStreams; n++) {
        rateAcked += streams[n]->rateAcked;
    }
    if (rateAcked < 50000.0f) {
        tmp *= 2;
    }
    if (time_us - lastRateUpdateT_us > tmp) {
        rateTransmitted = 0.0;
        for (int n = 0; n < nStreams; n++) {
            streams[n]->updateRate(time_us);
            rateTransmitted += streams[n]->rateTransmitted;
        }
        lastRateUpdateT_us = time_us;
        /*
        * Adjust stream priorities
        */
        adjustPriorities(time_us);

        /*
        * Update maxRate
        */
        maxRate = 0.0f;
        for (int n = 0; n < nStreams; n++)
            maxRate += streams[n]->getMaxRate();
    }

    /*
    * Get index to the prioritized RTP queue
    */
    Stream* stream = getPrioritizedStream(time_us);

    if (stream == NULL)
        /*
        * No RTP packets to transmit
        */
        return -1.0f;
    ssrc = stream->ssrc;


    /*
    * Enforce packet pacing
    */
    float retVal = 0.0f;
    if (kEnablePacketPacing && nextTransmitT_us > time_us){
        retVal = (nextTransmitT_us - time_us) / 1e6f;
    }


    bytesInFlightMaxLo = 0;
    if (nAccBytesInFlightMax > 0) {
        bytesInFlightMaxLo = accBytesInFlightMax / nAccBytesInFlightMax;
    }
    bytesInFlightMaxHi = std::max(bytesInFlight, bytesInFlightMaxHi);

    /*
    * Update bytes in flight history for congestion window validation
    */
    if (time_us - lastBytesInFlightT_us > kBytesInFlightHistInterval_us) {
        bytesInFlightHistLo[bytesInFlightHistPtr] = bytesInFlightMaxLo;
        bytesInFlightHistHi[bytesInFlightHistPtr] = bytesInFlightMaxHi;
        bytesInFlightHistPtr = (bytesInFlightHistPtr + 1) % kBytesInFlightHistSize;
        lastBytesInFlightT_us = time_us;
        accBytesInFlightMax = 0;
        nAccBytesInFlightMax = 0;
        bytesInFlightMaxHi = 0;
        /*
        * In addition, reset MSS, this is useful in case for instance
        * a video stream is put on hold, leaving only audio packets to be
        * transmitted
        */
        mss = kInitMss;
        cwndMin = kMinCwndMss * mss;
        cwnd = max(cwnd, cwndMin);
    }

    int sizeOfNextRtp = stream->rtpQueue->sizeOfNextRtp();
    if (sizeOfNextRtp == -1){
        return -1.0f;
    }
    /*
    * Determine if window is large enough to transmit
    * an RTP packet
    */
    bool exit = false;
    if (queueDelay < queueDelayTarget)
        exit = (bytesInFlight + sizeOfNextRtp) > cwnd + mss;
    else
        exit = (bytesInFlight + sizeOfNextRtp) > cwnd;
    /*
    * A retransmission time out mechanism to avoid deadlock
    */
    if (time_us - lastTransmitT_us > 200000) { // 200ms
        exit = false;
    }
    if (!exit) {
        /*
        * Return value 0.0 = RTP packet can be immediately transmitted
        */
        return retVal;
    }

    return -1.0f;
}

/*
* RTP packet transmitted
*/
float ScreamTx::addTransmitted(uint64_t time_us,
    uint32_t ssrc,
    int size,
    uint16_t seqNr) {
    if (!isInitialized)
        initialize(time_us);

    Stream *stream = getStream(ssrc);

    int k = 0;
    int ix = -1;
    while (k < kMaxTxPackets) {
        stream->txPacketsPtr = (stream->txPacketsPtr + 1) % kMaxTxPackets;
        if (stream->txPackets[stream->txPacketsPtr].isUsed == false) {
            ix = stream->txPacketsPtr;
            break;
        }
        k++;
    }
    if (ix == -1) {
        /*
        * If you end up here then it is necessary to increase
        * kMaxTxPackets... Or it may be the case that feedback
        * is lost.
        */
        ix = 0;
        stream->txPacketsPtr = ix;
        cerr << "Max number of txPackets allocated" << endl;
    }
    Transmitted *txPacket = &(stream->txPackets[ix]);
    txPacket->timestamp = (uint32_t)(time_us / 1000);
    txPacket->timeTx_us = time_us;
    txPacket->size = size;
    txPacket->seqNr = seqNr;
    txPacket->isUsed = true;
    txPacket->isAcked = false;
    txPacket->isAfterReceivedEdge = false;
    /*
    * Update bytesInFlight
    */
    computeBytesInFlight();

    stream->bytesTransmitted += size;
    lastTransmitT_us = time_us;
    /*
    * Add credit to unserved streams
    */
    addCredit(time_us, stream, size);
    /*
    * Reduce used credit for served stream
    */
    subtractCredit(time_us, stream, size);

    /*
    * Update MSS and cwndMin
    */
    mss = std::max(mss, size);
    cwndMin = 2 * mss;
    cwnd = max(cwnd, cwndMin);

    /*
    * Determine when next RTP packet can be transmitted
    */
    if (kEnablePacketPacing)
        nextTransmitT_us = time_us + paceInterval_us;
    else
        nextTransmitT_us = time_us;

    return paceInterval;
}

/*
* New incoming feedback
*/
void ScreamTx::incomingFeedback(uint64_t time_us,
    uint32_t ssrc,
    uint32_t timestamp,
    uint16_t highestSeqNr,
    uint64_t ackVector,
    bool qBit) {
    if (!isInitialized) initialize(time_us);
    Stream *stream = getStream(ssrc);
    accBytesInFlightMax += bytesInFlight;
    nAccBytesInFlightMax++;
    Transmitted *txPackets = stream->txPackets;
    for (int n = 0; n < kMaxTxPackets; n++) {
        /*
        * Loop through TX packets
        */
        if (txPackets[n].isUsed) {
            /*
            * RTP packet is in flight
            */
            Transmitted *tmp = &txPackets[n];

            /*
            * Compute SN difference
            */
            uint16_t diff = highestSeqNr - tmp->seqNr;
            diff -= 1;

            if (diff <= kAckVectorBits) {
                if (ackVector & (INT64_C(1) << diff)) {
                    /*
                    * SN marked as received
                    */
                    tmp->isAcked = true;
                }
            }

            /*
            * Receiption of packet given by highestSeqNr
            */
            if (tmp->seqNr == highestSeqNr) {
                tmp->isAcked = true;
                stream->hiSeqAck = highestSeqNr;
                ackedOwd = timestamp - tmp->timestamp;
                uint64_t rtt = time_us - tmp->timeTx_us;

                sRttSh_us = (7 * sRttSh_us + rtt) / 8;
                if (time_us - lastSRttUpdateT_us > sRttSh_us) {
                    sRtt_us = (7 * sRtt_us + sRttSh_us) / 8;
                    lastSRttUpdateT_us = time_us;
                }
                stream->timeTxAck_us = tmp->timeTx_us;
            }
        }
    }

    /*
    * Determine if loss event has occured
    */
    for (int n = 0; n < kMaxTxPackets; n++) {
        /*
        * Loop through TX packets
        */
        if (txPackets[n].isUsed) {
            Transmitted *tmp = &txPackets[n];
            /*
            * RTP packet is in flight
            */
            /*
            * Wrap-around safety net
            */
            uint32_t seqNrExt = tmp->seqNr;
            uint32_t highestSeqNrExt = highestSeqNr;
            if (seqNrExt < highestSeqNrExt && highestSeqNrExt - seqNrExt > 20000)
                seqNrExt += 65536;
            else if (seqNrExt > highestSeqNrExt && seqNrExt - highestSeqNrExt > 20000)
                highestSeqNrExt += 65536;

            /*
            * RTP packets with a sequence number lower
            * than or equal to the highest received sequence number
            * are treated as received even though they are not
            * This advances the send window, similar to what
            * SACK does in TCP
            */
            if (seqNrExt <= highestSeqNrExt && tmp->isAfterReceivedEdge == false) {
                bytesNewlyAcked += tmp->size;
                stream->bytesAcked += tmp->size;
                tmp->isAfterReceivedEdge = true;
            }

            if (tmp->timeTx_us + kReorderTime_us < stream->timeTxAck_us && !tmp->isAcked) {
                /*
                * Packet ACK is delayed more than kReorderTime_us after an ACK of a higher SN packet
                * raise a loss event and remove from TX list
                */
                if (time_us - lastLossEventT_us > sRtt_us) {
                    lossEvent = true;
                }
                stream->bytesLost += tmp->size;
                tmp->isUsed = false;
                stream->repairLoss = true;
            }
            else if (tmp->isAcked) {
                tmp->isUsed = false;
            }
        }
    }

    if (lossEvent) {
        lastLossEventT_us = time_us;
        for (int n = 0; n < nStreams; n++) {
            Stream *tmp = streams[n];
            tmp->lossEventFlag = true;
        }
    }
    computeBytesInFlight();
    updateCwnd(time_us);
}


float ScreamTx::getTargetBitrate(uint32_t ssrc) {
    return  getStream(ssrc)->getTargetBitrate();
}

void ScreamTx::setTargetPriority(uint32_t ssrc, float priority) {
    getStream(ssrc)->targetPriority = priority;
    getStream(ssrc)->targetPriorityInv = 1.0f / priority;
}

void ScreamTx::printLog(float time, char *s) {
    int inFlightMax = std::max(bytesInFlight, getMaxBytesInFlightHi());
    sprintf(s, "%4.3f, %4.3f, %4.3f, %4.3f, %6d, %6d, %1d, ",
        queueDelay, queueDelayMax, queueDelayMinAvg, getSRtt(),
        cwnd, bytesInFlight, isInFastStart());

    queueDelayMax = 0.0;
    for (int n = 0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        char s2[200];
        sprintf(s2, "%4.3f, %5.0f, %5.0f, %5.0f, %5.0f, %5.0f, %5d, ",
            tmp->rtpQueue->getDelay(time),
            tmp->targetBitrate / 1000.0f, tmp->rateRtp / 1000.0f,
            tmp->rateTransmitted / 1000.0f, tmp->rateAcked / 1000.0f,
            tmp->rateLost / 1000.0f,
            tmp->hiSeqAck);
        strcat(s, s2);
    }
}

void ScreamTx::initialize(uint64_t time_us) {
    isInitialized = true;
    lastSRttUpdateT_us = time_us;
    lastBaseOwdAddT_us = time_us;
    baseOwdResetT_us = time_us;
    lastAddToQueueDelayFractionHistT_us = time_us;
    lastLossEventT_us = time_us;
    lastTransmitT_us = time_us;
    nextTransmitT_us = time_us;
    lastRateUpdateT_us = time_us;
    lastAdjustPrioritiesT_us = time_us;
    lastRttT_us = time_us;
    lastBaseDelayRefreshT_us = time_us + 1000000;
}

ScreamTx::Stream::Stream(ScreamTx *parent_,
    RtpQueueIface *rtpQueue_,
    uint32_t ssrc_,
    float priority_,
    float minBitrate_,
    float startBitrate_,
    float maxBitrate_,
    float rampUpSpeed_,
    float maxRtpQueueDelay_,
    float txQueueSizeFactor_,
    float queueDelayGuard_,
    float lossEventRateScale_) {
    parent = parent_;
    rtpQueue = rtpQueue_;
    ssrc = ssrc_;
    targetPriority = priority_;
    targetPriorityInv = 1.0f / targetPriority;
    minBitrate = minBitrate_;
    maxBitrate = maxBitrate_;
    targetBitrate = std::min(maxBitrate, std::max(minBitrate, startBitrate_));
    rampUpSpeed = rampUpSpeed_;
    maxRtpQueueDelay = maxRtpQueueDelay_;
    txQueueSizeFactor = txQueueSizeFactor_;
    queueDelayGuard = queueDelayGuard_;
    lossEventRateScale = lossEventRateScale_;
    targetBitrateHistUpdateT_us = 0;
    targetBitrateI = 1.0f;
    credit = 0;
    bytesTransmitted = 0;
    bytesAcked = 0;
    bytesLost = 0;
    hiSeqAck = 0;
    rateTransmitted = 0.0f;
    rateAcked = 0.0f;
    rateLost = 0.0f;
    lossEventFlag = false;
    txSizeBitsAvg = 0.0f;
    lastRateUpdateT_us = 0;
    lastBitrateAdjustT_us = 0;
    lastTargetBitrateIUpdateT_us = 0;
    bytesRtp = 0;
    rateRtp = 0.0f;
    for (int n = 0; n < kRateUpDateSize; n++) {
        rateRtpHist[n] = 0.0f;
        rateAckedHist[n] = 0.0f;
        rateLostHist[n] = 0.0f;
        rateTransmittedHist[n] = 0.0f;
    }
    rateUpdateHistPtr = 0;
    for (int n = 0; n < kTargetBitrateHistSize; n++) {
        targetBitrateHist[n] = 0;
    }
    targetBitrateHistPtr = 0;
    targetRateScale = 1.0;
    isActive = false;
    lastFrameT_us = 0;
    initTime_us = 0;
    rtpQueueDiscard = false;
    lastRtpQueueDiscardT_us = 0;
    rateLost = 0.0;
    bytesLost = 0;
    wasRepairLoss = false;
    repairLoss = false;
    for (int n = 0; n < kMaxTxPackets; n++)
        txPackets[n].isUsed = false;
    txPacketsPtr = 0;
}

/*
* Update the estimated max media rate
*/
void ScreamTx::Stream::updateRate(uint64_t time_us) {
    if (lastRateUpdateT_us != 0) {
        float tDelta = (time_us - lastRateUpdateT_us) / 1e6f;

        rateTransmittedHist[rateUpdateHistPtr] = bytesTransmitted*8.0f / tDelta;
        rateAckedHist[rateUpdateHistPtr] = bytesAcked*8.0f / tDelta;
        rateLostHist[rateUpdateHistPtr] = bytesLost*8.0f / tDelta;
        rateRtpHist[rateUpdateHistPtr] = bytesRtp * 8.0f / tDelta;
        rateUpdateHistPtr = (rateUpdateHistPtr + 1) % kRateUpDateSize;
        rateTransmitted = 0.0f;
        rateAcked = 0.0f;
        rateLost = 0.0f;
        rateRtp = 0.0f;
        for (int n = 0; n < kRateUpDateSize; n++) {
            rateTransmitted += rateTransmittedHist[n];
            rateAcked += rateAckedHist[n];
            rateLost += rateLostHist[n];
            rateRtp += rateRtpHist[n];
        }
        rateTransmitted /= kRateUpDateSize;
        rateAcked /= kRateUpDateSize;
        rateLost /= kRateUpDateSize;
        rateRtp /= kRateUpDateSize;
        if (rateRtp > 0) {
            /*
            * Video coders are strange animals.. In certain cases the average bitrate is
            * consistently lower or higher than the target bitare. This additonal scaling compensates
            * for this anomaly.
            */
            const float alpha = 0.05f;
            targetRateScale *= (1.0f - alpha);
            targetRateScale += alpha*targetBitrate / rateRtp;
            targetRateScale = std::min(1.25f, std::max(0.8f, targetRateScale));
        }
    }

    bytesTransmitted = 0;
    bytesAcked = 0;
    bytesRtp = 0;
    bytesLost = 0;
    lastRateUpdateT_us = time_us;
}

/*
* Get the estimated maximum media rate
*/
float ScreamTx::Stream::getMaxRate() {
    return std::max(rateTransmitted, rateAcked);
}

/*
* The the stream that matches SSRC
*/
ScreamTx::Stream* ScreamTx::getStream(uint32_t ssrc) {
    for (int n = 0; n < nStreams; n++) {
        if (streams[n]->isMatch(ssrc)) {
            return streams[n];
        }
    }
    return NULL;
}

/*
* Get the target bitrate.
* This function returns a value -1 if loss of RTP packets is detected,
*  either because of loss in network or RTP queue discard
*/
float ScreamTx::Stream::getTargetBitrate() {

    bool requestRefresh = isRtpQueueDiscard() || repairLoss;
    repairLoss = false;
    if (requestRefresh  && !wasRepairLoss) {
        wasRepairLoss = true;
        return -1.0;
    }
    float rate = targetRateScale*targetBitrate;
    /*
    * Video coders are strange animals.. In certain cases a very frequent rate requests can confuse the
    * rate control logic in the coder
    */
    wasRepairLoss = false;
    return rate;
}


/*
* A small history of past max bitrates is maintained and the max value is picked.
* This solves a problem where consequtive rate decreases can give too low
*  targetBitrateI values.
*/
void ScreamTx::Stream::updateTargetBitrateI(float br) {
    targetBitrateHist[targetBitrateHistPtr] = std::min(br, targetBitrate);
    targetBitrateHistPtr = (targetBitrateHistPtr + 1) % kTargetBitrateHistSize;
    targetBitrateI = std::min(br, targetBitrate);
    for (int n = 0; n < kTargetBitrateHistSize; n++) {
        targetBitrateI = std::max(targetBitrateI, targetBitrateHist[n]);
    }

}

/*
* Update the target bitrate, the target bitrate includes the RTP overhead
*/
void ScreamTx::Stream::updateTargetBitrate(uint64_t time_us) {
    /*
    * Compute a maximum bitrate, this bitrates includes the RTP overhead
    */
    float br = getMaxRate();
    float rateRtpLimit = br;
    if (initTime_us == 0) {
        /*
        * Initialize if the first time
        */
        initTime_us = time_us;
        lastRtpQueueDiscardT_us = time_us;
    }

    if (lastBitrateAdjustT_us == 0) lastBitrateAdjustT_us = time_us;
    isActive = true;
    lastFrameT_us = time_us;

    if (lossEventFlag) {
        /*
        * Loss event handling
        * Rate is reduced slightly to avoid that more frames than necessary
        * queue up in the sender queue
        */
        lossEventFlag = false;
        if (time_us - lastTargetBitrateIUpdateT_us > 2000000) {
            /*
            * The timing constraint avoids that targetBitrateI
            *  is set too low in cases where a congestion event is prolonged.
            * An accurate targetBitrateI is not of extreme importance
            *  but helps to avoid jitter spikes when SCReAM operates
            *  over fixed bandwidth or slowly varying links.
            */
            updateTargetBitrateI(br);
            lastTargetBitrateIUpdateT_us = time_us;
        }
        targetBitrate = std::max(minBitrate,
            targetBitrate*lossEventRateScale);
        lastBitrateAdjustT_us = time_us;
    }
    else {
        if (time_us - lastBitrateAdjustT_us < kRateAdjustInterval_us)
            return;
        /*
        * A scale factor that is dependent on the inflection point
        * i.e the last known highest video bitrate
        */
        float sclI = (targetBitrate - targetBitrateI) / targetBitrateI;
        sclI *= 4;
        sclI = std::max(0.2f, std::min(1.0f, sclI*sclI));
        float increment = 0.0f;

        /*
        * Size of RTP queue [bits]
        * As this function is called immediately after a
        * video frame is produced, we need to accept the new
        * RTP packets in the queue, we subtract a number of bytes correspoding to the size
        * of the last frame (including RTP overhead), this is simply the aggregated size
        * of the RTP packets with the highest RTP timestamp
        */
        int lastBytes = rtpQueue->getSizeOfLastFrame();
        int txSizeBits = std::max(0, rtpQueue->bytesInQueue() - lastBytes) * 8;

        float alpha = 0.5f;

        txSizeBitsAvg = txSizeBitsAvg*alpha + txSizeBits*(1.0f - alpha);
        /*
        * tmp is a local scaling factor that makes rate adaptation sligthly more
        * aggressive when competing flows (e.g file transfers) are detected
        */
        float rampUpSpeedTmp = std::min(rampUpSpeed, targetBitrate*0.5f);
        if (parent->isCompetingFlows()) {
            rampUpSpeedTmp *= 2.0f;
        }

        if (rtpQueue->getDelay(time_us / 1e6f) > maxRtpQueueDelay &&
            (time_us - lastRtpQueueDiscardT_us > kMinRtpQueueDiscardInterval_us)) {
            /*
            * RTP queue is cleared as it is becoming too large,
            * Function is however disabled initially as there is no reliable estimate of the
            * throughput in the initial phase.
            */
            rtpQueue->clear();
            rtpQueueDiscard = true;
            lastRtpQueueDiscardT_us = time_us;
            targetRateScale = 1.0;
            txSizeBitsAvg = 0.0f;
        }
        else if (parent->inFastStart && txSizeBits / std::max(br, targetBitrate) < 0.1f) {
            /*
            * Increment bitrate, limited by the rampUpSpeed
            */
            increment = rampUpSpeedTmp*(kRateAdjustInterval_us / 1e6f);
            /*
            * Limit increase rate near the last known highest bitrate or if priority is low
            */
            increment *= sclI*sqrt(targetPriority);
            /*
            * No increase if the actual coder rate is lower than the target
            */
            if (targetBitrate > rateRtpLimit*1.5f)
                increment = 0;
            /*
            * Add increment
            */
            targetBitrate += increment;
            wasFastStart = true;
        }
        else {
            if (wasFastStart) {
                wasFastStart = false;
                if (time_us - lastTargetBitrateIUpdateT_us > 2000000) {
                    /*
                    * The timing constraint avoids that targetBitrateI
                    * is set too low in cases where a
                    * congestion event is prolonged
                    */
                    updateTargetBitrateI(br);
                    lastTargetBitrateIUpdateT_us = time_us;
                }
            }
            /*
            * scl is based on the queue delay trend
            */
            float scl = queueDelayGuard*parent->getQueueDelayTrend();
            if (parent->isCompetingFlows())
                scl *= 0.05f;

            /*
            * Update target rate
            * At very low bitrates it is necessary to actively try to push the
            *  the bitrate up some extra
            */
            float incrementScale = 1.0f + 0.05f*std::min(1.0f, 50000.0f / targetBitrate);

            float increment = incrementScale*br*(1.0f - scl) -
                txQueueSizeFactor*txSizeBitsAvg - targetBitrate;
            if (txSizeBits > 12000 && increment > 0)
                increment = 0;

            if (increment > 0) {
                wasFastStart = true;
                if (!parent->isCompetingFlows()) {
                    /*
                    * Limit the bitrate increase so that it does not go faster than rampUpSpeedTmp
                    * This limitation is not in effect if competing flows are detected
                    */
                    increment *= sclI;
                    increment = std::min(increment, (float)(rampUpSpeedTmp*(kRateAdjustInterval_us / 1e6)));
                }
                if (targetBitrate > rateRtpLimit*1.5f)
                    increment = 0;
            }
            else {
                /*
                * Avoid that the target bitrate is reduced if it actually is the media
                * coder that limits the output rate e.g due to inactivity
                */
                if (rateRtp < targetBitrate*0.8f)
                    increment = 0.0f;
                /*
                * Also avoid that the target bitrate is reduced if
                * the coder bitrate is higher
                * than the target.
                * The possible reason is that a large I frame is transmitted, another reason is
                * complex dynamic content.
                */
                if (rateRtp > targetBitrate*2.0f)
                    increment = 0.0f;
            }
            targetBitrate += increment;
        }
        lastBitrateAdjustT_us = time_us;
    }

    targetBitrate = std::min(maxBitrate, std::max(minBitrate, targetBitrate));
}

bool ScreamTx::Stream::isRtpQueueDiscard() {
    bool tmp = rtpQueueDiscard;
    rtpQueueDiscard = false;
    return tmp;
}

/*
* Adjust (enforce) proper prioritization between active streams
* at regular intervals. This is a necessary addon to mitigate
* issues that VBR media brings
*/
void ScreamTx::adjustPriorities(uint64_t time_us) {
    if (nStreams == 1 || time_us - lastAdjustPrioritiesT_us < 1000000) {
        /*
        * Skip if only one stream or if adjustment done less than 1s ago
        */
        return;
    }
    lastAdjustPrioritiesT_us = time_us;
    float br = 0.0f;
    float tPrioSum = 0.0f;
    for (int n = 0; n < nStreams; n++) {
        if (streams[n]->isActive) {
            if (streams[n]->targetBitrate > 0.7*streams[n]->maxBitrate ||
                streams[n]->rateRtp < streams[n]->targetBitrate*0.2) {
                /*
                * Don't adjust prioritites if atleast one stream runs at its
                * highest target bitrate or if atleast one stream is idle
                */
                return;
            }
            br += streams[n]->getMaxRate();
            tPrioSum += streams[n]->targetPriority;
        }
    }
    /*
    * Force down the target bitrate for streams that consume an unduly
    * amount of the bandwidth, given the priority distribution
    */
    for (int n = 0; n < nStreams; n++) {
        if (streams[n]->isActive) {
            float brShare = br*streams[n]->targetPriority / tPrioSum;
            float diff = (streams[n]->getMaxRate() - brShare);
            if (diff > 0) {
                float newBitrate = std::max(streams[n]->minBitrate, streams[n]->targetBitrate - diff);
                streams[n]->targetBitrate = std::max(streams[n]->targetBitrate*0.8f, newBitrate);
            }
        }
    }
}

/*
* Get the prioritized stream
*/
ScreamTx::Stream* ScreamTx::getPrioritizedStream(uint64_t time_us) {
    /*
    * Function that prioritizes between streams, this function may need
    * to be modified to handle the prioritization better for e.g
    * FEC, SVC etc.
    */
    if (nStreams == 1)
        /*
        * Skip if only one stream to save CPU
        */
        return streams[0];

    int maxCredit = 1;
    Stream *stream = NULL;
    /*
    * Pick a stream with credit higher or equal to
    * the size of the next RTP packet in queue for the given stream.
    */
    for (int n = 0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        if (tmp->rtpQueue->sizeOfQueue() == 0) {
            /*
            * Queue empty
            */
        }
        else {
            /*
            * Pick stream if it has the highest credit so far
            */
            if (tmp->credit >= std::max(maxCredit, tmp->rtpQueue->sizeOfNextRtp())) {
                stream = tmp;
                maxCredit = tmp->credit;
            }
        }
    }
    if (stream != NULL) {
        return stream;
    }
    /*
    * If the above doesn't give a candidate..
    * Pick the stream with the highest priority that also
    * has at least one RTP packet in queue.
    */
    float maxPrio = 0.0;
    for (int n = 0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        float priority = tmp->targetPriority;
        if (tmp->rtpQueue->sizeOfQueue() > 0 && priority > maxPrio) {
            maxPrio = priority;
            stream = tmp;
        }
    }
    return stream;
}

/*
* Add credit to streams that was not served
*/
void ScreamTx::addCredit(uint64_t time_us, Stream* servedStream, int transmittedBytes) {
    /*
    * Add a credit to stream(s) that did not get priority to transmit RTP packets
    */
    if (nStreams == 1)
        /*
        * Skip if only one stream to save CPU
        */
        return;
    for (int n = 0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        if (tmp != servedStream) {
            int credit = (int)(transmittedBytes*tmp->targetPriority * servedStream->targetPriorityInv);
            if (tmp->rtpQueue->sizeOfQueue() > 0) {
                tmp->credit += credit;
            }
            else {
                tmp->credit = std::min(10 * mss, tmp->credit + credit);
            }
        }
    }
}

/*
* Subtract credit from served stream
*/
void ScreamTx::subtractCredit(uint64_t time_us, Stream* servedStream, int transmittedBytes) {
    /*
    * Subtract a credit equal to the number of transmitted bytes from the stream that
    * transmitted a packet
    */
    if (nStreams == 1)
        /*
        * Skip if only one stream to save CPU
        */
        return;
    servedStream->credit = std::max(0, servedStream->credit - transmittedBytes);
}

/*
* Update the  congestion window
*/
void ScreamTx::updateCwnd(uint64_t time_us) {
    float dT = 0.001;
    if (lastCwndUpdateT_us == 0)
        lastCwndUpdateT_us = time_us;
    else
        dT = (time_us - lastCwndUpdateT_us) / 1e6f;

    /*
    * This adds a limit to how much the CWND can grow, it is particularly useful
    * in case of short gliches in the transmission, in which a large chunk of data is delivered
    * in a burst with the effect that the estimated queue delay is low because it typically depict the queue
    * delay for the last non-delayed RTP packet. The rule is that the CWND cannot grow faster
    * than the 1.25 times the average amount of bytes that transmitted in the given feedback interval
    */
    float bytesNewlyAckedLimited = bytesNewlyAcked;
    if (maxRate > 1.0e5f)
        bytesNewlyAckedLimited = std::min(bytesNewlyAckedLimited, 1.25f*maxRate*dT / 8.0f);
    else
        bytesNewlyAckedLimited = 2.0f*mss;

    /*
    * Compute the queue delay
    */
    uint64_t tmp = estimateOwd(time_us) - getBaseOwd();
    /*
    * Convert from [jiffy] OWD to an OWD in [s]
    */
    queueDelay = tmp / kTimestampRate;

    queueDelayMin = std::min(queueDelayMin, queueDelay);

    queueDelayMax = std::max(queueDelayMax, queueDelay);

    if (queueDelayMinAvg > 0.25f*queueDelayTarget && time_us - baseOwdResetT_us > 20000000) {
        /*
        * The base OWD is likely wrong, for instance due to
        * a channel change or clock drift, reset base OWD history
        */
        queueDelayMinAvg = 0.0f;
        queueDelay = 0.0f;
        for (int n = 0; n < kBaseOwdHistSize; n++)
            baseOwdHist[n] = UINT32_MAX;
        baseOwd = UINT32_MAX;
        baseOwdResetT_us = time_us;
    }
    /*
    * An averaged version of the queue delay fraction
    * neceassary in order to make video rate control robust
    * against jitter
    */
    queueDelayFractionAvg = 0.9f*queueDelayFractionAvg + 0.1f*getQueueDelayFraction();

    /*
    * Less frequent updates here
    * + Compute pacing interval
    * + Save to queue delay fraction history
    *   used in computeQueueDelayTrend()
    * + Update queueDelayTarget
    */
    if ((time_us - lastAddToQueueDelayFractionHistT_us) >
        kQueueDelayFractionHistInterval_us) {

        /*
        * compute paceInterval, we assume a min bw of 50kbps and a min tp of 1ms
        * for stable operation
        * this function implements the packet pacing
        */
        paceInterval = kMinPaceInterval;
        if (queueDelayFractionAvg > 0.1f && kEnablePacketPacing) {
            float pacingBitrate = kPacketPacingHeadRoom*std::max(kMinimumBandwidth, cwnd * 8.0f / std::max(0.001f, getSRtt()));
            float tp = (mss * 8.0f) / pacingBitrate;
            paceInterval = std::max(kMinPaceInterval, tp);
        }
        paceInterval_us = (uint64_t)(paceInterval * 1000000);

        if (queueDelayMin < queueDelayMinAvg)
            queueDelayMinAvg = queueDelayMin;
        else 
            queueDelayMinAvg = 0.001f*queueDelayMin + 0.999f*queueDelayMinAvg;
        queueDelayMin = 1000.0f;
        /*
        * Need to duplicate insertion incase the feedback is sparse
        */
        int nIter = (int)(time_us - lastAddToQueueDelayFractionHistT_us) / kQueueDelayFractionHistInterval_us;
        for (int n = 0; n < nIter; n++) {
            queueDelayFractionHist[queueDelayFractionHistPtr] = getQueueDelayFraction();
            queueDelayFractionHistPtr = (queueDelayFractionHistPtr + 1) % kQueueDelayFractionHistSize;
        }
        computeQueueDelayTrend();

        queueDelayTrendMem = std::max(queueDelayTrendMem*0.98f, queueDelayTrend);

        if (enableSbd) {
            /*
            * Shared bottleneck detection,
            */
            float queueDelayNorm = queueDelay / queueDelayTargetMin;
            queueDelayNormHist[queueDelayNormHistPtr] = queueDelayNorm;
            queueDelayNormHistPtr = (queueDelayNormHistPtr + 1) % kQueueDelayNormHistSize;
            /*
            * Compute shared bottleneck detection and update queue delay target
            * if queue dela variance is sufficienctly low
            */
            computeSbd();
            /*
            * This function avoids the adjustment of queueDelayTarget when
            * congestion occurs (indicated by high queueDelaydSbdVar and queueDelaySbdSkew)
            */
            float oh = queueDelayTargetMin*(queueDelaySbdMeanSh + sqrt(queueDelaySbdVar));
            if (lossEventRate > 0.002 && queueDelaySbdMeanSh > 0.5 && queueDelaySbdVar < 0.2) {
                queueDelayTarget = std::min(kQueueDelayTargetMax, oh*1.5f);
            }
            else {
                if (queueDelaySbdVar < 0.2 && queueDelaySbdSkew < 0.05) {
                    queueDelayTarget = std::max(queueDelayTargetMin, std::min(kQueueDelayTargetMax, oh));
                }
                else {
                    if (oh < queueDelayTarget)
                        queueDelayTarget = std::max(queueDelayTargetMin, std::max(queueDelayTarget*0.99f, oh));
                    else
                        queueDelayTarget = std::max(queueDelayTargetMin, queueDelayTarget*0.999f);
                }
            }
        }
        lastAddToQueueDelayFractionHistT_us = time_us;
    }
    /*
    * offTarget is a normalized deviation from the queue delay target
    */
    float offTarget = (queueDelayTarget - queueDelay) / float(queueDelayTarget);

    if (lossEvent) {
        /*
        * loss event detected, decrease congestion window
        */
        cwnd = std::max(cwndMin, (int)(lossBeta*cwnd));
        lossEvent = false;
        lastCongestionDetectedT_us = time_us;

        inFastStart = false;
        wasLossEvent = true;
    }
    else {

        if (time_us - lastRttT_us > getSRtt()*1e6) {
            if (wasLossEvent)
                lossEventRate = 0.99f*lossEventRate + 0.01f;
            else
                lossEventRate *= 0.99f;
            wasLossEvent = false;
            lastRttT_us = time_us;
        }

        if (inFastStart) {
            /*
            * In fast start
            */
            if (queueDelayTrend < 0.2) {
                /*
                * CWND is increased by the number of ACKed bytes if
                * window is used to 1/1.5 = 67%
                * We need to relax the rule a bit for the case that
                * feedback may be sparse due to limited RTCP report interval
                * In addition we put a limit for the cases where feedback becomes
                * piled up (sometimes happens with e.g LTE)
                */
                if (bytesInFlight*1.5 + bytesNewlyAcked > cwnd) {
                    cwnd += bytesNewlyAckedLimited;
                }
            }
            else {
                inFastStart = false;
            }
        }
        else {
            if (offTarget > 0.0f) {
                /*
                * queue delay below target, increase CWND if window
                * is used to 1/1.2 = 83%
                */
                if (bytesInFlight*1.2 + bytesNewlyAcked > cwnd) {
                    float increment = gainUp * offTarget * bytesNewlyAckedLimited * mss / cwnd;
                    cwnd += (int)(increment + 0.5f);
                }
            }
            else {
                /*
                * Queue delay above target.
                * Limit the CWND reduction to at most a quarter window
                *  this avoids unduly large reductions for the cases
                *  where data is queued up e.g because of retransmissions
                *  on lower protocol layers.
                */
                float delta = -(gainDown * offTarget * bytesNewlyAcked * mss / cwnd);
                delta = std::min((int)delta, cwnd / 4);
                cwnd -= (int)(delta);

                /*
                * Especially when running over low bitrate bottlenecks, it is
                *  beneficial to reduce the target bitrate a little, it limits
                *  possible RTP queue delay spikes when congestion window is reduced
                */
                float rateTotal = 0.0f;
                for (int n = 0; n < nStreams; n++)
                    rateTotal += streams[n]->getMaxRate();
                if (rateTotal < 1.0e5f) {
                    delta = delta / cwnd;
                    float rateAdjustFactor = (1.0f - delta);
                    for (int n = 0; n < nStreams; n++) {
                        Stream *tmp = streams[n];
                        tmp->targetBitrate = std::max(tmp->minBitrate,
                            std::min(tmp->maxBitrate,
                            tmp->targetBitrate*rateAdjustFactor));
                    }
                }
                lastCongestionDetectedT_us = time_us;

            }
        }
    }
    /*
    * Congestion window validation, checks that the congestion window is
    * not considerably higher than the actual number of bytes in flight
    */
    int maxBytesInFlightHi = (int)(std::max(bytesInFlightMaxHi, getMaxBytesInFlightHi()));
    int maxBytesInFlightLo = (int)(std::max(bytesInFlight, getMaxBytesInFlightLo()));

    float maxBytesInFlight = //maxBytesInFlightHi*kMaxBytesInFlightHeadRoom;
        (maxBytesInFlightHi*(1.0f - queueDelayTrend) + maxBytesInFlightLo*queueDelayTrend)*
        kMaxBytesInFlightHeadRoom;
    if (maxBytesInFlight > 5000) {
        cwnd = std::min(cwnd, (int)maxBytesInFlight);
    }

    if (getSRtt() < 0.01f && queueDelayTrend < 0.1) {
        int tmp = int(rateTransmitted*0.01f / 8);
        tmp = std::max(tmp, (int)(maxBytesInFlight*1.5f));
        cwnd = std::max(cwnd, tmp);
    }
    cwnd = std::max(cwndMin, cwnd);

    /*
    * Make possible to enter fast start if OWD has been low for a while
    */
    if (queueDelayTrend > 0.2) {
        lastCongestionDetectedT_us = time_us;
    }
    else if (time_us - lastCongestionDetectedT_us > 5000000 &&
        !inFastStart && kEnableConsecutiveFastStart) {
        /*
        * The queue delay trend has been low for more than 5.0s, resume fast start
        */
        inFastStart = true;
        lastCongestionDetectedT_us = time_us;
    }
    bytesNewlyAcked = 0;

    lastCwndUpdateT_us = time_us;
}

/*
* Update base OWD (if needed) and return the
* last estimated OWD (without offset compensation)
*/
uint32_t ScreamTx::estimateOwd(uint64_t time_us) {
    baseOwd = std::min(baseOwd, ackedOwd);
    if (time_us - lastBaseOwdAddT_us >= kBaseDelayUpdateInterval_us) {
        baseOwdHist[baseOwdHistPtr] = baseOwd;
        baseOwdHistPtr = (baseOwdHistPtr + 1) % kBaseOwdHistSize;
        lastBaseOwdAddT_us = time_us;
        baseOwd = UINT32_MAX;
        /*
        * _Very_ long periods of congestion can cause the base delay to increase
        * with the effect that the queue delay is estimated wrong, therefore we seek to
        * refresh the whole thing by deliberately allowing the network queue to drain
        */
        if (time_us - lastBaseDelayRefreshT_us > kBaseDelayUpdateInterval_us*(kBaseOwdHistSize - 1)) {
            lastBaseDelayRefreshT_us = time_us;
        }
    }
    return ackedOwd;
}

/*
* Get the base one way delay
*/
uint32_t ScreamTx::getBaseOwd() {
    uint32_t ret = baseOwd;
    for (int n = 0; n < kBaseOwdHistSize; n++)
        ret = std::min(ret, baseOwdHist[n]);
    return ret;
}

/*
* Compute current number of bytes in flight
*/
void ScreamTx::computeBytesInFlight() {
    bytesInFlight = 0;
    for (int m = 0; m < nStreams; m++) {
        Transmitted *txPackets = streams[m]->txPackets;
        for (int n = 0; n < kMaxTxPackets; n++) {
            if (txPackets[n].isUsed)
                bytesInFlight += txPackets[n].size;
        }
    }
}

/*
* Get std::max bytes in flight over a time window
*/
int ScreamTx::getMaxBytesInFlightLo() {
    /*
    * All elements in the buffer must be initialized before
    * return value > 0
    */
    if (bytesInFlightHistLo[bytesInFlightHistPtr] == 0)
        return 0;
    int ret = 0;
    for (int n = 0; n < kBytesInFlightHistSize; n++) {
        ret = std::max(ret, bytesInFlightHistLo[n]);
    }
    return ret;
}
int ScreamTx::getMaxBytesInFlightHi() {
    /*
    * All elements in the buffer must be initialized before
    * return value > 0
    */
    if (bytesInFlightHistHi[bytesInFlightHistPtr] == 0)
        return 0;
    int ret = 0;
    for (int n = 0; n < kBytesInFlightHistSize; n++) {
        ret = std::max(ret, bytesInFlightHistHi[n]);
    }
    return ret;
}

/*
* Get the queue delay fraction
*/
float ScreamTx::getQueueDelayFraction() {
    return queueDelay / queueDelayTarget;
}

/*
* Compute congestion indicator
*/
void ScreamTx::computeQueueDelayTrend() {
    queueDelayTrend = 0.0;
    int ptr = queueDelayFractionHistPtr;
    float avg = 0.0f, x1, x2, a0, a1;

    for (int n = 0; n < kQueueDelayFractionHistSize; n++) {
        avg += queueDelayFractionHist[ptr];
        ptr = (ptr + 1) % kQueueDelayFractionHistSize;
    }
    avg /= kQueueDelayFractionHistSize;

    ptr = queueDelayFractionHistPtr;
    x2 = 0.0f;
    a0 = 0.0f;
    a1 = 0.0f;
    for (int n = 0; n < kQueueDelayFractionHistSize; n++) {
        x1 = queueDelayFractionHist[ptr] - avg;
        a0 += x1 * x1;
        a1 += x1 * x2;
        x2 = x1;
        ptr = (ptr + 1) % kQueueDelayFractionHistSize;
    }
    if (a0 > 0) {
        queueDelayTrend = std::max(0.0f, std::min(1.0f, (a1 / a0)*queueDelayFractionAvg));
    }
}

/*
* Compute indicators of shared bottleneck
*/
void ScreamTx::computeSbd() {
    float queueDelayNorm, tmp;
    queueDelaySbdMean = 0.0;
    queueDelaySbdMeanSh = 0.0;
    queueDelaySbdVar = 0.0;
    int ptr = queueDelayNormHistPtr;
    for (int n = 0; n < kQueueDelayNormHistSize; n++) {
        queueDelayNorm = queueDelayNormHist[ptr];
        queueDelaySbdMean += queueDelayNorm;
        if (n >= kQueueDelayNormHistSize - kQueueDelayNormHistSizeSh) {
            queueDelaySbdMeanSh += queueDelayNorm;
        }
        ptr = (ptr + 1) % kQueueDelayNormHistSize;
    }
    queueDelaySbdMean /= kQueueDelayNormHistSize;
    queueDelaySbdMeanSh /= kQueueDelayNormHistSizeSh;

    ptr = queueDelayNormHistPtr;
    for (int n = 0; n < kQueueDelayNormHistSize; n++) {
        queueDelayNorm = queueDelayNormHist[ptr];
        tmp = queueDelayNorm - queueDelaySbdMean;
        queueDelaySbdVar += tmp * tmp;
        queueDelaySbdSkew += tmp * tmp * tmp;
        ptr = (ptr + 1) % kQueueDelayNormHistSize;
    }
    queueDelaySbdVar /= kQueueDelayNormHistSize;
    queueDelaySbdSkew /= kQueueDelayNormHistSize;
}

/*
* true if the queueDelayTarget is increased due to
* detected competing flows
*/
bool ScreamTx::isCompetingFlows() {
    return queueDelayTarget > queueDelayTargetMin;
}

/*
* Get queue delay trend
*/
float ScreamTx::getQueueDelayTrend() {
    return queueDelayTrend;
}
