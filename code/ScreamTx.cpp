#include "RtpQueue.h"
#include "ScreamTx.h"
#include <string.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>

// === A few switches to make debugging easier 
// Turn off transmission scheduling i.e RTP packets pass throrugh
// This makes it possible to verify that OWD and CWND calulation works
const bool kBypassTxSheduling = false;

// Open a full congestion window
const bool kOpenCwnd = false;

// === Some good to have features, SCReAM works also 
//     with these disabled
// Enable shared bottleneck detection and OWD target adjustement
// good if SCReAM needs to compete with e.g FTP but 
// Can in some cases cause self-inflicted congestion
//  i.e the e2e delay can become large even though
//  there is no competing traffic present
static const bool kEnableSbd = true; 
// Fast start can resume if little or no congestion detected 
static const bool kEnableConsecutiveFastStart = true;
// Packet pacing reduces jitter
static const bool kEnablePacketPacing = true;

// ==== Main tuning parameters (if tuning necessary) ====
// Most important parameters first
static const float framePeriod = 0.040f;
// Max video rampup speed in bps/s (bits per second increase per second) 
static const float kRampUpSpeed = 200000.0f; // bps/s
// CWND scale factor upon loss event
static const float kLossBeta = 0.6f;
// Compensation factor for RTP queue size
// A higher value such as 0.2 gives less jitter esp. in wireless (LTE)
// but potentially also lower link utilization
static const float kTxQueueSizeFactor = 1.0f;
// Compensation factor for detected congestion in rate computation 
// A higher value such as 0.2 gives less jitter esp. in wireless (LTE)
// but potentially also lower link utilization
static const float kOwdGuard = 0.2f;
// Video rate scaling due to loss events
static const float kLossEventRateScale = 0.9f;
// Rate update interval
static const uint64_t kRateAdjustInterval_us = 200000; // 200ms  

// ==== Less important tuning parameters ====
// Min pacing interval and min pacing rate
static const float kMinPaceInterval = 0.000f;
static const float kMinimumBandwidth = 5000.0f; // bps
// Initial MSS, this is set quite low in order to make it possible to 
//  use SCReAM with audio only 
static const int kInitMss = 100;
// CWND up and down gain factors
static const float kGainUp = 1.0f;
static const float kGainDown = 1.0f;
// Min and max OWD target
static const float kOwdTargetMin = 0.1f; //ms
static const float kOwdTargetMax = 0.3f; //ms
// Loss event detection reordering margin
static const int kReOrderingLimit = 5; 
// Congestion window validation
static const float kBytesInFlightHistInterval_us = 1000000; // Time (s) between stores 1s
static const float kMaxBytesInFlightHeadRoom = 1.0f;
// OWD trend and shared bottleneck detection
static const uint64_t kOwdFractionHistInterval_us = 50000; // 50ms
// Max video rate estimation update period
static const float kRateUpdateInterval_us = 50000;  // 200ms
// Max RTP queue delay, RTP queue is cleared if this value is exceeded
static const float kMaxRtpQueueDelay = 2.0;  // 2s


// Base delay history size

