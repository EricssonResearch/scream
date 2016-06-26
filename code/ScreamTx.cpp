#include "RtpQueue.h"
#include "ScreamTx.h"
#include "ScreamRx.h"
#include <cstdint>
#include <string.h>
#include <iostream>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
using namespace std;

// === A few switches to make debugging easier 
// Turn off transmission scheduling i.e RTP packets pass throrugh
// This makes it possible to verify that queue delay and CWND calulation works
const bool kBypassTxSheduling = false;

// Open a full congestion window
const bool kOpenCwnd = false;

// === Some good to have features, SCReAM works also 
//     with these disabled
// Fast start can resume if little or no congestion detected 
static const bool kEnableConsecutiveFastStart = true;
// Packet pacing reduces jitter
static const bool kEnablePacketPacing = true;


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
// Min and max queue delay target
static const float kQueueDelayTargetMax = 0.3f; //ms

// Congestion window validation
static const float kBytesInFlightHistInterval_us = 1000000; // Time (s) between stores 1s
static const float kMaxBytesInFlightHeadRoom = 1.0f;
// Queue delay trend and shared bottleneck detection
static const uint64_t kQueueDelayFractionHistInterval_us = 50000; // 50ms
// video rate estimation update period
static const float kRateUpdateInterval_us = 50000;  // 50ms

// Packet reordering margin (us)
static const uint64_t kReorderTime_us = 20000;


// Base delay history size

ScreamTx::ScreamTx(float lossBeta_,
	float queueDelayTargetMin_,
	bool enableSbd_
	)
	: sRttSh_us(0),
	lossBeta(lossBeta_),
	queueDelayTargetMin(queueDelayTargetMin_),
	enableSbd(enableSbd_),
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
	cwnd(5000),//kInitMss * 2),
	cwndMin(kInitMss * 2),
	lastBytesInFlightT_us(0),
	bytesInFlightMaxLo(0),
	bytesInFlightMaxHi(0),

	lossEvent(false),
	wasLossEvent(false),
	lossEventRate(0.0),

	inFastStart(true),

	pacingBitrate(0.0),

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
	queueDelayTrendMem(0.0f)
{
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
	for (int n = 0; n < kMaxTxPackets; n++)
		txPackets[n].isUsed = false;
	nStreams = 0;
	for (int n = 0; n < kMaxStreams; n++)
		streams[n] = NULL;
}

