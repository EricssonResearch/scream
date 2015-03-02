#include "RtpQueue.h"
#include "ScreamTx.h"
#include <string.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>

// === A few switches to make debugging easier 
// Turn off transmission scheduling i.e RTP packets pass throrugh
// This makes it possible to verify that OWD and CWND calulation works
const gboolean kBypassTxSheduling = FALSE;

// Open a full congestion window
const gboolean kOpenCwnd = FALSE;

// === Some good to have features, SCReAM works also 
//     with these disabled
// Enable shared bottleneck detection and OWD target adjustement
// good if SCReAM needs to compete with e.g FTP but 
// Can in some cases cause self-inflicted congestion
//  i.e the e2e delay can become large even though
//  there is no competing traffic present
static const gboolean kEnableSbd = FALSE; 
// Fast start can resume if little or no congestion detected 
static const gboolean kEnableConsecutiveFastStart = TRUE;
// Packet pacing reduces jitter
static const gboolean kEnablePacketPacing = TRUE;

// ==== Main tuning parameters (if tuning necessary) ====
// Most important parameters first
static const gfloat framePeriod = 0.040f;
// Max video rampup speed in bps/s (bits per second increase per second) 
static const gfloat kRampUpSpeed = 200000.0f; // bps/s
// CWND scale factor upon loss event
static const gfloat kLossBeta = 0.6f;
// Compensation factor for RTP queue size
// A higher value such as 0.2 gives less jitter esp. in wireless (LTE)
// but potentially also lower link utilization
static const gfloat kTxQueueSizeFactor = 1.0f;
// Compensation factor for detected congestion in rate computation 
// A higher value such as 0.2 gives less jitter esp. in wireless (LTE)
// but potentially also lower link utilization
static const gfloat kOwdGuard = 0.05f;
// Video rate scaling due to loss events
static const gfloat kLossEventRateScale = 0.9f;
// Additional send window slack (if no or little congestion detected)
// An incrased value such as 0.5 can improve transmission of Key frames
// however with a higher risk of unstable behavior in 
// sudden congestion situations
static const gfloat kBytesInFlightSlack = 0.0f;
// Rate update interval
static const guint64 kRateAdjustInterval_us = 200000; // 200ms  

// ==== Less important tuning parameters ====
// Min pacing interval and min pacing rate
static const gfloat kMinPaceInterval = 0.000f;
static const gfloat kMinimumBandwidth = 5000.0f; // bps
// Initial MSS, this is set quite low in order to make it possible to 
//  use SCReAM with audio only 
static const gint kInitMss = 100;
// CWND up and down gain factors
static const gfloat kGainUp = 1.0f;
static const gfloat kGainDown = 1.0f;
// Min and max OWD target
static const gfloat kOwdTargetMin = 0.1f; //ms
static const gfloat kOwdTargetMax = 0.3f; //ms
// Loss event detection reordering margin
static const gint kReOrderingLimit = 5; 
// Congestion window validation
static const gfloat kBytesInFlightHistInterval_us = 1000000; // Time (s) between stores 1s
static const gfloat kMaxBytesInFlightHeadRoom = 1.1f;
// OWD trend and shared bottleneck detection
static const guint64 kOwdFractionHistInterval_us = 50000; // 50ms
// Max video rate estimation update period
static const gfloat kRateUpdateInterval_us = 200000;  // 200ms

// Base delay history size

ScreamTx::ScreamTx()
    : sRttSh_us(0),
    sRtt_us(0),
    ackedOwd(0),
    baseOwd(G_MAXUINT32),
    owd(0.0),
    owdFractionAvg(0.0),
    owdTrend(0.0),
    owdTarget(kOwdTargetMin),
    owdSbdVar(0.0),
    owdSbdMean(0.0),
    owdSbdMeanSh(0.0),

    bytesNewlyAcked(0),
    mss(kInitMss),
    cwnd(kInitMss * 2),
    cwndMin(kInitMss * 2),
    cwndI(1),
    wasCwndIncrease(FALSE),
    lastBytesInFlightT_us(0),
    bytesInFlightMax(0),

    lossEvent(FALSE),

    inFastStart(TRUE),
    nFastStart(1),

    pacingBitrate(0.0),

    isInitialized(FALSE),
    lastSRttUpdateT_us(0), 
    lastBaseOwdAddT_us(0),
    baseOwdResetT_us(0),
    lastAddToOwdFractionHistT_us(0),
    lastLossEventT_us(0),
    lastTransmitT_us(0),
    nextTransmitT_us(0),
    lastRateUpdateT_us(0)
{
    for (int n=0; n < kBaseOwdHistSize; n++)
        baseOwdHist[n] = G_MAXUINT32;
    baseOwdHistPtr = 0;
    for (int n=0; n < kOwdFractionHistSize; n++)
        owdFractionHist[n] = 0.0f;
    owdFractionHistPtr = 0;
    for (int n=0; n < kOwdNormHistSize; n++)
        owdNormHist[n] = 0.0f;
    owdNormHistPtr = 0;
    for (int n=0; n < kBytesInFlightHistSize; n++)
        bytesInFlightHist[n] = 0;
    bytesInFlightHistPtr = 0;
    for (int n=0; n < kMaxTxPackets; n++) 
        txPackets[n].isUsed = FALSE;
    nStreams = 0;
    for (int n=0; n < kMaxStreams; n++) 
        streams[n] = NULL;
}