ScreamTx::ScreamTx()
    : sRttSh_us(0),
    sRtt_us(0),
    ackedOwd(0),
    baseOwd(UINT32_MAX),
    owd(0.0),
    owdFractionAvg(0.0),
    owdTrend(0.0),
    owdTarget(kOwdTargetMin),
    owdSbdVar(0.0),
    owdSbdMean(0.0),
    owdSbdMeanSh(0.0),

    bytesNewlyAcked(0),
    mss(kInitMss),
    cwnd(5000),//kInitMss * 2),
    cwndMin(kInitMss * 2),
    cwndI(1),
    wasCwndIncrease(false),
    lastBytesInFlightT_us(0),
    bytesInFlightMaxLo(0),
    bytesInFlightMaxHi(0),

    lossEvent(false),

    inFastStart(true),
    nFastStart(1),

    pacingBitrate(0.0),

    isInitialized(false),
    lastSRttUpdateT_us(0), 
    lastBaseOwdAddT_us(0),
    baseOwdResetT_us(0),
    lastAddToOwdFractionHistT_us(0),
    lastLossEventT_us(0),
    lastTransmitT_us(0),
    nextTransmitT_us(0),
    lastRateUpdateT_us(0),
    accBytesInFlightMax(0),
    nAccBytesInFlightMax(0),
    rateTransmitted(0),
    owdTrendMem(0.0f)
{
    for (int n=0; n < kBaseOwdHistSize; n++)
        baseOwdHist[n] = UINT32_MAX;
    baseOwdHistPtr = 0;
    for (int n=0; n < kOwdFractionHistSize; n++)
        owdFractionHist[n] = 0.0f;
    owdFractionHistPtr = 0;
    for (int n=0; n < kOwdNormHistSize; n++)
        owdNormHist[n] = 0.0f;
    owdNormHistPtr = 0;
    for (int n=0; n < kBytesInFlightHistSize; n++) {
        bytesInFlightHistLo[n] = 0;
        bytesInFlightHistHi[n] = 0;
    }
    bytesInFlightHistPtr = 0;
    for (int n=0; n < kMaxTxPackets; n++) 
        txPackets[n].isUsed = false;
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
    uint32_t ssrc, 
    float priority,
    float minBitrate,
    float maxBitrate,
    float frameRate) {
        Stream *stream = new Stream(this, rtpQueue,ssrc,priority,minBitrate,maxBitrate,frameRate);
        streams[nStreams++] = stream;
}

/*
* New media frame
*/
void ScreamTx::newMediaFrame(uint64_t time_us, uint32_t ssrc, int bytesRtp) {
    if (!isInitialized) initialize(time_us);
    Stream *stream = getStream(ssrc);
    stream->updateTargetBitrate(time_us);
    stream->bytesRtp += bytesRtp;
    /*
    * Need to update MSS here, otherwise it will be nearly impossible to 
    * transmit video packets, this because of the small initial MSS 
    * which is necessary to make SCReAM work with audio only
    */
    int sizeOfNextRtp = stream->rtpQueue->sizeOfNextRtp();
    mss = std::max(mss, sizeOfNextRtp);
    cwndMin = 2 * mss;
}

