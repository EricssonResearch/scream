#include "RtpQueue.h"
#include "ScreamTx.h"
#include "ScreamRx.h"
#ifdef _MSC_VER
#define NOMINMAX
#include <winSock2.h>
#else
#include <arpa/inet.h>
#endif
#include <cstdint>
#include <cmath>
#include <string.h>
#include <iostream>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>


//using namespace std;

// === Some good to have features, SCReAM works also
//     with these disabled
// Fast start can resume if little or no congestion detected
static const bool kEnableConsecutiveFastStart = true;
// Packet pacing reduces jitter
static const bool kEnablePacketPacing = true;
static const float kPacketPacingHeadRoom = 1.25f;

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
    float ecnCeBeta_,
    float queueDelayTargetMin_,
    bool enableSbd_,
    float gainUp_,
    float gainDown_,
    int cwnd_,
    float cautiousPacing_,
    int bytesInFlightHistSize_,
    bool openWindow_
    )
    : sRttSh_us(0),
    lossBeta(lossBeta_),
    ecnCeBeta(ecnCeBeta_),
    queueDelayTargetMin(queueDelayTargetMin_),
    enableSbd(enableSbd_),
    gainUp(gainUp_),
    gainDown(gainDown_),
    cautiousPacing(cautiousPacing_),
    bytesInFlightHistSize(bytesInFlightHistSize_),
    openWindow(openWindow_),
    sRtt_us(0),
    sRtt(0.0f),
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
    bytesInFlightHistLoMem(0),
    bytesInFlightHistHiMem(0),
    maxBytesInFlight(0.0f),

    lossEvent(false),
    wasLossEvent(false),
    lossEventRate(0.0),
    ecnCeEvent(false),

    inFastStart(true),

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
    rateTransmitted(0.0f),
    rateAcked(0.0f),
    queueDelayTrendMem(0.0f),
    lastCwndUpdateT_us(0),
    bytesInFlight(0),
    lastBaseDelayRefreshT_us(0),
    maxRate(0.0f),
    baseOwdHistMin(UINT32_MAX)
{
    if (cwnd_ == 0) {
        cwnd = kInitMss * 2;
    }
    else {
        cwnd = cwnd_;
    }
    if (openWindow) {
        cwndMin = 10000000;
        cwnd = cwndMin;
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
    for (int n = 0; n < kBytesInFlightHistSizeMax; n++) {
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

    statistics = new Statistics();
}

ScreamTx::~ScreamTx() {
    for (int n = 0; n < nStreams; n++)
        delete streams[n];
    delete statistics;
}

ScreamTx::Statistics::Statistics() {
    avgRateTx = 0.0f;
    avgRtt = 0.0f;
    avgQueueDelay = 0.0f;
    rateLostAcc = 0.0f;
    rateLostN = 0;
    for (int n = 0; n < kLossRateHistSize; n++) {
        lossRateHist[n] = 0.0f;
    }
    lossRateHistPtr = 0;
}

void ScreamTx::Statistics::add(float rateTx, float rateLost, float rtt, float queueDelay) {
    const float alpha = 0.98f;
    if (avgRateTx == 0.0f) {
        avgRateTx = rateTx;
        avgRtt = rtt;
        avgQueueDelay = queueDelay;
    }
    else {
        avgRateTx = alpha*avgRateTx + (1.0f - alpha)*rateTx;
        rateLostAcc += rateLost;
        rateLostN++;
        if (rateLostN == 10) {
            rateLostAcc /= 10;
            rateLostN = 0;
            float lossRate = 0.0f;
            if (rateTx > 0)
                lossRate = rateLostAcc / rateTx*100.0f;
            lossRateHist[lossRateHistPtr] = lossRate;
            lossRateHistPtr = (lossRateHistPtr + 1) % kLossRateHistSize;
        }
        avgRtt = alpha*avgRtt + (1.0f - alpha)*rtt;
        avgQueueDelay = alpha*avgQueueDelay + (1.0f - alpha)*queueDelay;
    }
}

void ScreamTx::Statistics::getSummary(float time, char s[]) {
    float lossRate = 0.0f;
    for (int n = 0; n < kLossRateHistSize; n++)
        lossRate += lossRateHist[n];
    lossRate /= kLossRateHistSize;

    sprintf(s, "%5.1f  Transmit rate = %5.0fkbps, PLR = %5.2f%%, RTT = %5.3fs, Queue delay = %5.3fs",
        time,
        avgRateTx / 1000.0f,
        lossRate,
        avgRtt,
        avgQueueDelay);
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
    float lossEventRateScale,
    float ecnCeEventRateScale) {
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
        lossEventRateScale,
        ecnCeEventRateScale);
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
    if (time_us - lastCwndUpdateT_us < 500000) {
        /*
        * We expect feedback at least every 500ms
        * to update the target rate.
        */
        stream->updateTargetBitrate(time_us);
    }
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
        if (!openWindow)
            cwndMin = 2 * mss;
        cwnd = max(cwnd, cwndMin);
    }
}