ScreamTx::~ScreamTx() {
    for (int n=0; n < nStreams; n++) 
        delete streams[n];
}

/*
* Register new stream
*/
void ScreamTx::registerNewStream(RtpQueue *rtpQueue,
    guint32 ssrc, 
    gfloat priority,
    gfloat minBitrate,
    gfloat maxBitrate,
    gfloat frameRate) {
        Stream *stream = new Stream(this, rtpQueue,ssrc,priority,minBitrate,maxBitrate,frameRate);
        streams[nStreams++] = stream;
}

/*
* New media frame
*/
void ScreamTx::newMediaFrame(guint64 time_us, guint32 ssrc) {
    if (!isInitialized) initialize(time_us);
    Stream *stream = getStream(ssrc);
    stream->updateTargetBitrate(time_us);
    /*
    * Need to update MSS here, otherwise it will be nearly impossible to 
    * transmit video packets, this because of the small initial MSS 
    * which is necessary to make SCReAM work with audio only
    */
    gint sizeOfNextRtp = stream->rtpQueue->sizeOfNextRtp();
    mss = MAX(mss, sizeOfNextRtp);
    cwndMin = 2 * mss;
}

/*
* Determine if OK to transmit RTP packet
*/
gfloat ScreamTx::isOkToTransmit(guint64 time_us, guint32 &ssrc) {
    if (!isInitialized) initialize(time_us);
    /*
    * Update rateTransmitted and rateAcked if time for it
    * this is used in video rate computation
    */
    if (time_us-lastRateUpdateT_us > kRateUpdateInterval_us) {
        for (int n=0; n < nStreams; n++) {
            streams[n]->updateRate(time_us);
        }
        lastRateUpdateT_us = time_us;
        /*
        * Adjust stream priorities
        */
        adjustPriorities(time_us);
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

    if (kBypassTxSheduling) {
        /*
        * Transmission scheduling is bypassed
        */ 
        return 0;
    }
    /*
    * Enforce packet pacing
    */
    if (nextTransmitT_us - time_us > 1000 && nextTransmitT_us > time_us)
        return (nextTransmitT_us-time_us)/1e6;

    gfloat paceInterval = kMinPaceInterval;

    bytesInFlightMax = MAX(bytesInFlight(), bytesInFlightMax);

    /*
    * Update bytes in flight history for congestion window validation
    */ 
    if (time_us - lastBytesInFlightT_us > kBytesInFlightHistInterval_us) {
        bytesInFlightHist[bytesInFlightHistPtr] = bytesInFlightMax;
        bytesInFlightHistPtr = (bytesInFlightHistPtr+1) % kBytesInFlightHistSize;
        lastBytesInFlightT_us = time_us;
        bytesInFlightMax = 0;
        /* 
        * In addition, reset MSS, this is useful in case for instance
        * a video stream is put on hold, leaving only audio packets to be 
        * transmitted
        */ 
        mss = kInitMss;
        cwndMin = 2 * mss;
    }

    gint sizeOfNextRtp = stream->rtpQueue->sizeOfNextRtp();
    if (sizeOfNextRtp == -1)
        return -1.0f;
    gboolean exit = FALSE;
    /* 
    * Determine if window is large enough to transmit 
    * an RTP packet
    */ 
    if (owd > owdTarget) {
        exit = (bytesInFlight() + sizeOfNextRtp) > cwnd;
    } else {
        gfloat x_cwnd = 1.0f+kBytesInFlightSlack*MAX(0.0f,MIN(1.0f,1.0f-owdTrend/0.5f));
        gfloat max_cwnd = MAX(cwnd*x_cwnd, (gfloat)cwnd+mss);
        exit = bytesInFlight() + sizeOfNextRtp > max_cwnd;
    }
    /*
    * A retransmission time out mechanism to avoid deadlock
    */
    if (time_us - lastTransmitT_us > 200000) { // 200ms
        exit = FALSE;
    }
    if (!exit) {
        /*
        * Return value 0.0 = RTP packet can be immediately transmitted
        */
        return 0.0f;
    }

    return -1.0f;
} 

/*
* RTP packet transmitted
*/
gfloat ScreamTx::addTransmitted(guint64 time_us,
    guint32 ssrc,
    gint size,
    guint16 seqNr) {
        if (!isInitialized) 
            initialize(time_us);

        int k = 0;
        int ix = -1;
        while (k < kMaxTxPackets) {
            if (txPackets[k].isUsed == FALSE) {
                ix = k;
                break;
            }
            k++;
        }
        if (ix == -1) {
            /*
            * If you end up here then it is necessary to increase
            * kMaxTxPackets
            */
            ix = 0;
            cerr << "Max number of txPackets allocated" << endl;
        }
        txPackets[ix].timestamp = (guint32) (time_us/1000);
        txPackets[ix].timeTx_us = time_us;
        txPackets[ix].ssrc = ssrc;
        txPackets[ix].size = size;
        txPackets[ix].seqNr = seqNr;
        txPackets[ix].isUsed = TRUE;

        Stream* stream = getStream(ssrc);
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
        * compute paceInterval, we assume a min bw of 50kbps and a min tp of 1ms
        * for stable operation
        * this function implements the packet pacing
        */
        gfloat paceInterval = kMinPaceInterval;
        pacingBitrate = MAX(kMinimumBandwidth,cwnd * 8.0f / MAX(0.001f,getSRtt()));
        gfloat tp = (size * 8.0f) / pacingBitrate;
        if (owdFractionAvg > 0.1f && kEnablePacketPacing) {
            paceInterval = MAX(kMinPaceInterval,tp);
        }
        if (kBypassTxSheduling) {
            paceInterval = 0.0;
        }
        guint64 paceInterval_us = (guint64) (paceInterval*1000000);

        /*
        * Update MSS and cwndMin
        */
        mss = MAX(mss, size);
        cwndMin = 2 * mss;

        /*
        * Determine when next RTP packet can be transmitted
        */
        nextTransmitT_us = time_us + paceInterval_us;

        return paceInterval;
}

/*
* New incoming feedback
*/
void ScreamTx::incomingFeedback(guint64 time_us,
    guint32 ssrc,
    guint32 timestamp,
    guint16 highestSeqNr,
    guint8  nLoss,        
    gboolean qBit) {

        if (!isInitialized) initialize(time_us);
        Stream *stream = getStream(ssrc);
        for (int n=0; n < kMaxTxPackets; n++) {
            /*
            * Loop through TX packets with matching SSRC
            */ 
            Transmitted *tmp = &txPackets[n];
            if (tmp->isUsed == TRUE) {
                /*
                * RTP packet is in flight
                */ 
                if (stream->isMatch(tmp->ssrc)) {
                    if (tmp->seqNr == highestSeqNr) {
                        ackedOwd = timestamp - tmp->timestamp; 
                        guint64 rtt = time_us - tmp->timeTx_us;

                        sRttSh_us = (7 * sRttSh_us + rtt) / 8;
                        if (time_us - lastSRttUpdateT_us >  sRttSh_us) {
                            sRtt_us = (7 * sRtt_us + sRttSh_us) / 8;
                            lastSRttUpdateT_us = time_us;
                        }
                    }

                    /*
                    * Wrap-around safety net
                    */
                    guint32 seqNrExt = tmp->seqNr;
                    guint32 highestSeqNrExt = highestSeqNr;
                    if (seqNrExt < highestSeqNrExt && highestSeqNrExt-seqNrExt > 20000)
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
                    if (seqNrExt <= highestSeqNrExt) {
                        bytesNewlyAcked += tmp->size;
                        stream->bytesAcked += tmp->size;
                        tmp->isUsed = FALSE; 
                    } 
                }
            }
        }
        /*
        * Determine if a loss event has occurred
        */
        if (stream->nLoss != nLoss) {
            /*
            * The loss counter has increased 
            */
            stream->nLoss = nLoss;
            if (time_us - lastLossEventT_us > sRtt_us) {
                /*
                * The loss counter has increased and it is more than one RTT since last
                * time loss was detected
                */
                lossEvent = TRUE;
                lastLossEventT_us = time_us;
            }
        }

        if (lossEvent) {
            cerr << "LOSS " << (int)nLoss << endl;
            lastLossEventT_us = time_us;
            for (int n=0; n < nStreams; n++) {
                Stream *tmp = streams[n];
                tmp->lossEventFlag = TRUE;
            }
        }
        updateCwnd(time_us);
}

gfloat ScreamTx::getTargetBitrate(guint32 ssrc) {
    return getStream(ssrc)->targetBitrate;
}

void ScreamTx::printLog(double time) {
    gint inFlightMax = MAX(bytesInFlight(),getMaxBytesInFlight());

    /* 2- 4 */	cout <<	owd             << " " << owdTrend           << " " << owdTarget   << " " 
        /* 5- 7 */		 << owdSbdVar       << " " << owdSbdSkew         << " " << getSRtt()   << " "
        /* 8-11 */		 << cwnd            << " " << cwndI              << " " << inFlightMax << " " << bytesInFlight() << " " 
        /*12-13 */		 << isInFastStart() << " " << getPacingBitrate() << " ";

    for (int n=0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        /*14-17 */		 	    cout << tmp->txSizeBitsAvg/8 << " " << tmp->rtpQueue->getDelay(time) << " "
            << tmp->targetBitrate << " " << tmp->getMaxRate() << " ";
        /*18-21 */
    }
}

void ScreamTx::initialize(guint64 time_us) {
    isInitialized = TRUE;
    lastSRttUpdateT_us = time_us; 
    lastBaseOwdAddT_us = time_us;
    baseOwdResetT_us = time_us;
    lastAddToOwdFractionHistT_us = time_us;
    lastLossEventT_us = time_us;
    lastTransmitT_us = time_us;
    nextTransmitT_us = time_us;
    lastRateUpdateT_us = time_us;
    lastAdjustPrioritiesT_us = time_us;
}

ScreamTx::Stream::Stream(ScreamTx *parent_,
    RtpQueue *rtpQueue_,
    guint32 ssrc_, 
    gfloat priority_,
    gfloat minBitrate_,
    gfloat maxBitrate_,
    gfloat frameRate_) {
        parent = parent_;
        rtpQueue = rtpQueue_;
        ssrc = ssrc_;
        targetPriority = priority_;
        minBitrate = minBitrate_;
        maxBitrate = maxBitrate_;
        targetBitrate = minBitrate;
        frameRate = frameRate_;
        targetBitrateI = 1.0f;
        credit = 0.0f;
        bytesTransmitted = 0;
        bytesAcked = 0;
        rateTransmitted = 0.0f;
        rateAcked = 0.0f;
        lossEventFlag=FALSE;
        txSizeBitsAvg = 0.0f;
        lastRateUpdateT_us = 0;
        lastBitrateAdjustT_us = 0;
        nLoss = 0;

}

/*
* Update the estimated max media rate
*/
void ScreamTx::Stream::updateRate(guint64 time_us) {
    if (lastRateUpdateT_us != 0) {
        gfloat tDelta = (time_us-lastRateUpdateT_us)/1e6f;
        rateTransmitted = 0.5f*(rateTransmitted + bytesTransmitted*8.0f/tDelta);
        rateAcked = 0.5f*(rateAcked + bytesAcked*8.0f/tDelta);
    }
    bytesTransmitted = 0;
    bytesAcked = 0;
    lastRateUpdateT_us = time_us;
}

/*
* Get the estimated maximum media rate
*/
gfloat ScreamTx::Stream::getMaxRate() {
    return MAX(rateTransmitted,rateAcked);
}

/*
* The the stream that matches SSRC
*/
ScreamTx::Stream* ScreamTx::getStream(guint32 ssrc) {
    for (int n=0; n < nStreams; n++) {
        if (streams[n]->isMatch(ssrc)) {
            return streams[n];
        }
    }
    return NULL;
}

/*
* Update the target bitrate, the target bitrate includes the RTP overhead
*/
void ScreamTx::Stream::updateTargetBitrate(guint64 time_us) {
    if (lastBitrateAdjustT_us == 0) lastBitrateAdjustT_us = time_us;
    /*
    * Loss event handling
    * Rate is reduced slightly to avoid that more frames than necessary
    * queue up in the sender queue
    */
    if (lossEventFlag) {
        lossEventFlag = FALSE;
        targetBitrateI = targetBitrate;
        targetBitrate = MAX(minBitrate,
            targetBitrate*kLossEventRateScale);
        lastBitrateAdjustT_us  = time_us;	
    } else {
        if (time_us - lastBitrateAdjustT_us < kRateAdjustInterval_us)
            return;
        /*
        * Compute a maximum bitrate, this bitrates includes the RTP overhead
        */
        gfloat br = getMaxRate();
        /*
        * A scale factor that is dependent on the inflection point
        * i.e the last known highest video bitrate
        */ 
        gfloat sclI = (targetBitrate-targetBitrateI)/
            targetBitrateI;
        sclI *= 4;
        sclI = MAX(0.1f, MIN(1.0f, sclI*sclI));
        /* 
        * Need a priority scale as well
        * Otherwise it will be impossible to 
        *  achieve a bitrate differentiation
        * TODO This needs to be done differently
        *  distribute the total max throughput among the
        *  streams according to their priorities
        */

        gfloat increment = 0.0f;

        /*
        * Size of RTP queue [bits]
        * As this function is called immediately after a 
        * video frame is produced, we need to accept the new 
        * RTP packets in the queue, we subtract a number of bytes 
        * corresponding to the current bitrate
        * Ideally the txSizeBits value should ignore the last inserted frame
        * (RTP packets)
        */
        gint lastBytes = MAX(0,(int)(targetBitrate/8.0/frameRate));
        gint txSizeBits = MAX(0,rtpQueue->sizeOfQueue()-lastBytes)*8;

        txSizeBitsAvg = 0.5f*(txSizeBitsAvg+txSizeBits);
        /*
        * tmp is a local scaling factor that makes rate adaptation sligthly more 
        * aggressive when competing flows (e.g file transfers) are detected
        */ 
        gfloat tmp = 1.0f;
        if (parent->isCompetingFlows())
            tmp = 0.5f;

        if (parent->inFastStart && txSizeBits/targetBitrate < 0.1f) {
            /*
            * Increment scale factor, rate can increase from min to max
            * in kRampUpTime if no congestion is detected
            */
            increment = kRampUpSpeed*(kRateAdjustInterval_us/1e6)*(1.0f-MIN(1.0f, parent->owdTrend/0.2f*tmp));
            /*
            * Limit increase rate near the last known highest bitrate
            */
            increment *= sclI;
            /*
            * Add increment
            */
            targetBitrate += increment;
            /*
            * Put an extra cap in case the OWD starts to increase
            */
            targetBitrate *= MAX(0.5,(1.0f-kOwdGuard*parent->owdTrend*tmp));

            wasFastStart = TRUE;
        } else {
            /*
            * scl is an an adaptive scaling to prevent overshoot
            */
            gfloat scl = MIN(1.0f,MAX(0.0f, parent->owdFractionAvg-0.3f)/0.7f);
            scl += parent->owdTrend;

            /*
            * Update target rate
            */ 
            gfloat increment = br*
                MAX(0.5,(1.0f-kOwdGuard*scl*tmp))*
                (1.0f-kTxQueueSizeFactor*txSizeBitsAvg/targetBitrate*tmp)-targetBitrate;
            if (increment < 0) {
                if (wasFastStart) {
                    wasFastStart = FALSE;
                    targetBitrateI = targetBitrate;
                }
            } else {
                wasFastStart = TRUE;
                if (!parent->isCompetingFlows()) {
                    /*
                    * Limit the bitrate increase so that it takes atleast kRampUpTime to reach 
                    * from lowest to highest bitrate.
                    * This limitation is not in effect if competing flows are detected
                    */ 
                    increment *= sclI;	
                    increment = MIN(increment,(gfloat)(kRampUpSpeed*(kRateAdjustInterval_us/1e6)));
                }
            }
            targetBitrate += increment;		
        }
        lastBitrateAdjustT_us  = time_us;	
    }
    targetBitrate = MIN(maxBitrate,MAX(minBitrate,targetBitrate));

}

void ScreamTx::adjustPriorities(guint64 time_us) {
    if (nStreams == 1 || time_us - lastAdjustPrioritiesT_us < 5000000) {
        return;
    }
    lastAdjustPrioritiesT_us = time_us;
    gfloat br = 0.0f;
    gfloat tPrioSum = 0.0f;
    for (int n=0; n < nStreams; n++) {
        if (streams[n]->targetBitrate > 0.9*streams[n]->maxBitrate) {
            /*
            * Don't adjust prioritites if atleast one stream runs at its
            * highest target bitrate
            */
            return;
        }
        br += streams[n]->getMaxRate();
        tPrioSum += streams[n]->targetPriority;
    }
    /*
    * Force down the target bitrate for streams that consume an unduly 
    * amount of the bandwidth, given the priority distribution
    */
    for (int n=0; n < nStreams; n++) {
        gfloat brShare = br*streams[n]->targetPriority/tPrioSum; 
        gfloat diff = (streams[n]->getMaxRate()-brShare);
        if (diff > 0) {
            streams[n]->targetBitrate = MAX(streams[n]->minBitrate, streams[n]->targetBitrate-diff);
        }
    }
}


/*
* Get the prioritized stream
*/
ScreamTx::Stream* ScreamTx::getPrioritizedStream(guint64 time_us) {
    /*
    * Function that prioritizes between streams, this function may need
    * to be modified to handle the prioritization better for e.g 
    * FEC, SVC etc.
    */ 
    gfloat maxCredit = 1.0;
    Stream *stream = NULL;

    /*
    * Pick a stream with credit higher or equal to
    * the size of the next RTP packet in queue for the given stream.
    */
    for (int n=0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        if (tmp->rtpQueue->sizeOfQueue() == 0) {
            /*
            * Queue empty
            */
        } else {
            /*
            * Pick stream if it has the highest credit so far
            */
            int rtpSz = tmp->rtpQueue->sizeOfNextRtp();
            if (tmp->credit >= MAX(maxCredit, (gfloat) rtpSz)) {
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
    double maxPrio = 0.0;
    for (int n=0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        gfloat priority =  tmp->targetPriority;
        int rtpSz = tmp->rtpQueue->sizeOfNextRtp();
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
void ScreamTx::addCredit(guint64 time_us, Stream* servedStream, gint transmittedBytes) {
    /*
    * Add a credit to stream(s) that did not get priority to transmit RTP packets
    */ 
    for (int n=0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        if (tmp != servedStream) { 
            gfloat credit = transmittedBytes*tmp->targetPriority/servedStream->targetPriority;
            if (tmp->rtpQueue->sizeOfNextRtp() > 0) {
                tmp->credit += credit;
            } else { 
                tmp->credit = MIN((gfloat) (2*mss), tmp->credit + credit);
            }
        }
    }
}

/*
* Subtract credit from served stream
*/
void ScreamTx::subtractCredit(guint64 time_us, Stream* servedStream, gint transmittedBytes) {
    /*
    * Subtract a credit equal to the number of transmitted bytes from the stream that 
    * transmitted a packet
    */
    servedStream->credit = MAX(0.0f, servedStream->credit-transmittedBytes);
}

/*
* Update the  congetsion window
*/
void ScreamTx::updateCwnd(guint64 time_us) {
    /*
    * Compute the OWD 
    */
    guint64 tmp = estimateOwd(time_us) - getBaseOwd();
    /*
    * Convert from [jiffy] OWD to an OWD in [s]
    */ 
    owd = tmp/kTimestampRate;


    if (owd > MAX(0.2f,2*getSRtt()) && time_us - baseOwdResetT_us > 10000000) {
        /*
        * The base OWD is likely wrong, for instance due to 
        * a channel change, reset base OWD history
        */
        for (int n=0; n < kBaseOwdHistSize; n++)
            baseOwdHist[n] = G_MAXUINT32;
        baseOwd = G_MAXUINT32;
        baseOwdResetT_us = time_us;
    }
    /*
    * An averaged version of the OWD fraction
    * neceassary in order to make video rate control robust 
    * against jitter 
    */
    owdFractionAvg = 0.9f*owdFractionAvg + 0.1f*getOwdFraction(); 

    /*
    * Save to OWD fraction history
    * used in computeOwdTrend()
    */
    if ((time_us - lastAddToOwdFractionHistT_us) >
        kOwdFractionHistInterval_us) {
            owdFractionHist[owdFractionHistPtr] = getOwdFraction();
            owdFractionHistPtr = (owdFractionHistPtr+1) % kOwdFractionHistSize;
            computeOwdTrend();
            if (kEnableSbd) {
                /*
                * Shared bottleneck detection, 
                */
                gfloat owdNorm = owd/kOwdTargetMin;
                owdNormHist[owdNormHistPtr] = owdNorm;
                owdNormHistPtr = (owdNormHistPtr+1) % kOwdNormHistSize;
                /*
                * Compute shared bottleneck detection and update OWD target
                * if OWD variance is sufficienctly low
                */    
                computeSbd(); 
                /* 
                * This function avoids the adjustment of owdTarget when 
                * congestion occurs (indicated by high owdSbdVar and owdSbdSkew)
                */
                if (owdSbdVar < 0.2 && owdSbdSkew < 0.05) { 
                    owdTarget = MAX(kOwdTargetMin,
                        MIN(kOwdTargetMax,owdSbdMeanSh*kOwdTargetMin*1.1f));
                } else if (owdSbdMeanSh*kOwdTargetMin < owdTarget) {
                    owdTarget = MAX(kOwdTargetMin,owdSbdMeanSh*kOwdTargetMin);
                }
            }
            lastAddToOwdFractionHistT_us = time_us;
    }
    /*
    * off_target is a normalized deviation from the owdTarget
    */
    gfloat offTarget = (owdTarget - owd) / gfloat(owdTarget);


    if (lossEvent) {
        /*
        * loss event detected, decrease congestion window
        */
        cwndI = cwnd;
        cwnd = MAX(cwndMin, (int) (kLossBeta*cwnd));
        lossEvent = FALSE;
        lastCongestionDetectedT_us = time_us;

        inFastStart = FALSE;
    }
    else {
        /*
        * Compute a scaling dependent on the relation between CWND and the inflection point
        * i.e the last known max value. This helps to reduce the CWND growth close to
        * the last known congestion point
        */
        gfloat sclI = (cwnd-cwndI)/((gfloat)(cwndI));
        sclI *= 4.0f;
        sclI = MAX(0.1f, MIN(1.0f, sclI*sclI));

        if (inFastStart) {
            /*
            * In fast start, variable exit condition depending of
            * if it is the first fast start or a later
            * In addition, the threshhold is more relaxed if 
            * competing flows are detected
            */
            gfloat th = 0.2f;
            if (isCompetingFlows())
                th = 0.5f;
            else if (nFastStart > 1)
                th = 0.1f;
            if (owdTrend < th) {
                /*
                * CWND is in principle increased by the number of ACKed bytes
                * save for a restriction to 10*MSS. In addition the increase
                * is limited if CWND is close to the last known max value
                */
                cwnd += MIN(10*mss,(int)(bytesNewlyAcked*sclI));
            } else {
                inFastStart = FALSE;
                lastCongestionDetectedT_us = time_us;
                cwndI = cwnd;
                wasCwndIncrease = TRUE;
            }
        } else {
            if (offTarget > 0.0f) {
                /*
                * OWD below target
                */
                wasCwndIncrease = TRUE;
                /*
                * Limit growth if OWD shows an increasing trend
                */
                gfloat gain = kGainUp*(1.0f+MAX(0.0f,1.0f-owdTrend/0.2f));
                gain *= sclI;

                cwnd += (int)(gain * offTarget * bytesNewlyAcked * mss / cwnd + 0.5f);
            } else {
                /*
                * OWD above target
                */
                if (wasCwndIncrease) {
                    wasCwndIncrease = FALSE;
                    cwndI = cwnd;
                }
                cwnd += (int)(kGainDown * offTarget * bytesNewlyAcked * mss / cwnd);
                lastCongestionDetectedT_us = time_us;
            }
        }
    }
    /*
    * Congestion window validation, checks that the congestion window is
    * not considerably higher than the actual number of bytes in flight
    */ 
    gint maxBytesInFlight = (int)(MAX(bytesInFlightMax, getMaxBytesInFlight()) *
        kMaxBytesInFlightHeadRoom);

    if (maxBytesInFlight > 0) {
        cwnd = MIN(cwnd, maxBytesInFlight);
    }

    cwnd = MAX(cwndMin, cwnd);

    if (kOpenCwnd) {
        /*
        *
        */
        cwnd = 1000000;
    }
    /*
    * Make possible to enter fast start if OWD has been low for a while
    * The detection threshold is a bit less strick when competing flows are detected
    */
    gfloat th = 0.2f;
    if (isCompetingFlows())
        th = 0.5f;
    if (owdTrend > th) {
        lastCongestionDetectedT_us = time_us;
    } else if (time_us - lastCongestionDetectedT_us > 1000000 && 
        !inFastStart && kEnableConsecutiveFastStart ) {
            /*
            * The OWD trend has been low for more than 1.0s, resume fast start
            */
            inFastStart = TRUE;
            lastCongestionDetectedT_us = time_us;
            nFastStart++;
    }
    bytesNewlyAcked = 0;
}

/*
* Update base OWD (if needed) and return the
* last estimated OWD (without offset compensation)
*/
guint32 ScreamTx::estimateOwd(guint64 time_us) {
    baseOwd = MIN(baseOwd, ackedOwd);
    if (time_us - lastBaseOwdAddT_us >= 1000000  ) {
        baseOwdHist[baseOwdHistPtr] = baseOwd;
        baseOwdHistPtr = (baseOwdHistPtr+1) % kBaseOwdHistSize;
        lastBaseOwdAddT_us = time_us;
        baseOwd = G_MAXUINT32;
    }
    return ackedOwd;
}

/*
* Get the base one way delay
*/
guint32 ScreamTx::getBaseOwd() {
    guint32 ret = baseOwd;
    for (int n=0; n < kBaseOwdHistSize; n++)
        ret = MIN(ret, baseOwdHist[n]);
    return ret;
}

/*
* Compute current number of bytes in flight
*/
gint ScreamTx::bytesInFlight() {
    gint ret = 0;
    for (int n=0; n < kMaxTxPackets; n++) {
        if (txPackets[n].isUsed == TRUE) 
            ret += txPackets[n].size;
    }
    return ret;
}

/*
* Get max bytes in flight over a time window
*/
gint ScreamTx::getMaxBytesInFlight() {
    /*
    * All elements in the buffer must be initialized before
    * return value > 0
    */
    if (bytesInFlightHist[bytesInFlightHistPtr] == 0)
        return 0;
    guint ret = 0;
    for (int n=0; n < kBytesInFlightHistSize; n++) {
        ret = MAX(ret,bytesInFlightHist[n]);
    }
    return ret;
}

/*
* Get the OWD fraction
*/
gfloat ScreamTx::getOwdFraction() {
    return owd / owdTarget;
}

/*
* Compute congestion indicator
*/
void ScreamTx::computeOwdTrend() {
    owdTrend = 0.0;
    gint ptr = owdFractionHistPtr;
    gfloat avg = 0.0f, x1, x2, a0, a1;

    for (int n=0; n < kOwdFractionHistSize; n++) {
        avg += owdFractionHist[ptr];
        ptr = (ptr+1) % kOwdFractionHistSize;
    }
    avg /= kOwdFractionHistSize;

    ptr = owdFractionHistPtr;
    x2 = 0.0f;
    a0 = 0.0f;
    a1 = 0.0f;
    for (int n=0; n < kOwdFractionHistSize; n++) {
        x1 = owdFractionHist[ptr] - avg;
        a0 += x1 * x1;
        a1 += x1 * x2;
        x2 = x1;
        ptr = (ptr+1) % kOwdFractionHistSize;
    }
    if (a0 > 0 ) {
        owdTrend = MAX(0.0f, MIN(1.0f, (a1 / a0)*owdFractionAvg));
    }
}

/*
* Compute indicators of shared bottleneck
*/
void ScreamTx::computeSbd() {
    gfloat owdNorm, tmp;
    owdSbdMean = 0.0;
    owdSbdMeanSh = 0.0;
    owdSbdVar = 0.0;
    gint ptr = owdNormHistPtr;
    for (int n=0; n < kOwdNormHistSize; n++) {
        owdNorm = owdNormHist[ptr];
        owdSbdMean += owdNorm;
        if (n >= kOwdNormHistSize - 20) {
            owdSbdMeanSh += owdNorm;
        }
        ptr = (ptr+1) % kOwdNormHistSize;
    }
    owdSbdMean /= kOwdNormHistSize;
    owdSbdMeanSh /= 20;

    ptr = owdNormHistPtr;
    for (int n=0; n < kOwdNormHistSize; n++) {
        owdNorm = owdNormHist[ptr];
        tmp = owdNorm - owdSbdMean;
        owdSbdVar += tmp * tmp;
        owdSbdSkew += tmp * tmp * tmp;
        ptr = (ptr+1) % kOwdNormHistSize;
    }
    owdSbdVar /= kOwdNormHistSize;
    owdSbdSkew /= kOwdNormHistSize;
}

/*
* True if the owdTarget is increased due to 
* detected competing flows
*/
gboolean ScreamTx::isCompetingFlows() {
    return owdTarget > kOwdTargetMin;
}