/*
* Determine active streams
*/
void ScreamTx::determineActiveStreams(uint64_t time_us) {
	float surplusBitrate = 0.0f;
	float sumPrio = 0.0;
	bool streamSetInactive = false;
	for (int n = 0; n < nStreams; n++) {
		if (time_us-streams[n]->lastFrameT_us > 1000000 && streams[n]->isActive) {
			streams[n]->isActive = false;
			surplusBitrate += streams[n]->targetBitrate;
			streams[n]->targetBitrate = streams[n]->minBitrate;
			streamSetInactive = true;
		} else {
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
    * this is used in video rate computation
    */
    if (time_us-lastRateUpdateT_us > kRateUpdateInterval_us) {
        rateTransmitted = 0.0;
        for (int n=0; n < nStreams; n++) {
            streams[n]->updateRate(time_us);
            rateTransmitted += streams[n]->rateTransmitted;
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

    float paceInterval = kMinPaceInterval;

    bytesInFlightMaxLo = 0;
    if (nAccBytesInFlightMax > 0) {
        bytesInFlightMaxLo = accBytesInFlightMax/nAccBytesInFlightMax;
    }
    bytesInFlightMaxHi = std::max(bytesInFlight(), bytesInFlightMaxHi);

    /*
    * Update bytes in flight history for congestion window validation
    */ 
    if (time_us - lastBytesInFlightT_us > kBytesInFlightHistInterval_us) {
        bytesInFlightHistLo[bytesInFlightHistPtr] = bytesInFlightMaxLo;
        bytesInFlightHistHi[bytesInFlightHistPtr] = bytesInFlightMaxHi;
        bytesInFlightHistPtr = (bytesInFlightHistPtr+1) % kBytesInFlightHistSize;
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
        cwndMin = 2 * mss;
    }

    int sizeOfNextRtp = stream->rtpQueue->sizeOfNextRtp();
    if (sizeOfNextRtp == -1)
        return -1.0f;
    bool exit = false;
    /* 
    * Determine if window is large enough to transmit 
    * an RTP packet
    */ 
    if (owd > owdTarget) {
        exit = (bytesInFlight() + sizeOfNextRtp) > cwnd;
    } else {
        exit = (bytesInFlight() + sizeOfNextRtp) > cwnd + mss;
    }
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
        return 0.0f;
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

        int k = 0;
        int ix = -1;
        while (k < kMaxTxPackets) {
            if (txPackets[k].isUsed == false) {
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
        txPackets[ix].timestamp = (uint32_t) (time_us/1000);
        txPackets[ix].timeTx_us = time_us;
        txPackets[ix].ssrc = ssrc;
        txPackets[ix].size = size;
        txPackets[ix].seqNr = seqNr;
        txPackets[ix].isUsed = true;

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
        float paceInterval = kMinPaceInterval;
        pacingBitrate = std::max(kMinimumBandwidth,cwnd * 8.0f / std::max(0.001f,getSRtt()));
        float tp = (size * 8.0f) / pacingBitrate;
        if (owdFractionAvg > 0.1f && kEnablePacketPacing) {
            paceInterval = std::max(kMinPaceInterval,tp);
        }
        if (kBypassTxSheduling) {
            paceInterval = 0.0;
        }
        uint64_t paceInterval_us = (uint64_t) (paceInterval*1000000);

        /*
        * Update MSS and cwndMin
        */
        mss = std::max(mss, size);
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
void ScreamTx::incomingFeedback(uint64_t time_us,
    uint32_t ssrc,
    uint32_t timestamp,
    uint16_t highestSeqNr,
    uint8_t  nLoss,        
    bool qBit) {
        if (!isInitialized) initialize(time_us);
        Stream *stream = getStream(ssrc);
        accBytesInFlightMax += bytesInFlight();
        nAccBytesInFlightMax++;

        for (int n=0; n < kMaxTxPackets; n++) {
            /*
            * Loop through TX packets with matching SSRC
            */ 
            Transmitted *tmp = &txPackets[n];
            if (tmp->isUsed == true) {
                /*
                * RTP packet is in flight
                */ 
                if (stream->isMatch(tmp->ssrc)) {
                    if (tmp->seqNr == highestSeqNr) {
                        ackedOwd = timestamp - tmp->timestamp; 
                        uint64_t rtt = time_us - tmp->timeTx_us;

                        sRttSh_us = (7 * sRttSh_us + rtt) / 8;
                        if (time_us - lastSRttUpdateT_us >  sRttSh_us) {
                            sRtt_us = (7 * sRtt_us + sRttSh_us) / 8;
                            lastSRttUpdateT_us = time_us;
                        }
                    }

                    /*
                    * Wrap-around safety net
                    */
                    uint32_t seqNrExt = tmp->seqNr;
                    uint32_t highestSeqNrExt = highestSeqNr;
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
                        tmp->isUsed = false; 
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
                lossEvent = true;
                lastLossEventT_us = time_us;
            }
        }

        if (lossEvent) {
            cerr << "LOSS " << (int)nLoss << endl;
            lastLossEventT_us = time_us;
            for (int n=0; n < nStreams; n++) {
                Stream *tmp = streams[n];
                tmp->lossEventFlag = true;
            }
        }
        updateCwnd(time_us);
}

float ScreamTx::getTargetBitrate(uint32_t ssrc) {
    return getStream(ssrc)->targetBitrate;
}

void ScreamTx::printLog(double time) {
    int inFlightMax = std::max(bytesInFlight(),getMaxBytesInFlightHi());

    /* 2- 4 */	cout <<	owd             << " " << owdTrend           << " " << owdTarget   << " " 
        /* 5- 7 */		 << owdSbdVar       << " " << owdSbdSkew         << " " << getSRtt()   << " "
        /* 8-11 */		 << cwnd            << " " << cwndI              << " " << inFlightMax << " " << bytesInFlight() << " " 
        /*12-13 */		 << isInFastStart() << " " << getPacingBitrate() << " ";

    for (int n=0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        /*14-20 */		 	    cout << tmp->txSizeBitsAvg/8 << " " << tmp->rtpQueue->getDelay(time) << " "
            << tmp->targetBitrate << " " << tmp->targetBitrateI << " " << tmp->getMaxRate() << " " << tmp->rateTransmitted << " " << tmp->rateAcked << " ";
        /*21-27 */
    }
}

void ScreamTx::initialize(uint64_t time_us) {
    isInitialized = true;
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
    uint32_t ssrc_, 
    float priority_,
    float minBitrate_,
    float maxBitrate_,
    float frameRate_) {
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
        lossEventFlag=false;
        txSizeBitsAvg = 0.0f;
        lastRateUpdateT_us = 0;
        lastBitrateAdjustT_us = 0;
		lastTargetBitrateIUpdateT_us = 0;
		nLoss = 0;
        bytesRtp = 0;
        rateRtp = 0.0f;
        rateRtpSum = 0.0f;
        rateRtpSumN = 0;
        for (int n=0; n < kRateRtpHistSize; n++)
           rateRtpHist[n] = 0;
        rateRtpHistPtr = 0;
        rateRtpMedian = 0.0f;
		for (int n = 0; n < kRateUpDateSize; n++) {
			rateRtpHistSh[n] = 0.0f;
			rateAckedHist[n] = 0.0f;
			rateTransmittedHist[n] = 0.0f;
		}
		rateUpdateHistPtr = 0;
		isActive = false;
		lastFrameT_us = 0;
		initTime_us = 0;
}

/*
* Update the estimated max media rate
*/
void ScreamTx::Stream::updateRate(uint64_t time_us) {
    if (lastRateUpdateT_us != 0) {
        float tDelta = (time_us-lastRateUpdateT_us)/1e6f;
		rateTransmittedHist[rateUpdateHistPtr] = bytesTransmitted*8.0f / tDelta;
		rateAckedHist[rateUpdateHistPtr] = bytesAcked*8.0f / tDelta;
		rateRtpHistSh[rateUpdateHistPtr] = bytesRtp * 8.0f / tDelta;
        if (rateRtpHist[0] == 0.0f) {
            /*
            * Initialize history
            */
            for (int i=0; i < kRateRtpHistSize; i++)
				rateRtpHist[i] = rateRtpHistSh[rateUpdateHistPtr];
        }
		rateUpdateHistPtr = (rateUpdateHistPtr + 1) % kRateUpDateSize;
		rateTransmitted = 0.0f;
		rateAcked = 0.0f;
		rateRtp = 0.0f;
		for (int n = 0; n < kRateUpDateSize; n++) {
			rateTransmitted += rateTransmittedHist[n];
			rateAcked += rateAckedHist[n];
			rateRtp += rateRtpHistSh[n];
		}
		rateTransmitted /= kRateUpDateSize;
		rateAcked /= kRateUpDateSize;
		rateRtp /= kRateUpDateSize;
    }

    /*
    * Generate a median RTP bitrate value, this serves to set a reasonably safe 
    * upper bound to the target bitrate. This limit is useful for stability purposes 
    * in cases where the link thoughput is limted and the input stimuli to e.g 
    * a video coder changes between static and varying
    */
    rateRtpSum += rateRtp;
    rateRtpSumN++;
	if (rateRtpSumN == 1000000 / kRateUpdateInterval_us) {
        /*
        * An average video bitrate is stored every 1s 
        */
        bool isPicked[kRateRtpHistSize];
        float sorted[kRateRtpHistSize];
        int i,j;
		rateRtpHist[rateRtpHistPtr] = rateRtpSum / (1000000 / kRateUpdateInterval_us);
        rateRtpHistPtr = (rateRtpHistPtr + 1) % kRateRtpHistSize;
        rateRtpSum = 0;
        rateRtpSumN = 0;
        /*
        * Create a sorted list
        */
        for (i=0; i < kRateRtpHistSize; i++)
            isPicked[i] = false;
        for (i=0; i < kRateRtpHistSize; i++) {
            float minR = 1.0e8;
            int minI;
            for (j=0; j < kRateRtpHistSize; j++) {
                if (rateRtpHist[j] < minR && !isPicked[j]) {
                    minR = rateRtpHist[j];
                    minI = j;
                }
            }
            sorted[i] = minR;
            isPicked[minI] = true;
        }
        /*
        * Get median value
        */
        rateRtpMedian = sorted[kRateRtpHistSize/2];
    }

    bytesTransmitted = 0;
    bytesAcked = 0;
    bytesRtp = 0;
    lastRateUpdateT_us = time_us;
}

/*
* Get the estimated maximum media rate
*/
float ScreamTx::Stream::getMaxRate() {
    return std::max(rateTransmitted,rateAcked);
}

/*
* The the stream that matches SSRC
*/
ScreamTx::Stream* ScreamTx::getStream(uint32_t ssrc) {
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
void ScreamTx::Stream::updateTargetBitrate(uint64_t time_us) {
	if (initTime_us == 0) {
		/*
		* Initialize if the first time
		*/
		initTime_us = time_us;
	}

    if (lastBitrateAdjustT_us == 0) lastBitrateAdjustT_us = time_us;
	isActive = true;
	lastFrameT_us = time_us;

    /*
    * Compute a maximum bitrate, this bitrates includes the RTP overhead
    */
    float br = getMaxRate();

    if (lossEventFlag) {
	    /*
		* Loss event handling
		* Rate is reduced slightly to avoid that more frames than necessary
		* queue up in the sender queue
		*/        
		lossEventFlag = false;
		if (time_us - lastTargetBitrateIUpdateT_us > 5000000) {
			/*
			* Avoid that target_bitrate_i is set too low in cases where a '
			* congestion event is prolonged
			*/
			targetBitrateI = rateAcked;
			lastTargetBitrateIUpdateT_us = time_us;
		}
        targetBitrate = std::max(minBitrate,
            targetBitrate*kLossEventRateScale);
        lastBitrateAdjustT_us  = time_us;	
    } else {
        if (time_us - lastBitrateAdjustT_us < kRateAdjustInterval_us)
            return;

        /*
        * A scale factor that is dependent on the inflection point
        * i.e the last known highest video bitrate
        */ 
        float sclI = (targetBitrate-targetBitrateI)/
            targetBitrateI;
        sclI *= 4;
        sclI = std::max(0.1f, std::min(1.0f, sclI*sclI));

        float increment = 0.0f;

        /*
        * Size of RTP queue [bits]
        * As this function is called immediately after a 
        * video frame is produced, we need to accept the new 
        * RTP packets in the queue, we subtract a number of bytes 
        * corresponding to the current bitrate
        * Ideally the txSizeBits value should ignore the last inserted frame
        * (RTP packets)
        */
        int lastBytes = std::max(0,(int)(targetBitrate/8.0/frameRate));
        int txSizeBits = std::max(0,rtpQueue->sizeOfQueue()-lastBytes)*8;
        
        float alpha = 0.5f;

        txSizeBitsAvg = txSizeBitsAvg*alpha+txSizeBits*(1.0f-alpha);
        /*
        * tmp is a local scaling factor that makes rate adaptation sligthly more 
        * aggressive when competing flows (e.g file transfers) are detected
        */ 
        float tmp = 1.0f;
        if (parent->isCompetingFlows())
            tmp = 0.5f;

		float rampUpSpeed = std::min(kRampUpSpeed, targetBitrate);

        if (txSizeBits/std::max(br,targetBitrate) > kMaxRtpQueueDelay && 
			(time_us - initTime_us > kMaxRtpQueueDelay*1000000)) {
			/*
			* RTP queue is cleared as it is becoming too large,
			* Function is however disabled initially as there is no reliable estimate of the 
			* thorughput in the initial phase.
			*/
            rtpQueue->clear();
            targetBitrate = minBitrate;
            txSizeBitsAvg = 0.0f;
        } else if (parent->inFastStart && txSizeBits/std::max(br,targetBitrate) < 0.1f) {
            /*
            * Increment scale factor, rate can increase from min to max
            * in kRampUpTime if no congestion is detected
            */
			increment = rampUpSpeed*(kRateAdjustInterval_us / 1e6)*(1.0f - std::min(1.0f, parent->getOwdTrend() / 0.2f*tmp));
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
            targetBitrate *= std::max(0.5f,(1.0f-kOwdGuard*parent->getOwdTrend()*tmp));

            wasFastStart = true;
        } else {
			if (wasFastStart) {
				wasFastStart = false;
				if (time_us - lastTargetBitrateIUpdateT_us > 5000000) {
					/*
					* Avoid that target_bitrate_i is set too low in cases where a
					* congestion event is prolonged
					*/
					targetBitrateI = rateAcked;
					lastTargetBitrateIUpdateT_us = time_us;
				}
			}
			/*
            * scl is an an adaptive scaling to prevent overshoot
            */
            float scl = std::min(1.0f,std::max(0.0f, parent->owdFractionAvg-0.3f)/0.7f);
			scl += parent->getOwdTrend();

            /*
            * Update target rate
            */ 
            float increment = br*(1.0f-kOwdGuard*scl*tmp)-
                kTxQueueSizeFactor*txSizeBitsAvg*tmp-targetBitrate;
            if (increment < 0) {
                if (wasFastStart) {
                    wasFastStart = false;
					if (time_us - lastTargetBitrateIUpdateT_us > 5000000) {
						/*
						* Avoid that target_bitrate_i is set too low in cases where a
						* congestion event is prolonged
						*/
						targetBitrateI = rateAcked;
						lastTargetBitrateIUpdateT_us = time_us;
					}
				}
            } else {
                wasFastStart = true;
                if (!parent->isCompetingFlows()) {
                    /*
                    * Limit the bitrate increase so that it takes atleast kRampUpTime to reach 
                    * from lowest to highest bitrate.
                    * This limitation is not in effect if competing flows are detected
                    */ 
                    increment *= sclI;	
					increment = std::min(increment, (float)(rampUpSpeed*(kRateAdjustInterval_us / 1e6)));
                }
            }
            targetBitrate += increment;		
        }
        lastBitrateAdjustT_us  = time_us;	
    }
    /*
    * Limit target bitrate so that it is not considerably higher than the actual bitrate,
    *  this improves stability in thorughput limited cases where video input changes a lot.
    * A median filtered value of the recent media bitrate is used for the limitation. This
    *  allows for a good performance in the cases where the input stimuli to the 
    *  video coder changes from static to varying.
    * This feature is disabled when competing (TCP) flows share the same bottleneck as it would
    *  otherwise degrade SCReAMs ability to grab a decent share of the bottleneck bandwidth
    */
    if (!parent->isCompetingFlows()) {
        float rateRtpLimit;
        rateRtpLimit = std::max(br,std::max(rateRtp, rateRtpMedian));
        rateRtpLimit *= (2.0-1.0*parent->owdTrendMem);
        targetBitrate = std::min(rateRtpLimit, targetBitrate);
    }

    targetBitrate = std::min(maxBitrate,std::max(minBitrate,targetBitrate));

}

/*
* Adjust (enforce) proper prioritization between active streams 
* at regular intervals. This is a necessary addon to mitigate 
* issues that VBR media brings
*/
void ScreamTx::adjustPriorities(uint64_t time_us) {
    if (nStreams == 1 || time_us - lastAdjustPrioritiesT_us < 5000000) {
        return;
    }
    lastAdjustPrioritiesT_us = time_us;
    float br = 0.0f;
    float tPrioSum = 0.0f;
    for (int n=0; n < nStreams; n++) {
		if (streams[n]->isActive) {
			if (streams[n]->targetBitrate > 0.9*streams[n]->maxBitrate ||
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
    for (int n=0; n < nStreams; n++) {
		if (streams[n]->isActive) {
			float brShare = br*streams[n]->targetPriority / tPrioSum;
			float diff = (streams[n]->getMaxRate() - brShare);
			if (diff > 0) {
				streams[n]->targetBitrate = std::max(streams[n]->minBitrate, streams[n]->targetBitrate - diff);
				streams[n]->targetBitrateI = streams[n]->targetBitrate;
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
    float maxCredit = 1.0;
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
            if (tmp->credit >= std::max(maxCredit, (float) rtpSz)) {
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
        float priority =  tmp->targetPriority;
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
void ScreamTx::addCredit(uint64_t time_us, Stream* servedStream, int transmittedBytes) {
    /*
    * Add a credit to stream(s) that did not get priority to transmit RTP packets
    */ 
    for (int n=0; n < nStreams; n++) {
        Stream *tmp = streams[n];
        if (tmp != servedStream) { 
            float credit = transmittedBytes*tmp->targetPriority/servedStream->targetPriority;
            if (tmp->rtpQueue->sizeOfNextRtp() > 0) {
                tmp->credit += credit;
            } else { 
				tmp->credit = std::min((float)(2 * mss), tmp->credit + credit);
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
    servedStream->credit = std::max(0.0f, servedStream->credit-transmittedBytes);
}

/*
* Update the  congestion window
*/
void ScreamTx::updateCwnd(uint64_t time_us) {
    /*
    * Compute the OWD 
    */
    uint64_t tmp = estimateOwd(time_us) - getBaseOwd();
    /*
    * Convert from [jiffy] OWD to an OWD in [s]
    */ 
    owd = tmp/kTimestampRate;


    if (owd > std::max(0.2f,2*getSRtt()) && time_us - baseOwdResetT_us > 10000000) {
        /*
        * The base OWD is likely wrong, for instance due to 
        * a channel change, reset base OWD history
        */
        for (int n=0; n < kBaseOwdHistSize; n++)
            baseOwdHist[n] = UINT32_MAX;
        baseOwd = UINT32_MAX;
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

            owdTrendMem = std::max(owdTrendMem*0.99f,owdTrend);

            if (kEnableSbd) {
                /*
                * Shared bottleneck detection, 
                */
                float owdNorm = owd/kOwdTargetMin;
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
                    owdTarget = std::max(kOwdTargetMin,
                        std::min(kOwdTargetMax,owdSbdMeanSh*kOwdTargetMin*1.1f));
                } else if (owdSbdMeanSh*kOwdTargetMin < owdTarget) {
                    owdTarget = std::max(kOwdTargetMin,owdSbdMeanSh*kOwdTargetMin);
                }
            }
            lastAddToOwdFractionHistT_us = time_us;
    }
    /*
    * off_target is a normalized deviation from the owdTarget
    */
    float offTarget = (owdTarget - owd) / float(owdTarget);


    if (lossEvent) {
        /*
        * loss event detected, decrease congestion window
        */
        cwndI = cwnd;
        cwnd = std::max(cwndMin, (int) (kLossBeta*cwnd));
        lossEvent = false;
        lastCongestionDetectedT_us = time_us;

        inFastStart = false;
    }
    else {
			/*
			* Compute a scaling dependent on the relation between CWND and the inflection point
			* i.e the last known max value. This helps to reduce the CWND growth close to
			* the last known congestion point
			*/
			float sclI = (cwnd - cwndI) / ((float)(cwndI));
			sclI *= 4.0f;
			sclI = std::max(0.1f, std::min(1.0f, sclI*sclI));

			if (inFastStart) {
				/*
				* In fast start, variable exit condition depending of
				* if it is the first fast start or a later
				* In addition, the threshhold is more relaxed if
				* competing flows are detected
				*/
				float th = 0.2f;
				if (isCompetingFlows())
					th = 0.5f;
				else if (nFastStart > 1)
					th = 0.1f;
				if (owdTrend < th) {
					/*
					* CWND is in principle increased by the number of ACKed bytes
					* save for a restriction to 10*MSS. In addition the increase
					* is limited if CWND is close to the last known max value
					* cwnd is not increased if less than half is used
					*/
					if (bytesInFlight() > cwnd / 2)
						cwnd += std::min(10 * mss, (int)(bytesNewlyAcked*sclI));
				}
				else {
					inFastStart = false;
					lastCongestionDetectedT_us = time_us;
					cwndI = cwnd;
					wasCwndIncrease = true;
				}
			}
			else {
				if (offTarget > 0.0f) {
					/*
					* OWD below target
					*/
					wasCwndIncrease = true;
					/*
					* Limit growth if OWD shows an increasing trend
					*/
					float gain = kGainUp*(1.0f + std::max(0.0f, 1.0f - owdTrend / 0.2f));
					gain *= sclI;

					cwnd += (int)(gain * offTarget * bytesNewlyAcked * mss / cwnd + 0.5f);
				}
				else {
					/*
					* OWD above target
					*/
					if (wasCwndIncrease) {
						wasCwndIncrease = false;
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
    int maxBytesInFlightHi = (int)(std::max(bytesInFlightMaxHi, getMaxBytesInFlightHi()));
    int maxBytesInFlightLo = (int)(std::max(bytesInFlight(), getMaxBytesInFlightLo()));
    float maxBytesInFlight = (maxBytesInFlightHi*(1.0-owdTrendMem)+maxBytesInFlightLo*owdTrendMem)*
        kMaxBytesInFlightHeadRoom;
    if (maxBytesInFlight > 5000) {
        cwnd = std::min(cwnd, (int)maxBytesInFlight);
    }

    if (getSRtt() < 0.01f && owdTrend < 0.1) {
        uint32_t tmp = rateTransmitted*0.01f/8;
        tmp = std::max(tmp,(uint32_t)(maxBytesInFlight*1.5));
        cwnd = std::max(cwnd,(int)tmp);
    }
    cwnd = std::max(cwndMin, cwnd);

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
    float th = 0.2f;
    if (isCompetingFlows())
        th = 0.5f;
    if (owdTrend > th) {
        lastCongestionDetectedT_us = time_us;
    } else if (time_us - lastCongestionDetectedT_us > 1000000 && 
        !inFastStart && kEnableConsecutiveFastStart ) {
            /*
            * The OWD trend has been low for more than 1.0s, resume fast start
            */
            inFastStart = true;
            lastCongestionDetectedT_us = time_us;
            nFastStart++;
    }
    bytesNewlyAcked = 0;
}

/*
* Update base OWD (if needed) and return the
* last estimated OWD (without offset compensation)
*/
uint32_t ScreamTx::estimateOwd(uint64_t time_us) {
    baseOwd = std::min(baseOwd, ackedOwd);
    if (time_us - lastBaseOwdAddT_us >= 1000000  ) {
        baseOwdHist[baseOwdHistPtr] = baseOwd;
        baseOwdHistPtr = (baseOwdHistPtr+1) % kBaseOwdHistSize;
        lastBaseOwdAddT_us = time_us;
        baseOwd = UINT32_MAX;
    }
    return ackedOwd;
}

/*
* Get the base one way delay
*/
uint32_t ScreamTx::getBaseOwd() {
    uint32_t ret = baseOwd;
    for (int n=0; n < kBaseOwdHistSize; n++)
        ret = std::min(ret, baseOwdHist[n]);
    return ret;
}

/*
* Compute current number of bytes in flight
*/
int ScreamTx::bytesInFlight() {
    int ret = 0;
    for (int n=0; n < kMaxTxPackets; n++) {
        if (txPackets[n].isUsed == true) 
            ret += txPackets[n].size;
    }
    return ret;
}

/*
* Get max bytes in flight over a time window
*/
int ScreamTx::getMaxBytesInFlightLo() {
    /*
    * All elements in the buffer must be initialized before
    * return value > 0
    */
    if (bytesInFlightHistLo[bytesInFlightHistPtr] == 0)
        return 0;
    uint32_t ret = 0;
    for (int n=0; n < kBytesInFlightHistSize; n++) {
        ret = std::max(ret,(uint32_t)bytesInFlightHistLo[n]);
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
    uint32_t ret = 0;
    for (int n=0; n < kBytesInFlightHistSize; n++) {
        ret = std::max(ret,(uint32_t)bytesInFlightHistHi[n]);
    }
    return ret;
}

/*
* Get the OWD fraction
*/
float ScreamTx::getOwdFraction() {
    return owd / owdTarget;
}

/*
* Compute congestion indicator
*/
void ScreamTx::computeOwdTrend() {
    owdTrend = 0.0;
    int ptr = owdFractionHistPtr;
    float avg = 0.0f, x1, x2, a0, a1;

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
        owdTrend = std::max(0.0f, std::min(1.0f, (a1 / a0)*owdFractionAvg));
    }
}

/*
* Compute indicators of shared bottleneck
*/
void ScreamTx::computeSbd() {
    float owdNorm, tmp;
    owdSbdMean = 0.0;
    owdSbdMeanSh = 0.0;
    owdSbdVar = 0.0;
    int ptr = owdNormHistPtr;
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
bool ScreamTx::isCompetingFlows() {
    return owdTarget > kOwdTargetMin;
}

/*
* Get OWD trend
*/
float ScreamTx::getOwdTrend() {
	return owdTrend;
}