float rateTransmittedAvg = 0.0f;
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

    if (rateAcked < 50000.0f) {
        tmp *= 2;
    }

    if (time_us - lastRateUpdateT_us > tmp) {
        rateTransmitted = 0.0;
        rateAcked = 0.0;
        for (int n = 0; n < nStreams; n++) {
            streams[n]->updateRate(time_us);
            rateTransmitted += streams[n]->rateTransmitted;
            rateTransmittedAvg = 0.8*rateTransmittedAvg + 0.2*rateTransmitted;
            rateAcked += streams[n]->rateAcked;
            if (n == 0)
                statistics->add(streams[0]->rateTransmitted, streams[0]->rateLost, sRtt, queueDelay);
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

    bytesInFlightMaxHi = std::max(bytesInFlight, bytesInFlightMaxHi);

    /*
    * Update bytes in flight history for congestion window validation
    */
    if (time_us - lastBytesInFlightT_us > kBytesInFlightHistInterval_us) {
        bytesInFlightMaxLo = 0;
        if (nAccBytesInFlightMax > 0) {
            bytesInFlightMaxLo = accBytesInFlightMax / nAccBytesInFlightMax;
        }
        bytesInFlightHistLo[bytesInFlightHistPtr] = bytesInFlightMaxLo;
        bytesInFlightHistHi[bytesInFlightHistPtr] = bytesInFlightMaxHi;
        bytesInFlightHistPtr = (bytesInFlightHistPtr + 1) % bytesInFlightHistSize;
        lastBytesInFlightT_us = time_us;
        accBytesInFlightMax = 0;
        nAccBytesInFlightMax = 0;
        bytesInFlightMaxHi = 0;
        bytesInFlightHistLoMem = 0;
        bytesInFlightHistHiMem = 0;
        for (int n = 0; n < bytesInFlightHistSize; n++) {
            bytesInFlightHistLoMem = std::max(bytesInFlightHistLoMem, bytesInFlightHistLo[n]);
            bytesInFlightHistHiMem = std::max(bytesInFlightHistHiMem, bytesInFlightHistHi[n]);
        }

        /*
        * In addition, reset MSS, this is useful in case for instance
        * a video stream is put on hold, leaving only audio packets to be
        * transmitted
        */
        mss = kInitMss;
        if (!openWindow)
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
    * Enforce packet pacing
    */
    float retVal = 0.0f;
    if (kEnablePacketPacing && nextTransmitT_us > time_us){
        retVal = (nextTransmitT_us - time_us) * 1e-6f;
    }

    /*
    * A retransmission time out mechanism to avoid deadlock
    */
    if (time_us - lastTransmitT_us > 500000) { // 200ms
        for (int n = 0; n < kMaxTxPackets; n++) {
            stream->txPackets[n].isUsed = false;
        }
        bytesInFlight = 0;
        exit = false;
        retVal = 0.0f;
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

    int ix = seqNr % kMaxTxPackets;
    Transmitted *txPacket = &(stream->txPackets[ix]);
    if (txPacket->isUsed) {
        /*
        * This event should occur quite rarely.
        * The most likely reason is that thoughput has dropped
        * considerably.
        * Therefore we clean the list and set cwnd and bitrate low
        */
        for (int n = 0; n < kMaxTxPackets; n++) {
            stream->txPackets[n].isUsed = false;
        }
        bytesInFlight = 0;//-= txPacket->size;
        cwnd = cwndMin;
        stream->targetBitrate = stream->minBitrate;
    }
    stream->hiSeqTx = seqNr;
    txPacket->timestamp = (uint32_t)(time_us / 1000);
    txPacket->timeTx_us = time_us;
    txPacket->size = size;
    txPacket->seqNr = seqNr;
    txPacket->isUsed = true;
    txPacket->isAcked = false;
    txPacket->isAfterReceivedEdge = false;
    bytesInFlight += size;

    /*
    * Update bytesInFlight
    */

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
    if (!openWindow)
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
* Parse feedback according to the format below. It is up to the
* wrapper application this RTCP from a compound RTCP if needed
* BT = 255, means that this is experimental use.
* The code currently only handles one SSRC source per IP packet
*
* 0                   1                   2                   3
* 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |V=2|P|reserved |   PT=XR=207   |           length=6            |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                              SSRC                             |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |     BT=255    |    reserved   |         block length=4        |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                        SSRC of source                         |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* | Highest recv. seq. nr. (16b)  |         ECN_CE_bytes          |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                     Ack vector (b0-31)                        |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                     Ack vector (b32-63)                       |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                    Timestamp (32bits)                         |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
void ScreamTx::incomingFeedback(uint64_t time_us,
    unsigned char* buf,
    int size) {
    if (!(size == 32 && buf[0] == 0x80 && buf[1] == 207 && buf[8] == 255)) {
        // This does not seem as a valid size, exit function
        return;
    }
    uint32_t tmp_l_1;
    uint16_t tmp_s;
    uint32_t tmp_l_2;
    uint32_t timeStamp;
    uint16_t seqNr;
    uint64_t ackVector;
    uint16_t ecnCeMarkedBytes;
    uint32_t ssrc;
    uint16_t rawSeq;
    memcpy(&tmp_l_1, buf + 12, 4);
    ssrc = ntohl(tmp_l_1);
    memcpy(&seqNr, buf + 16, 2);
    seqNr = ntohs(seqNr);
    memcpy(&ecnCeMarkedBytes, buf + 18, 2);
    ecnCeMarkedBytes = ntohs(ecnCeMarkedBytes);
    memcpy(&tmp_l_1, buf + 20, 4);
    memcpy(&tmp_l_2, buf + 24, 4);
    tmp_l_1 = ntohl(tmp_l_1);
    tmp_l_2 = ntohl(tmp_l_2);
    ackVector = ((uint64_t(tmp_l_1)) << 32) | tmp_l_2;
    memcpy(&timeStamp, buf + 28, 4);
    timeStamp = ntohl(timeStamp);
    incomingFeedback(time_us, ssrc, timeStamp, seqNr, ackVector, ecnCeMarkedBytes);
}
/*
* New incoming feedback
*/
void ScreamTx::incomingFeedback(uint64_t time_us,
    uint32_t ssrc,
    uint32_t timestamp,
    uint16_t highestSeqNr,
    uint64_t ackVector,
    uint16_t ecnCeMarkedBytes) {
    if (!isInitialized) initialize(time_us);
    Stream *stream = getStream(ssrc);
    if (stream == 0) {
        // Bogus RTCP?, the SSRC is wrong anyway, Skip
        return;
    }
    accBytesInFlightMax += bytesInFlight;
    nAccBytesInFlightMax++;
    Transmitted *txPackets = stream->txPackets;
    /*
    * Mark received packets, given by the ACK vector
    */
    markAcked(time_us, txPackets, highestSeqNr, ackVector, timestamp, stream);

    /*
    * Detect lost pakcets
    */
    detectLoss(time_us, txPackets, highestSeqNr, stream);

    if (ecnCeMarkedBytes != stream->ecnCeMarkedBytes && time_us - lastLossEventT_us > sRtt_us) {
        ecnCeEvent = true;
        lastLossEventT_us = time_us;
    }
    stream->ecnCeMarkedBytes = ecnCeMarkedBytes;

    if (lossEvent || ecnCeEvent) {
        lastLossEventT_us = time_us;
        for (int n = 0; n < nStreams; n++) {
            Stream *tmp = streams[n];
            if (lossEvent)
                tmp->lossEventFlag = true;
            else
                tmp->ecnCeEventFlag = true;
        }
    }

    if (lastCwndUpdateT_us == 0)
        lastCwndUpdateT_us = time_us;

    if (time_us - lastCwndUpdateT_us > 10000) {
        /*
        * There is no gain with a too frequent CWND update
        * An update every 10ms is fast enough even at very high high bitrates
        */
        updateCwnd(time_us);
        lastCwndUpdateT_us = time_us;
    }
}

/*
* Mark ACKed RTP packets
*/
void ScreamTx::markAcked(uint64_t time_us, struct Transmitted *txPackets, uint16_t highestSeqNr, uint64_t ackVector, uint32_t timestamp, Stream *stream) {
    /*
    * Loop only through the packets that are covered by the last highest ACK, this saves complexity
    */
    int ix1 = highestSeqNr; ix1 = ix1 % kMaxTxPackets;
    int ix0 = stream->hiSeqAck + 1;
    if (ix0 < 0) ix0 += kMaxTxPackets;
    while (ix1 < ix0)
        ix1 += kMaxTxPackets;
    for (int m = ix0; m <= ix1; m++) {
        int n = m % kMaxTxPackets;
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
                ackedOwd = timestamp - tmp->timestamp;
                /*
                * Compute the queue delay
                */
                uint32_t qDel = estimateOwd(time_us);
                qDel -= getBaseOwd();

                /*
                * Convert from [jiffy] OWD to an OWD in [s]
                */
                queueDelay = qDel / kTimestampRate;

                uint64_t rtt = time_us - tmp->timeTx_us;

                sRttSh_us = (7 * sRttSh_us + rtt) / 8;
                if (time_us - lastSRttUpdateT_us > sRttSh_us) {
                    sRtt_us = (7 * sRtt_us + sRttSh_us) / 8;
                    lastSRttUpdateT_us = time_us;
                    sRtt = sRtt_us*1e-6f;
                }
                stream->timeTxAck_us = tmp->timeTx_us;
            }
        }
    }
}

/*
* Detect lost RTP packets
*/
void ScreamTx::detectLoss(uint64_t time_us, struct Transmitted *txPackets, uint16_t highestSeqNr, Stream *stream) {
    /*
    * Loop only through the packets that are covered by the last highest ACK, this saves complexity
    * There is a faint possibility that we miss to detect large bursts of lost packets with this fix
    */
    int ix1 = highestSeqNr; ix1 = ix1 % kMaxTxPackets;
    int ix0 = stream->hiSeqAck + 1;//(ix1 - kAckVectorBits);
    stream->hiSeqAck = highestSeqNr;
    if (ix0 < 0) ix0 += kMaxTxPackets;
    while (ix1 < ix0)
        ix1 += kMaxTxPackets;

    /*
    * Mark packets outside the 64 bit ACK vector range as forever lost
    */
    if (stream->lastLossDetectIx >= 0) {
        int ix0_ = ix0;
        if (stream->lastLossDetectIx > ix0_) ix0_ += kMaxTxPackets;
        for (int m = stream->lastLossDetectIx; m < ix0_; m++) {
            int n = m % kMaxTxPackets;
            if (txPackets[n].isUsed) {
                Transmitted *tmp = &txPackets[n];
                if (time_us - lastLossEventT_us > sRtt_us && lossBeta < 1.0f) {
                    lossEvent = true;
                }
                stream->bytesLost += tmp->size;
                tmp->isUsed = false;
                stream->repairLoss = true;
            }
        }
    }
    stream->lastLossDetectIx = ix0;

    /*
    * Mark late packets as lost
    */
    for (int m = ix0; m <= ix1; m++) {
        int n = m % kMaxTxPackets;
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
                bytesInFlight -= tmp->size;
                if (bytesInFlight < 0)
                    bytesInFlight = 0;
                stream->bytesAcked += tmp->size;
                tmp->isAfterReceivedEdge = true;
            }

            if (tmp->timeTx_us + kReorderTime_us < stream->timeTxAck_us && !tmp->isAcked) {
                /*
                * Packet ACK is delayed more than kReorderTime_us after an ACK of a higher SN packet
                * raise a loss event and remove from TX list
                */
                if (time_us - lastLossEventT_us > sRtt_us && lossBeta < 1.0f) {
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

}


float ScreamTx::getTargetBitrate(uint32_t ssrc) {
    return  getStream(ssrc)->getTargetBitrate();
}

void ScreamTx::setTargetPriority(uint32_t ssrc, float priority) {
    getStream(ssrc)->targetPriority = priority;
    getStream(ssrc)->targetPriorityInv = 1.0f / priority;
}

void ScreamTx::getLog(float time, char *s) {
    int inFlightMax = std::max(bytesInFlight, bytesInFlightHistHiMem);
    sprintf(s, "%4.3f, %4.3f, %4.3f, %4.3f, %6d, %6d, %5.0f, %1d, ",
        queueDelay, queueDelayMax, queueDelayMinAvg, sRtt,
        cwnd, bytesInFlight, rateTransmitted / 1000.0f, isInFastStart());

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

void ScreamTx::getShortLog(float time, char *s) {
    int inFlightMax = std::max(bytesInFlight, bytesInFlightHistHiMem);
    sprintf(s, "%4.3f, %4.3f, %6d, %6d, %5.0f, ",
        queueDelay, sRtt,
        cwnd, bytesInFlight, rateTransmitted / 1000.0f);

    queueDelayMax = 0.0;
    for (int n = 0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        char s2[200];
        sprintf(s2, "%4.3f, %5.0f, %5.0f, %5.0f, %5.0f, ",
            tmp->rtpQueue->getDelay(time),
            tmp->targetBitrate / 1000.0f, tmp->rateRtp / 1000.0f,
            tmp->rateTransmitted / 1000.0f,
            tmp->rateLost / 1000.0f);
        strcat(s, s2);
    }
}

void ScreamTx::getStatistics(float time, char *s) {
    statistics->getSummary(time, s);
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
    initTime_us = time_us;
}

/*
* Update the  congestion window
*/
void ScreamTx::updateCwnd(uint64_t time_us) {

    float dT = 0.001;
    if (lastCwndUpdateT_us == 0)
        lastCwndUpdateT_us = time_us;
    else
        dT = (time_us - lastCwndUpdateT_us) * 1e-6f;



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

    queueDelayMin = std::min(queueDelayMin, queueDelay);

    queueDelayMax = std::max(queueDelayMax, queueDelay);

    float time = time_us*1e-6;
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
        baseOwdHistMin = UINT32_MAX;
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
        if (queueDelayFractionAvg > 0.05f && kEnablePacketPacing) {
            /*
            * The cautiousPacing parameter restricts transmission of large key frames when the parameter is set to a value closer to 1.0
            */
            float pacingBitrate = (1.0 - cautiousPacing)*cwnd * 8.0f / std::max(0.001f, sRtt) + cautiousPacing*rateTransmittedAvg;
            pacingBitrate = kPacketPacingHeadRoom*std::max(kMinimumBandwidth, pacingBitrate);
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

        if (time_us - initTime_us > 2000000) {
            /*
            * Queue delay trend calculations are reliable after ~2s
            */
            computeQueueDelayTrend();
        }

        queueDelayTrendMem = std::max(queueDelayTrendMem*0.98f, queueDelayTrend);

        /*
        * Compute bytes in flight limitation
        */
        int maxBytesInFlightHi = (int)(std::max(bytesInFlightMaxHi, bytesInFlightHistHiMem));
        int maxBytesInFlightLo = (int)(std::max(bytesInFlight, bytesInFlightHistLoMem));

        float alpha = std::max(queueDelayTrend, cautiousPacing);
        maxBytesInFlight =
            (maxBytesInFlightHi*(1.0f - alpha) + maxBytesInFlightLo*alpha)*
            kMaxBytesInFlightHeadRoom;

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
    else if (ecnCeEvent) {
        /*
        * loss event detected, decrease congestion window
        */
        cwnd = std::max(cwndMin, (int)(ecnCeBeta*cwnd));
        ecnCeEvent = false;
        lastCongestionDetectedT_us = time_us;

        inFastStart = false;
        wasLossEvent = true;
    }
    else {

        if (time_us - lastRttT_us > sRtt_us) {
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
    if (maxBytesInFlight > 5000) {
        cwnd = std::min(cwnd, (int)maxBytesInFlight);
    }

    if (sRtt < 0.01f && queueDelayTrend < 0.1) {
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
        baseOwdHistMin = UINT32_MAX;
        for (int n = 0; n < kBaseOwdHistSize; n++)
            baseOwdHistMin = std::min(baseOwdHistMin, baseOwdHist[n]);
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
    return  std::min(baseOwd, baseOwdHistMin);
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
    int maxCredit = 10 * mss;
    for (int n = 0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        if (tmp != servedStream) {
            int credit = (int)(transmittedBytes*tmp->targetPriority * servedStream->targetPriorityInv);
            if (tmp->rtpQueue->sizeOfQueue() > 0) {
                tmp->credit += credit;
            }
            else {
                tmp->credit += credit;
                if (tmp->credit > maxCredit) {
                    tmp->creditLost += tmp->credit - maxCredit;
                    tmp->credit = maxCredit;
                }
                //tmp->credit = std::min(10 * mss, tmp->credit + credit);
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
    float lossEventRateScale_,
    float ecnCeEventRateScale_) {
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
    ecnCeEventRateScale = ecnCeEventRateScale_;
    targetBitrateHistUpdateT_us = 0;
    targetBitrateI = 1.0f;
    credit = 0;
    creditLost = 0;
    bytesTransmitted = 0;
    bytesAcked = 0;
    bytesLost = 0;
    hiSeqAck = 0;
    hiSeqTx = 0;
    rateTransmitted = 0.0f;
    rateAcked = 0.0f;
    rateLost = 0.0f;
    lossEventFlag = false;
    ecnCeEventFlag = false;
    txSizeBitsAvg = 0.0f;
    lastRateUpdateT_us = 0;
    lastBitrateAdjustT_us = 0;
    lastTargetBitrateIUpdateT_us = 0;
    bytesRtp = 0;
    rateRtp = 0.0f;
    lastLossDetectIx = -1;
    ecnCeMarkedBytes = 0;

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
        float tDelta = (time_us - lastRateUpdateT_us) * 1e-6f;

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

    if (lossEventFlag || ecnCeEventFlag) {
        /*
        * Loss event handling
        * Rate is reduced slightly to avoid that more frames than necessary
        * queue up in the sender queue
        */
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
        if (lossEventFlag)
            targetBitrate = std::max(minBitrate, targetBitrate*lossEventRateScale);
        else if (ecnCeEventFlag)
            targetBitrate = std::max(minBitrate, targetBitrate*ecnCeEventRateScale);

        lossEventFlag = false;
        ecnCeEventFlag = false;
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

        float rtpQueueDelay = rtpQueue->getDelay(time_us * 1e-6f);
        if (rtpQueueDelay > maxRtpQueueDelay &&
            (time_us - lastRtpQueueDiscardT_us > kMinRtpQueueDiscardInterval_us)) {
            /*
            * RTP queue is cleared as it is becoming too large,
            * Function is however disabled initially as there is no reliable estimate of the
            * throughput in the initial phase.
            */
            rtpQueue->clear();

            rtpQueueDiscard = true;
            //parent->bytesInFlight = 0;

            lastRtpQueueDiscardT_us = time_us;
            targetRateScale = 1.0;
            txSizeBitsAvg = 0.0f;
        }
        else if (parent->inFastStart && rtpQueueDelay < 0.1f) {
            /*
            * Increment bitrate, limited by the rampUpSpeed
            */
            increment = rampUpSpeedTmp*(kRateAdjustInterval_us * 1e-6f);
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
                    increment = std::min(increment, (float)(rampUpSpeedTmp*(kRateAdjustInterval_us * 1e-6)));
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
* The function consists of equal measures or rational thinking and
* black magic, which means that there is no 100% guarantee that
* will always work.
*/
void ScreamTx::adjustPriorities(uint64_t time_us) {
    if (nStreams == 1 || time_us - lastAdjustPrioritiesT_us < 1000000) {
        /*
        * Skip if only one stream or if adjustment done less than 5s ago
        */
        return;
    }

    if (queueDelayTrend > 0.02) {
        /*
        * Adjust only if there is some evidence of congestion
        */
        int avgCreditLost = 0;
        int avgCreditLostN = 0;
        for (int n = 0; n < nStreams; n++) {
            avgCreditLost += streams[n]->creditLost;
            if (streams[n]->isActive)
                avgCreditLostN++;
        }
        if (avgCreditLostN <= 1) {
            /*
            * At the most 1 steam active, skip adjustment
            */
            return;
        }

        avgCreditLost /= avgCreditLostN;
        for (int n = 0; n < nStreams; n++) {
            if (true && streams[n]->isActive) {
                if (streams[n]->creditLost < avgCreditLost &&
                    streams[n]->targetBitrate > streams[n]->rateRtp) {
                    /*
                    * Stream is using more of its share than the average
                    * bitrate is likelky too high, reduce target bitrate
                    * This algorithm works best when we want to ensure
                    * different priorities
                    */
                    streams[n]->targetBitrate = std::max(streams[n]->minBitrate, streams[n]->targetBitrate*0.9f);
                }
            }
        }

        for (int n = 0; n < nStreams; n++)
            streams[n]->creditLost = 0;


        lastAdjustPrioritiesT_us = time_us;

    }
    if (time_us - lastAdjustPrioritiesT_us < 20000000) {
        /*
        * Clear old statistics of unused credits
        */
        for (int n = 0; n < nStreams; n++)
            streams[n]->creditLost = 0;


        lastAdjustPrioritiesT_us = time_us;
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