ScreamTx::~ScreamTx() {
	for (int n = 0; n < nStreams; n++)
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
		maxBitrate,
		rampUpSpeed,
		maxRtpQueueDelay,
		txQueueSizeFactor,
		queueDelayGuard,
		lossEventRateScale);
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
	* this is used in video rate computation
	*/
	if (time_us - lastRateUpdateT_us > kRateUpdateInterval_us) {
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
		return (nextTransmitT_us - time_us) / 1e6;

	float paceInterval = kMinPaceInterval;

	bytesInFlightMaxLo = 0;
	if (nAccBytesInFlightMax > 0) {
		bytesInFlightMaxLo = accBytesInFlightMax / nAccBytesInFlightMax;
	}
	bytesInFlightMaxHi = std::max(bytesInFlight(), bytesInFlightMaxHi);

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
	if (queueDelay > queueDelayTarget) {
		exit = (bytesInFlight() + sizeOfNextRtp) > cwnd;
	}
	else {
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
	txPackets[ix].timestamp = (uint32_t)(time_us / 1000);
	txPackets[ix].timeTx_us = time_us;
	txPackets[ix].ssrc = ssrc;
	txPackets[ix].size = size;
	txPackets[ix].seqNr = seqNr;
	txPackets[ix].isUsed = true;
	txPackets[ix].isAcked = false;
	txPackets[ix].isAfterReceivedEdge = false;

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
	pacingBitrate = std::max(kMinimumBandwidth, cwnd * 8.0f / std::max(0.001f, getSRtt()));
	float tp = (size * 8.0f) / pacingBitrate;
	if (queueDelayFractionAvg > 0.1f && kEnablePacketPacing) {
		paceInterval = std::max(kMinPaceInterval, tp);
	}
	if (kBypassTxSheduling) {
		paceInterval = 0.0;
	}
	uint64_t paceInterval_us = (uint64_t)(paceInterval * 1000000);

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
	uint64_t ackVector,
	bool qBit) {
	if (!isInitialized) initialize(time_us);
	Stream *stream = getStream(ssrc);
	accBytesInFlightMax += bytesInFlight();
	nAccBytesInFlightMax++;
	uint32_t tmp1 = (ackVector >> 32);
	uint32_t tmp2 = (ackVector & 0xFFFFFFFF);
	//fprintf(stderr, "SN %5d  %x%x \n", highestSeqNr, tmp1, tmp2);
	for (int n = 0; n < kMaxTxPackets; n++) {
		/*
		* Loop through TX packets with matching SSRC
		*/
		Transmitted *tmp = &txPackets[n];
		if (tmp->isUsed == true) {
			/*
			* RTP packet is in flight
			*/
			if (stream->isMatch(tmp->ssrc)) {

				for (int k = 0; k < kAckVectorBits; k++) {
					if (ackVector & (1 << k)) { // SN marked as received
						uint16_t seqNr = highestSeqNr - (k + 1);
						if (tmp->seqNr == seqNr) {
							/*
							* RTP packet marked as ACKed
							*/
							tmp->isAcked = true;
						}
					}
				}

				if (tmp->seqNr == highestSeqNr) {
					tmp->isAcked = true;
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
	}

	/*
	* Determine if loss event has occured
	*/
	for (int n = 0; n < kMaxTxPackets; n++) {
		/*
		* Loop through TX packets with matching SSRC
		*/
		Transmitted *tmp = &txPackets[n];
		if (tmp->isUsed == true) {
			if (stream->isMatch(tmp->ssrc)) {
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
					cerr << "LOST " << tmp->seqNr;
					tmp->isUsed = false;
				}
				else if (tmp->isAcked) {
					tmp->isUsed = false;
				}
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

	updateCwnd(time_us);
}


float ScreamTx::getTargetBitrate(uint32_t ssrc) {
	return getStream(ssrc)->targetBitrate;
}

void ScreamTx::printLog(double time) {
	int inFlightMax = std::max(bytesInFlight(), getMaxBytesInFlightHi());

	cout <<
		/* 2- 4 */	queueDelay << " " << queueDelayTrend << " " << queueDelayTarget << " "
		/* 5- 7 */ << queueDelaySbdVar << " " << queueDelaySbdSkew << " " << getSRtt() << " "
		/* 8-10 */ << cwnd << " " << inFlightMax << " " << bytesInFlight() << " "
		/*11-12 */ << isInFastStart() << " " << getPacingBitrate() << " ";

	for (int n = 0; n < nStreams; n++) {
		Stream *tmp = streams[n];
		/*13-19 */		 	    cout << tmp->txSizeBitsAvg / 8 << " " << tmp->rtpQueue->getDelay(time) << " "
			<< tmp->targetBitrate << " " << tmp->targetBitrateI << " " << tmp->getMaxRate() << " " << tmp->rateTransmitted << " " << tmp->rateAcked << " ";
		/*20-26 */
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
}

ScreamTx::Stream::Stream(ScreamTx *parent_,
	RtpQueue *rtpQueue_,
	uint32_t ssrc_,
	float priority_,
	float minBitrate_,
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
	minBitrate = minBitrate_;
	maxBitrate = maxBitrate_;
	targetBitrate = minBitrate;
	rampUpSpeed = rampUpSpeed_;
	maxRtpQueueDelay = maxRtpQueueDelay_;
	txQueueSizeFactor = txQueueSizeFactor_;
	queueDelayGuard = queueDelayGuard_;
	lossEventRateScale = lossEventRateScale_;
	targetBitrateI = 1.0f;
	credit = 0.0f;
	bytesTransmitted = 0;
	bytesAcked = 0;
	rateTransmitted = 0.0f;
	rateAcked = 0.0f;
	lossEventFlag = false;
	txSizeBitsAvg = 0.0f;
	lastRateUpdateT_us = 0;
	lastBitrateAdjustT_us = 0;
	lastTargetBitrateIUpdateT_us = 0;
	bytesRtp = 0;
	rateRtp = 0.0f;
	rateRtpSum = 0.0f;
	rateRtpSumN = 0;
	for (int n = 0; n < kRateRtpHistSize; n++)
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
		float tDelta = (time_us - lastRateUpdateT_us) / 1e6f;
		rateTransmittedHist[rateUpdateHistPtr] = bytesTransmitted*8.0f / tDelta;
		rateAckedHist[rateUpdateHistPtr] = bytesAcked*8.0f / tDelta;
		rateRtpHistSh[rateUpdateHistPtr] = bytesRtp * 8.0f / tDelta;
		if (rateRtpHist[0] == 0.0f) {
			/*
			* Initialize history
			*/
			for (int i = 0; i < kRateRtpHistSize; i++)
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
	* in cases where the link thoughput is limited and the input stimuli to e.g
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
		int i, j;
		rateRtpHist[rateRtpHistPtr] = rateRtpSum / (1000000 / kRateUpdateInterval_us);
		rateRtpHistPtr = (rateRtpHistPtr + 1) % kRateRtpHistSize;
		rateRtpSum = 0;
		rateRtpSumN = 0;
		/*
		* Create a sorted list
		*/
		for (i = 0; i < kRateRtpHistSize; i++)
			isPicked[i] = false;
		for (i = 0; i < kRateRtpHistSize; i++) {
			float minR = 1.0e8;
			int minI;
			for (j = 0; j < kRateRtpHistSize; j++) {
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
		rateRtpMedian = sorted[kRateRtpHistSize / 2];
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
			targetBitrateI = targetBitrate;
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
		float sclI = (targetBitrate - targetBitrateI) /
			targetBitrateI;
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
		int txSizeBits = std::max(0, rtpQueue->sizeOfQueue() - lastBytes) * 8;

		float alpha = 0.5f;

		txSizeBitsAvg = txSizeBitsAvg*alpha + txSizeBits*(1.0f - alpha);
		/*
		* tmp is a local scaling factor that makes rate adaptation sligthly more
		* aggressive when competing flows (e.g file transfers) are detected
		*/
		float rampUpSpeedTmp = std::min(rampUpSpeed, targetBitrate / 2);
		if (parent->isCompetingFlows()) {
			rampUpSpeedTmp *= 2.0f;
		}

		if (txSizeBits / std::max(br, targetBitrate) > maxRtpQueueDelay &&
			(time_us - initTime_us > maxRtpQueueDelay * 1000000)) {
			/*
			* RTP queue is cleared as it is becoming too large,
			* Function is however disabled initially as there is no reliable estimate of the
			* thorughput in the initial phase.
			*/
			rtpQueue->clear();
			targetBitrate = minBitrate;
			txSizeBitsAvg = 0.0f;
		}
		else if (parent->inFastStart && txSizeBits / std::max(br, targetBitrate) < 0.1f) {
			/*
			* Increment scale factor, rate can increase from min to std::max
			* in kRampUpTime if no congestion is detected
			*/
			increment = rampUpSpeedTmp*(kRateAdjustInterval_us / 1e6);
			/*
			* Limit increase rate near the last known highest bitrate
			*/
			increment *= sclI;

			/*
			* Add increment
			*/
			targetBitrate += increment;

			wasFastStart = true;
		}
		else {
			if (wasFastStart) {
				wasFastStart = false;
				if (time_us - lastTargetBitrateIUpdateT_us > 5000000) {
					/*
					* Avoid that target_bitrate_i is set too low in cases where a
					* congestion event is prolonged
					*/
					targetBitrateI = std::min(br, targetBitrate);
					lastTargetBitrateIUpdateT_us = time_us;
				}
			}
			/*
			* scl is based on the queue delay trend
			*/
			double scl = queueDelayGuard*parent->getQueueDelayTrend();
			if (parent->isCompetingFlows())
				scl *= 0.05;

			/*
			* Update target rate
			*/
			float increment = br*(1.0f - scl) -
				txQueueSizeFactor*txSizeBitsAvg - targetBitrate;
			if (txSizeBits > 12000 && increment > 0)
				increment = 0;
			if (increment > 0) {
				wasFastStart = true;
				if (!parent->isCompetingFlows()) {
					/*
					* Limit the bitrate increase so that it takes atleast kRampUpTime to reach
					* from lowest to highest bitrate.
					* This limitation is not in effect if competing flows are detected
					*/
					increment *= sclI;
					increment = std::min(increment, (float)(rampUpSpeedTmp*(kRateAdjustInterval_us / 1e6)));
				}
			}
			targetBitrate += increment;
			float rtpQueueDelay = 0;
			/*
			* Apply additional rate decrease in case RTP queue delay is greater than 20ms
			*/
			if (br > 1e5)
				rtpQueueDelay = txSizeBits / br;
			if (rtpQueueDelay > 0.02) {
				if (time_us - lastTargetBitrateIUpdateT_us > 5000000) {
					targetBitrateI = std::min(rateAcked, targetBitrate);
					lastTargetBitrateIUpdateT_us = time_us;
				}
				targetBitrate *= 0.95;
			}

		}
		lastBitrateAdjustT_us = time_us;
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
		rateRtpLimit = std::max(br, std::max(rateRtp, rateRtpMedian));
		rateRtpLimit *= (2.0 - 1.0*parent->queueDelayTrendMem);
		targetBitrate = std::min(rateRtpLimit, targetBitrate);
	}

	targetBitrate = std::min(maxBitrate, std::max(minBitrate, targetBitrate));

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
	for (int n = 0; n < nStreams; n++) {
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
	for (int n = 0; n < nStreams; n++) {
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
			int rtpSz = tmp->rtpQueue->sizeOfNextRtp();
			if (tmp->credit >= std::max(maxCredit, (float)rtpSz)) {
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
	for (int n = 0; n < nStreams; n++) {
		Stream *tmp = streams[n];
		float priority = tmp->targetPriority;
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
	for (int n = 0; n < nStreams; n++) {
		Stream *tmp = streams[n];
		if (tmp != servedStream) {
			float credit = transmittedBytes*tmp->targetPriority / servedStream->targetPriority;
			if (tmp->rtpQueue->sizeOfNextRtp() > 0) {
				tmp->credit += credit;
			}
			else {
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
	servedStream->credit = std::max(0.0f, servedStream->credit - transmittedBytes);
}

/*
* Update the  congestion window
*/
void ScreamTx::updateCwnd(uint64_t time_us) {
	/*
	* Compute the queuing delay
	*/
	uint64_t tmp = estimateOwd(time_us) - getBaseOwd();
	/*
	* Convert from [jiffy] OWD to an OWD in [s]
	*/
	queueDelay = tmp / kTimestampRate;


	if (queueDelay > std::max(0.2f, 2 * getSRtt()) && time_us - baseOwdResetT_us > 10000000) {
		/*
		* The base OWD is likely wrong, for instance due to
		* a channel change, reset base OWD history
		*/
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
	* Save to queue delay fraction history
	* used in computeQueueDelayTrend()
	*/
	if ((time_us - lastAddToQueueDelayFractionHistT_us) >
		kQueueDelayFractionHistInterval_us) {
		queueDelayFractionHist[queueDelayFractionHistPtr] = getQueueDelayFraction();
		queueDelayFractionHistPtr = (queueDelayFractionHistPtr + 1) % kQueueDelayFractionHistSize;
		computeQueueDelayTrend();

		queueDelayTrendMem = std::max(queueDelayTrendMem*0.99f, queueDelayTrend);

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
				lossEventRate = 0.99*lossEventRate + 0.01;
			else
				lossEventRate *= 0.99;
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
				*/
				if (bytesInFlight()*1.5 > cwnd)
					cwnd += bytesNewlyAcked;
			}
			else {
				inFastStart = false;
			}
		}
		else {
			if (offTarget > 0.0f) {
				/*
				* queue delay below target, increase CWND if window
				* is used to 1/1.25 = 80%
				*/
				if (bytesInFlight()*1.25 > cwnd) {
					float increment = kGainUp * offTarget * bytesNewlyAcked * mss / cwnd;
					cwnd += (int)(increment + 0.5f);
				}
			}
			else {
				/*
				* queue delay above target
				*/
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
	float maxBytesInFlight = (maxBytesInFlightHi*(1.0 - queueDelayTrendMem) + maxBytesInFlightLo*queueDelayTrendMem)*
		kMaxBytesInFlightHeadRoom;
	if (maxBytesInFlight > 5000) {
		cwnd = std::min(cwnd, (int)maxBytesInFlight);
	}

	if (getSRtt() < 0.01f && queueDelayTrend < 0.1) {
		int tmp = rateTransmitted*0.01f / 8;
		tmp = std::max(tmp, (int)(maxBytesInFlight*1.5f));
		cwnd = std::max(cwnd, tmp);
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
	if (time_us - lastBaseOwdAddT_us >= 1000000) {
		baseOwdHist[baseOwdHistPtr] = baseOwd;
		baseOwdHistPtr = (baseOwdHistPtr + 1) % kBaseOwdHistSize;
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
	for (int n = 0; n < kBaseOwdHistSize; n++)
		ret = std::min(ret, baseOwdHist[n]);
	return ret;
}

/*
* Compute current number of bytes in flight
*/
int ScreamTx::bytesInFlight() {
	int ret = 0;
	for (int n = 0; n < kMaxTxPackets; n++) {
		if (txPackets[n].isUsed == true)
			ret += txPackets[n].size;
	}
	return ret;
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
