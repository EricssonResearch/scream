#include "RtpQueue.h"
#include "ScreamTx.h"
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

const char *log_tag = "scream_lib";

// === Some good to have features, SCReAM works also
//     with these disabled
// Fast start can resume if little or no congestion detected
static const bool kEnableConsecutiveFastStart = true;
// Packet pacing reduces jitter
static const bool kEnablePacketPacing = true;

// Rate update interval
static const uint32_t kRateAdjustInterval_ntp = 3277; // 50ms in NTP domain

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
static const float kBytesInFlightHistInterval_ntp = 65536; // Time (s) between stores=1s in NTP doain
static const float kMaxBytesInFlightHeadRoom = 1.25f;
// Queue delay trend and shared bottleneck detection
static const uint32_t kQueueDelayFractionHistInterval_us = 3277; // 50ms in NTP domain
// video rate estimation update period
static const uint32_t kRateUpdateInterval_ntp = 3277;  // 50ms in NTP domain

// Packet reordering margin (us)
static const uint32_t kReordertime_ntp = 655;  // 10ms in NTP domain
static const uint32_t kMinRtpQueueDiscardInterval_ntp = 16384; // 0.25s in NTP doain

// Update interval for base delay history
static const uint32_t kBaseDelayUpdateInterval_ntp = 655360; // 10s in NTP doain

// L4S alpha gain factor, for scalable congestion control
static const float kL4sG = 0.125f;

// L4S alpha max value, for scalable congestion control
static const float kL4sAlphaMax = 0.25;

// Min CWND in MSS
static const int kMinCwndMss = 3;

// Compensate for max 33/65536 = 0.05% clock drift
static const uint32_t kMaxClockdriftCompensation = 33;

// Time stamp scale
static const int kTimeStampAtoScale = 1024;
static const float ntp2SecScaleFactor = 1.0 / 65536;

ScreamTx::ScreamTx(float lossBeta_,
	float ecnCeBeta_,
	float queueDelayTargetMin_,
	bool enableSbd_,
	float gainUp_,
	float gainDown_,
	int cwnd_,
	float packetPacingHeadroom_,
	int bytesInFlightHistSize_,
	bool isL4s_,
	bool openWindow_,
	bool enableClockDriftCompensation_
) :
	sRttSh_ntp(0),
	lossBeta(lossBeta_),
	ecnCeBeta(ecnCeBeta_),
	queueDelayTargetMin(queueDelayTargetMin_),
	enableSbd(enableSbd_),
	gainUp(gainUp_),
	gainDown(gainDown_),
	packetPacingHeadroom(packetPacingHeadroom_),
	bytesInFlightHistSize(bytesInFlightHistSize_),
	isL4s(isL4s_),
	openWindow(openWindow_),
	enableClockDriftCompensation(enableClockDriftCompensation_),
	sRtt_ntp(0),
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
	cwndMinLow(0),
	lastBytesInFlightT_ntp(0),
	bytesInFlightMaxLo(0),
	bytesInFlightMaxHi(0),
	bytesInFlightHistLoMem(0),
	bytesInFlightHistHiMem(0),
	maxBytesInFlight(0.0f),

	lossEvent(false),
	wasLossEvent(false),
	lossEventRate(0.0),
	ecnCeEvent(false),
	isCeThisFeedback(false),
	l4sAlpha(0.1f),
	bytesMarkedThisRtt(0),
	bytesDeliveredThisRtt(0),
	lastL4sAlphaUpdateT_ntp(0),
	inFastStart(true),
	maxTotalBitrate(0.0f),

	paceInterval_ntp(0),
	paceInterval(0.0),
	rateTransmittedAvg(0.0),

	isInitialized(false),
	lastSRttUpdateT_ntp(0),
	lastBaseOwdAddT_ntp(0),
	baseOwdResetT_ntp(0),
	lastAddToQueueDelayFractionHistT_ntp(0),
	lastLossEventT_ntp(0),
	lastTransmitT_ntp(0),
	nextTransmitT_ntp(0),
	lastRateUpdateT_ntp(0),
	accBytesInFlightMax(0),
	nAccBytesInFlightMax(0),
	rateTransmitted(0.0f),
	rateAcked(0.0f),
	queueDelayTrendMem(0.0f),
	lastCwndUpdateT_ntp(0),
	bytesInFlight(0),
	bytesInFlightLog(0),
	lastBaseDelayRefreshT_ntp(0),
	maxRate(0.0f),
	baseOwdHistMin(UINT32_MAX),
	clockDriftCompensation(0),
	clockDriftCompensationInc(0),
	enableRateUpdate(true),

	bytesNewlyAckedLog(0),
	ecnCeMarkedBytesLog(0),
	isUseExtraDetailedLog(false),
	fp_log(0),
	completeLogItem(false)
{
	strcpy(detailedLogExtraData, "");
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
	sumRateTx = 0.0f;
	sumRateLost = 0.0f;
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
	sumRateTx += rateTx;
	sumRateLost += rateLost;
	if (avgRateTx == 0.0f) {
		avgRateTx = rateTx;
		avgRtt = rtt;
		avgQueueDelay = queueDelay;
	}
	else {
		avgRateTx = alpha * avgRateTx + (1.0f - alpha)*rateTx;
		rateLostAcc += rateLost;
		rateLostN++;
		if (rateLostN == 10) {
			rateLostAcc /= 10;
			rateLostN = 0;
			float lossRate = 0.0f;
			if (rateTx > 0)
				lossRate = rateLostAcc / rateTx * 100.0f;
			lossRateHist[lossRateHistPtr] = lossRate;
			lossRateHistPtr = (lossRateHistPtr + 1) % kLossRateHistSize;
		}
		avgRtt = alpha * avgRtt + (1.0f - alpha)*rtt;
		avgQueueDelay = alpha * avgQueueDelay + (1.0f - alpha)*queueDelay;
	}
}

void ScreamTx::Statistics::getSummary(float time, char s[]) {
	float lossRate = 0.0f;
	for (int n = 0; n < kLossRateHistSize; n++)
		lossRate += lossRateHist[n];
	lossRate /= kLossRateHistSize;
	float lossRateLong = 0.0f;
	if (sumRateTx > 100000.0f) {
		lossRateLong = sumRateLost / sumRateTx * 100.0f;
	}
	sprintf(s, "%s summary %5.1f  Transmit rate = %5.0fkbps, PLR = %5.2f%%(%5.2f%%), RTT = %5.3fs, Queue delay = %5.3fs",
        log_tag,
		time,
		avgRateTx / 1000.0f,
		lossRate,
		lossRateLong,
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
	float rampUpScale,
	float maxRtpQueueDelay,
	float txQueueSizeFactor,
	float queueDelayGuard,
	float lossEventRateScale,
	float ecnCeEventRateScale,
	bool isAdaptiveTargetRateScale) {
	Stream *stream = new Stream(this,
		rtpQueue,
		ssrc,
		priority,
		minBitrate,
		startBitrate,
		maxBitrate,
		rampUpSpeed,
		rampUpScale,
		maxRtpQueueDelay,
		txQueueSizeFactor,
		queueDelayGuard,
		lossEventRateScale,
		ecnCeEventRateScale,
		isAdaptiveTargetRateScale);
	streams[nStreams++] = stream;
}

void ScreamTx::updateBitrateStream(uint32_t ssrc,
	float minBitrate,
	float maxBitrate) {
	int id;
	Stream *stream = getStream(ssrc, id);
	stream->minBitrate = minBitrate;
	stream->maxBitrate = maxBitrate;
}

RtpQueueIface * ScreamTx::getStreamQueue(uint32_t ssrc) {
	int id;
	Stream* stream = getStream(ssrc, id);
	if (stream == 0)
		return 0;
	else
		return stream->rtpQueue;
}


/*
* New media frame
*/
void ScreamTx::newMediaFrame(uint32_t time_ntp, uint32_t ssrc, int bytesRtp) {
	if (!isInitialized) initialize(time_ntp);

	int id;
	Stream *stream = getStream(ssrc, id);
	stream->updateTargetBitrate(time_ntp);
	if (time_ntp - lastCwndUpdateT_ntp < 32768) { // 32768 = 0.5s in NTP domain
		/*
		* We expect feedback at least every 500ms
		* to update the target rate.
		*/
		stream->updateTargetBitrate(time_ntp);
	}
	if (time_ntp - lastBaseDelayRefreshT_ntp < sRtt_ntp * 2) {
		/*
		* _Very_ long periods of congestion can cause the base delay to increase
		* with the effect that the queue delay is estimated wrong, therefore we seek to
		* refresh the whole thing by deliberately allowing the network queue to drain
		* Clear the RTP queue for 2 RTTs, this will allow the queue to drain so that we
		* get a good estimate for the min queue delay.
		* This funtion is executed very seldom so it should not affect overall experience too much
		*/
		int cur_cleared = stream->rtpQueue->clear();
        if (cur_cleared) {
            cerr << log_tag << " refresh " << time_ntp / 65536.0f << " RTP queue " << cur_cleared  << " packetes discarded for SSRC " << ssrc << endl;
            stream->cleared += cur_cleared;
        }
	}
	else {
		stream->bytesRtp += bytesRtp;
		stream->packetsRtp++;
		/*
		* Need to update MSS here, otherwise it will be nearly impossible to
		* transmit video packets, this because of the small initial MSS
		* which is necessary to make SCReAM work with audio only
		*/
		int sizeOfNextRtp = stream->rtpQueue->sizeOfNextRtp();
		mss = std::max(mss, sizeOfNextRtp);
		if (!openWindow)
			cwndMin = std::max(cwndMinLow, 2 * mss);
		cwnd = max(cwnd, cwndMin);
	}
}

/*
* Determine if OK to transmit RTP packet
*/
float ScreamTx::isOkToTransmit(uint32_t time_ntp, uint32_t &ssrc) {
	if (!isInitialized) initialize(time_ntp);
	/*
	* Update rateTransmitted and rateAcked if time for it
	* This is used in video rate computation
	* The update interval is doubled at very low bitrates,
	* the reason is that the feedback interval is very low then and
	* a longer window is needed to avoid aliasing effects
	*/
	uint32_t tmp = kRateUpdateInterval_ntp;

	if (rateAcked < 50000.0f) {
		tmp *= 2;
	}

	if (time_ntp - lastRateUpdateT_ntp > tmp) {
		rateTransmitted = 0.0f;
		rateAcked = 0.0f;
		float rateRtp = 0.0f;
		for (int n = 0; n < nStreams; n++) {
			streams[n]->updateRate(time_ntp);
			rateTransmitted += streams[n]->rateTransmitted;
			rateRtp += streams[n]->rateRtp;
			rateTransmittedAvg = 0.9f*rateTransmittedAvg + 0.1f*rateTransmitted;
			rateAcked += streams[n]->rateAcked;
			if (n == 0)
				statistics->add(streams[0]->rateTransmitted, streams[0]->rateLost, sRtt, queueDelay);
		}
		lastRateUpdateT_ntp = time_ntp;
		/*
		* Adjust stream priorities
		*/
		adjustPriorities(time_ntp);

		/*
		* Update maxRate
		*/
		maxRate = 0.0f;
		for (int n = 0; n < nStreams; n++)
			maxRate += streams[n]->getMaxRate();

		/*
		* Updated target bitrates if total RTP bitrate
		* exceeds maxTotalBitrate
		*/
		if (maxTotalBitrate > 0) {
			float tmp = maxTotalBitrate * 0.9f;
			if (rateRtp > tmp) {
				inFastStart = false;
				/*
				* Use a little safety margin because the video coder
				* can occasionally send at a higher bitrate than the target rate
				*/
				float delta = (rateRtp - tmp) / tmp;
				delta /= kRateUpDateSize;
				/*
				* The target bitrate for each stream should be adjusted down by the same fraction
				*/
				for (int n = 0; n < nStreams; n++) {
					Stream* stream = streams[n];
					stream->targetBitrate *= (1.0f - delta);
					stream->targetBitrateI = streams[n]->targetBitrate;
					stream->lastBitrateAdjustT_ntp = time_ntp;
				}
			}
		}
	}

	/*
	* Get index to the prioritized RTP queue
	*/
	Stream* stream = getPrioritizedStream(time_ntp);

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
	if (time_ntp - lastBytesInFlightT_ntp > kBytesInFlightHistInterval_ntp) {
		bytesInFlightMaxLo = 0;
		if (nAccBytesInFlightMax > 0) {
			bytesInFlightMaxLo = accBytesInFlightMax / nAccBytesInFlightMax;
		}
		bytesInFlightHistLo[bytesInFlightHistPtr] = bytesInFlightMaxLo;
		bytesInFlightHistHi[bytesInFlightHistPtr] = bytesInFlightMaxHi;
		bytesInFlightHistPtr = (bytesInFlightHistPtr + 1) % bytesInFlightHistSize;
		lastBytesInFlightT_ntp = time_ntp;
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
			cwndMin = std::max(cwndMinLow, kMinCwndMss * mss);
		cwnd = max(cwnd, cwndMin);

		/*
		* Add a small clock drift compensation
		*  for the case that the receiver clock is faster
		*/
		clockDriftCompensation += clockDriftCompensationInc;
	}

	int sizeOfNextRtp = stream->rtpQueue->sizeOfNextRtp();
	if (sizeOfNextRtp == -1) {
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
	uint32_t tmp_l = nextTransmitT_ntp - time_ntp;
	if (kEnablePacketPacing && (nextTransmitT_ntp > time_ntp) && (tmp_l < 0xFFFF0000)) {
		retVal = (nextTransmitT_ntp - time_ntp) * ntp2SecScaleFactor;
	}

	/*
	* A retransmission time out mechanism to avoid deadlock
	*/
	if (time_ntp - lastTransmitT_ntp > 32768 && lastTransmitT_ntp < time_ntp) { // 500ms in NTP domain
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
float ScreamTx::addTransmitted(uint32_t time_ntp,
	uint32_t ssrc,
	int size,
	uint16_t seqNr,
	bool isMark) {
	if (!isInitialized)
		initialize(time_ntp);


	int id;
	Stream *stream = getStream(ssrc, id);

	int ix = seqNr % kMaxTxPackets;
	Transmitted *txPacket = &(stream->txPackets[ix]);
	stream->hiSeqTx = seqNr;
	txPacket->timeTx_ntp = time_ntp;
	txPacket->size = size;
	txPacket->seqNr = seqNr;
	txPacket->isMark = isMark;
	txPacket->isUsed = true;
	txPacket->isAcked = false;
	txPacket->isAfterReceivedEdge = false;

	/*
	* Update bytesInFlight
	*/
	bytesInFlight += size;
	bytesInFlightLog = std::max(bytesInFlightLog, bytesInFlight);

	stream->bytesTransmitted += size;
	lastTransmitT_ntp = time_ntp;
	stream->lastTransmitT_ntp = time_ntp;
	/*
	* Add credit to unserved streams
	*/
	addCredit(time_ntp, stream, size);
	/*
	* Reduce used credit for served stream
	*/
	subtractCredit(time_ntp, stream, size);

	/*
	* Update MSS and cwndMin
	*/
	mss = std::max(mss, size);
	if (!openWindow)
		cwndMin = std::max(cwndMinLow, 2 * mss);
	cwnd = max(cwnd, cwndMin);

	/*
	* Determine when next RTP packet can be transmitted
	*/
	if (kEnablePacketPacing)
		nextTransmitT_ntp = time_ntp + paceInterval_ntp;
	else
		nextTransmitT_ntp = time_ntp;
	return paceInterval;
}

//extern uint32_t getTimeInNtp();
static uint32_t    unused;
static uint32_t time_ntp_prev = 0;
void ScreamTx::incomingStandardizedFeedback(uint32_t time_ntp,
	unsigned char* buf,
	int size) {

	if (!isInitialized) initialize(time_ntp);
	int ptr = 2;
	/*
	* read length in 32bit words
	*/
	uint16_t length;
	memcpy(&length, buf + ptr, 2);
	length = ntohs(length);
	ptr += 2;
	/*
	* read RTCP sender SSRC
	*/
	uint32_t ssrc_rtcp;
	memcpy(&ssrc_rtcp, buf + ptr, 4);
	ssrc_rtcp = ntohl(ssrc_rtcp);
	ptr += 4;
	/*
	* read report timestamp, it is located at the very end
	*/
	uint32_t rts;
	memcpy(&rts, buf + length * 4, 4);
	rts = ntohl(rts);
	//printf(" TS %X\n", rts);


	while (ptr != size - 4) {
		/*
		* read RTP source (stream) SSRC
		*/
		uint32_t ssrc;
		memcpy(&ssrc, buf + ptr, 4);
		ssrc = ntohl(ssrc);
		ptr += 4;
		/*
		* read begin_seq and end_seq
		*/
		uint16_t begin_seq, end_seq;
		uint16_t num_reports;
		memcpy(&begin_seq, buf + ptr, 2);
		ptr += 2;
		memcpy(&num_reports, buf + ptr, 2);
		ptr += 2;
		begin_seq = ntohs(begin_seq);
		num_reports = ntohs(num_reports) + 1;
		end_seq = begin_seq + num_reports - 1;

		/*
		* Validate RTCP feedback message
		* Discard out of order RTCP feedback,
		*  they are quite rate but will mess up the entire
		*  feedback handling if they are not taken care of.
		*/
		int streamId = -1;
		Stream *stream = getStream(ssrc, streamId);
		if (stream == 0) {
			/*
			* Bogus RTCP?, the SSRC is wrong anyway, Skip
			*/
			return;
		}

		uint16_t diff = end_seq - stream->hiSeqAck;

		if (diff > 65000 && stream->hiSeqAck != 0 && stream->timeTxAck_ntp != 0) {
			/*
			* Out of order received ACKs are ignored
			*/
			return;
		}
        uint32_t size_before = stream->rtpQueue->sizeOfQueue();
		uint16_t N = end_seq - begin_seq;

        uint16_t nRx = 0;
        uint16_t first = 0;
        uint16_t last = 0;
        unused = 0;
		for (int n = 0; n <= N; n++) {
			uint16_t sn = begin_seq + n;
			uint16_t tmp_s;
			memcpy(&tmp_s, buf + ptr, 2);
			tmp_s = ntohs(tmp_s);
			ptr += 2;
			bool isRx = ((tmp_s & 0x8000) == 0x8000);
			if (isRx) {
                if (first == 0) {
                    first = n;
                }
                last = n;
                nRx++;
				/*
				* packet indicated as being received
				*/
				uint8_t ceBits = (tmp_s & 0x6FFF) >> 13;
				uint32_t rxTime = (tmp_s & 0x1FFF) << 6; // Q10->Q16
				rxTime = rts - rxTime;
				incomingStandardizedFeedback(time_ntp, streamId, rxTime, sn, ceBits, n == N);
			}
		}
        if (isUseExtraDetailedLog) {
            time_ntp = time_ntp;
            float rtpQueueDelay = stream->rtpQueue->getDelay(time_ntp * ntp2SecScaleFactor);
            int Last = stream->rtpQueue->seqNrOfLastRtp();
            int pak_diff = (Last == -1) ? -1 : ((Last >=  stream->hiSeqTx ) ? (Last -  stream->hiSeqTx ) : Last + 0xffff -  stream->hiSeqTx);
            printf("%s diff %d %6u %u beg_seq %u num_reps %u end_seq %u nRx %u unused %u sQ %d first %u last %u NrNext %d Last %d hiSeqTx %u hiSeqAck %u packetsRtp %lu rtpQueueDelay %f sRtt %f\n",
                   log_tag, pak_diff,
                   time_ntp - time_ntp_prev, time_ntp, begin_seq, num_reports, end_seq, nRx, unused, size_before,
                   first, last, stream->rtpQueue->seqNrOfNextRtp(), Last,
                   stream->hiSeqTx, stream->hiSeqAck, stream->packetsRtp, rtpQueueDelay, sRtt);
            time_ntp_prev = time_ntp;
        }
		/*
		* Skip zeropadded two octets if odd number of octets reported for this SSRC
		*/
		if ((N + 1) % 2 == 1) {
			ptr += 2;
		}
	}
}

/*
* New incoming feedback
*/
void ScreamTx::incomingStandardizedFeedback(uint32_t time_ntp,
	int streamId,
	uint32_t timestamp,
	uint16_t seqNr,
	uint8_t ceBits,
	bool isLast) {

	Stream *stream = streams[streamId];
	accBytesInFlightMax += bytesInFlight;
	nAccBytesInFlightMax++;
	Transmitted *txPackets = stream->txPackets;
	completeLogItem = false;
	/*
	* Mark received packets, given by the ACK vector
	*/
	bool isMark = false;
	isCeThisFeedback |= markAcked(time_ntp, txPackets, seqNr, timestamp, stream, ceBits, ecnCeMarkedBytesLog, isLast, isMark);

	/*
	* Detect lost packets
	*/
	if (isUseExtraDetailedLog || isLast || isMark) {
		detectLoss(time_ntp, txPackets, seqNr, stream);
	}

	if (isLast) {
		if (isCeThisFeedback && time_ntp - lastLossEventT_ntp > sRtt_ntp) {
			ecnCeEvent = true;
			lastLossEventT_ntp = time_ntp;
		}
		isCeThisFeedback = false;
		if (isL4s) {
			/*
			* L4S mode compute a congestion scaling factor that is dependent on the fraction
			*  of ECN marked packets
			*/
			if (time_ntp - lastL4sAlphaUpdateT_ntp > sRtt_ntp) {
				lastL4sAlphaUpdateT_ntp = time_ntp;
				if (bytesDeliveredThisRtt > 0) {
					float F = float(bytesMarkedThisRtt) / float(bytesDeliveredThisRtt);
					/*
					 * L4S alpha (backoff factor) is averaged and limited
					 * It makes sense to limit the backoff because
					 *   1) source is rate limited
					 *   2) delay estimation algorithm also works in parallel
					 *   3) L4S marking algorithm can lag behind a little and potentially overmark
					 */
					l4sAlpha = std::min(kL4sAlphaMax, kL4sG * F + (1.0f - kL4sG)*l4sAlpha);
					bytesDeliveredThisRtt = 0;
					bytesMarkedThisRtt = 0;
				}
			}
		}

		if (lossEvent || ecnCeEvent) {
			lastLossEventT_ntp = time_ntp;
			for (int n = 0; n < nStreams; n++) {
				Stream *tmp = streams[n];
				if (lossEvent)
					tmp->lossEventFlag = true;
				else
					tmp->ecnCeEventFlag = true;
			}
		}

		if (lastCwndUpdateT_ntp == 0)
			lastCwndUpdateT_ntp = time_ntp;

		if (time_ntp - lastCwndUpdateT_ntp > 655) { // 10ms in NTP domain
			/*
			* There is no gain with a too frequent CWND update
			* An update every 10ms is fast enough even at very high high bitrates
			*/
			updateCwnd(time_ntp);
			lastCwndUpdateT_ntp = time_ntp;
		}

	}
	if (isUseExtraDetailedLog || isLast || isMark) {

		if (fp_log && completeLogItem) {
			fprintf(fp_log, " %d,%d,%d,%1.0f,%d,%d,%d,%d,%1.0f,%1.0f,%1.0f,%1.0f,%1.0f,%d",
                    cwnd, bytesInFlight, inFastStart, rateTransmitted, streamId, seqNr, bytesNewlyAckedLog, ecnCeMarkedBytesLog, stream->rateRtp, stream->rateTransmitted, stream->rateAcked, stream->rateLost, stream->rateCe, isMark);
			if (strlen(detailedLogExtraData) > 0) {
				fprintf(fp_log, ",%s", detailedLogExtraData);
			}
			bytesNewlyAckedLog = 0;
			ecnCeMarkedBytesLog = 0;
		}
		if (fp_log && completeLogItem) {
			fprintf(fp_log, "\n");
		}
	}

}

/*
*  Mark ACKed RTP packets
*/
bool ScreamTx::markAcked(uint32_t time_ntp,
	struct Transmitted *txPackets,
	uint16_t seqNr,
	uint32_t timestamp,
	Stream *stream,
	uint8_t ceBits,
	int &encCeMarkedBytesLog,
	bool isLast,
	bool &isMark) {

	bool isCe = false;
	int ix = seqNr % kMaxTxPackets;
	if (txPackets[ix].isUsed) {
		/*
		* RTP packet is in flight
		*/
		Transmitted *tmp = &txPackets[ix];

		/*
		* Receiption of packet given by seqNr
		*/
		if ((tmp->seqNr == seqNr) & !tmp->isAcked) {
			bytesDeliveredThisRtt += tmp->size;
			isMark = tmp->isMark;
			if (ceBits == 0x03) {
				/*
				* Packet was CE marked, increase counter
				*/
				encCeMarkedBytesLog += tmp->size;
				bytesMarkedThisRtt += tmp->size;
				stream->bytesCe += tmp->size;
                stream->packetsCe++;
				isCe = true;
			}
			tmp->isAcked = true;
			ackedOwd = timestamp - tmp->timeTx_ntp;

			/*
			* Compute the queue delay i NTP domain (Q16)
			*/
			estimateOwd(time_ntp);
			/*
			* Small compensation for positive clock drift
			*/
			ackedOwd -= clockDriftCompensation;

			uint32_t qDel = ackedOwd - getBaseOwd();

			if (qDel > 0xFFFF0000 && clockDriftCompensation != 0) {
				/*
				* We have the case that the clock drift compensation is too large as it gives negative queue delays
				*  reduce the clock drift compensation and restore qDel to 0
				*/
				uint32_t diff = 0 - qDel;
				clockDriftCompensation -= diff;
				qDel = 0;
			}

			if (qDel > 0xFFFF0000 || clockDriftCompensation > 0xFFFF0000) {
				/*
				* TX and RX clock diff made qDel wrap around
				*  reset history
				*/
				clockDriftCompensation = 0;
				clockDriftCompensationInc = 0;
				queueDelayMinAvg = 0.0f;
				queueDelay = 0.0f;
				for (int n = 0; n < kBaseOwdHistSize; n++)
					baseOwdHist[n] = UINT32_MAX;
				baseOwd = UINT32_MAX;
				baseOwdHistMin = UINT32_MAX;
				baseOwdResetT_ntp = time_ntp;
				qDel = 0;
			}

			/*
			* Convert from NTP domain OWD to an OWD in [s]
			*/
			queueDelay = qDel * ntp2SecScaleFactor;

			uint32_t rtt = time_ntp - tmp->timeTx_ntp;

			if (fp_log && (isUseExtraDetailedLog || isLast || isMark)) {
				fprintf(fp_log, "%s,%1.4f,%1.4f,", timeString, queueDelay, rtt*ntp2SecScaleFactor);
				completeLogItem = true;
			}
			if (rtt < 1000000 && isLast) {
				sRttSh_ntp = (7 * sRttSh_ntp + rtt) / 8;
				if (time_ntp - lastSRttUpdateT_ntp > sRttSh_ntp) {
					sRtt_ntp = (7 * sRtt_ntp + sRttSh_ntp) / 8;
					lastSRttUpdateT_ntp = time_ntp;
					sRtt = sRtt_ntp * ntp2SecScaleFactor;
				}
			}
			stream->timeTxAck_ntp = tmp->timeTx_ntp;
		}
	} else {
        unused++;
    }
	return isCe;
}

/*
* Detect lost RTP packets
*/
void ScreamTx::detectLoss(uint32_t time_ntp, struct Transmitted *txPackets, uint16_t highestSeqNr, Stream *stream) {
	/*
	* Loop only through the packets that are covered by the last highest ACK, this saves complexity
	* There is a faint possibility that we miss to detect large bursts of lost packets with this fix
	*/
	int ix1 = highestSeqNr; ix1 = ix1 % kMaxTxPackets;
	int ix0 = stream->hiSeqAck - 256;
	stream->hiSeqAck = highestSeqNr;
	if (ix0 < 0) ix0 += kMaxTxPackets;
	while (ix1 < ix0)
		ix1 += kMaxTxPackets;

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
				bytesNewlyAckedLog += tmp->size;
				bytesInFlight -= tmp->size;
				if (bytesInFlight < 0)
					bytesInFlight = 0;
				stream->bytesAcked += tmp->size;
				tmp->isAfterReceivedEdge = true;
			}

			if (tmp->timeTx_ntp + kReordertime_ntp < stream->timeTxAck_ntp && !tmp->isAcked) {
				/*
				* Packet ACK is delayed more than kReordertime_ntp after an ACK of a higher SN packet
				* raise a loss event and remove from TX list
				*/
				if (time_ntp - lastLossEventT_ntp > sRtt_ntp && lossBeta < 1.0f) {
					lossEvent = true;
				}
				stream->bytesLost += tmp->size;
				stream->packetLost++;
				tmp->isUsed = false;
				cerr << log_tag << " LOSS detected by reorder timer SSRC=" << stream->ssrc << " SN=" << tmp->seqNr << endl;
				stream->repairLoss = true;
			}
			else if (tmp->isAcked) {
				tmp->isUsed = false;
			}
		}
	}
}

float ScreamTx::getTargetBitrate(uint32_t ssrc) {
	int id;
	return  getStream(ssrc, id)->getTargetBitrate();
}

void ScreamTx::setTargetPriority(uint32_t ssrc, float priority) {
	int id;
	Stream *stream = getStream(ssrc, id);
	if (queueDelayFractionAvg > 0.1 || !inFastStart) {
		stream->targetBitrate *= priority / stream->targetPriority;
		stream->targetBitrate = std::min(std::max(stream->targetBitrate, stream->minBitrate), stream->maxBitrate);
	}
	stream->targetPriority = priority;
	stream->targetPriorityInv = 1.0f / priority;
}

void ScreamTx::getLogHeader(char *s) {
	sprintf(s,
            "LogName,queueDelay,queueDelayMax,queueDelayMinAvg,sRtt,cwnd,bytesInFlightLog,rateTransmitted,isInFastStart,rtpQueueDelay,bytes,size,targetBitrate,rateRtp,packetsRtp,rateTransmittedStream,rateAcked,rateLost,rateCe, packetsCe,hiSeqAck,packetetsRtpCleared,packetsLost");
}

void ScreamTx::getLog(float time, char *s) {
	int inFlightMax = std::max(bytesInFlight, bytesInFlightHistHiMem);
	sprintf(s, "%s Log, %4.3f, %4.3f, %4.3f, %4.3f, %6d, %6d, %6.0f, %1d, ",
        log_tag, queueDelay, queueDelayMax, queueDelayMinAvg, sRtt,
		cwnd, bytesInFlightLog, rateTransmitted / 1000.0f, isInFastStart());
	bytesInFlightLog = bytesInFlight;
	queueDelayMax = 0.0;
	for (int n = 0; n < nStreams; n++) {
		Stream *tmp = streams[n];
		char s2[200];
		sprintf(s2, "%4.3f, %d,%d,%6.0f, %6.0f, %lu, %6.0f, %6.0f, %5.0f, %5.0f, %lu, %5d, %lu,%lu",
			std::max(0.0f, tmp->rtpQueue->getDelay(time)),
            tmp->rtpQueue->bytesInQueue(),
            tmp->rtpQueue->sizeOfQueue(),
			tmp->targetBitrate / 1000.0f, tmp->rateRtp / 1000.0f,
            tmp->packetsRtp,
			tmp->rateTransmitted / 1000.0f, tmp->rateAcked / 1000.0f,
			tmp->rateLost / 1000.0f, tmp->rateCe / 1000.0f,
            tmp->packetsCe,
            tmp->hiSeqAck,
            tmp->cleared, tmp->packetLost);
		strcat(s, s2);
	}
}

void ScreamTx::getShortLog(float time, char *s) {
	int inFlightMax = std::max(bytesInFlight, bytesInFlightHistHiMem);
	sprintf(s, "%s ShortLog %4.3f, %4.3f, %6d, %6d, %6.0f, %1d, ",
        log_tag, queueDelay, sRtt,
		cwnd, bytesInFlightLog, rateTransmitted / 1000.0f, isInFastStart());
	bytesInFlightLog = bytesInFlight;
	queueDelayMax = 0.0;
	for (int n = 0; n < nStreams; n++) {
		Stream *tmp = streams[n];
		char s2[200];
		sprintf(s2, "%4.3f, %6.0f, %6.0f, %6.0f, %5.0f, %5.0f, ",
			std::max(0.0f, tmp->rtpQueue->getDelay(time)),
			tmp->targetBitrate / 1000.0f, tmp->rateRtp / 1000.0f,
			tmp->rateTransmitted / 1000.0f,
			tmp->rateLost / 1000.0f, tmp->rateCe / 1000.0f);
		strcat(s, s2);
	}
}

void ScreamTx::getVeryShortLog(float time, char *s) {
	int inFlightMax = std::max(bytesInFlight, bytesInFlightHistHiMem);
	sprintf(s, "%s VeryShortLog %4.3f, %4.3f, %6d, %6d, %6.0f, ",
        log_tag, queueDelay, sRtt,
		cwnd, bytesInFlightLog, rateTransmitted / 1000.0f);
	bytesInFlightLog = bytesInFlight;
	queueDelayMax = 0.0;
	for (int n = 0; n < 1; n++) {
		Stream *tmp = streams[n];
		char s2[200];
		sprintf(s2, "%5.0f, %5.0f, ",
			tmp->rateLost / 1000.0f, tmp->rateCe / 1000.0f);
		strcat(s, s2);
	}
}

void ScreamTx::getStatistics(float time, char *s) {
	statistics->getSummary(time, s);
}

float ScreamTx::getQualityIndex(float time, float thresholdRate, float rttMin) {
	/*
	* The quality index is an approximate value of the streaming quality and takes the bitrate
	*  the RTT and the RTP queue delay into account.
	* The quality index is a value between 0 and 100% and gives an impression of the
	*  experienced quality e.g in remote control applications.
	* Note that this is not a MOS score!.
	*/
	float qualityIndex = std::max(0.0f, std::min(1.0f, (rateTransmitted - thresholdRate * 0.1f) / (thresholdRate*0.9f)));
	qualityIndex *= std::max(0.0f, (0.1f - streams[0]->rtpQueue->getDelay(time)) / 0.1f);
	qualityIndex *= std::max(0.0f, std::min(1.0f, (4 * rttMin - (sRtt - rttMin)) / (4 * rttMin)));
	return qualityIndex * 100.0f;
}

bool ScreamTx::isLossEpoch(uint32_t ssrc) {
	int id;
	return  getStream(ssrc, id)->isLossEpoch();
}

void ScreamTx::initialize(uint32_t time_ntp) {
	isInitialized = true;
	lastSRttUpdateT_ntp = time_ntp;
	lastBaseOwdAddT_ntp = time_ntp;
	baseOwdResetT_ntp = time_ntp;
	lastAddToQueueDelayFractionHistT_ntp = time_ntp;
	lastLossEventT_ntp = time_ntp;
	lastTransmitT_ntp = time_ntp;
	nextTransmitT_ntp = time_ntp;
	lastRateUpdateT_ntp = time_ntp;
	lastAdjustPrioritiesT_ntp = time_ntp;
	lastRttT_ntp = time_ntp;
	lastBaseDelayRefreshT_ntp = time_ntp - 1;
	lastL4sAlphaUpdateT_ntp = time_ntp;
	initTime_ntp = time_ntp;
}

float ScreamTx::getTotalTargetBitrate() {
	float totalTargetBitrate = 0.0f;
	for (int n = 0; n < nStreams; n++) {
		totalTargetBitrate += streams[n]->targetBitrate;
	}
	return totalTargetBitrate;
}

/*
* Update the  congestion window
*/
void ScreamTx::updateCwnd(uint32_t time_ntp) {
	float dT = 0.001f;
	if (lastCwndUpdateT_ntp == 0)
		lastCwndUpdateT_ntp = time_ntp;
	else
		dT = (time_ntp - lastCwndUpdateT_ntp) * ntp2SecScaleFactor;
	/*
	* This adds a limit to how much the CWND can grow, it is particularly useful
	* in case of short gliches in the transmission, in which a large chunk of data is delivered
	* in a burst with the effect that the estimated queue delay is low because it typically depict the queue
	* delay for the last non-delayed RTP packet. The rule is that the CWND cannot grow faster
	* than the 1.25 times the average amount of bytes that transmitted in the given feedback interval
	*/
	float bytesNewlyAckedLimited = float(bytesNewlyAcked);
	if (maxRate > 1.0e5f)
		bytesNewlyAckedLimited = std::min(bytesNewlyAckedLimited, 1.25f*maxRate*dT / 8.0f);
	else
		bytesNewlyAckedLimited = 2.0f*mss;

	queueDelayMin = std::min(queueDelayMin, queueDelay);

	queueDelayMax = std::max(queueDelayMax, queueDelay);

	float time = time_ntp * ntp2SecScaleFactor;
	if (queueDelayMinAvg > 0.25f*queueDelayTarget && time_ntp - baseOwdResetT_ntp > 1310720) { // 20s in NTP domain
		/*
		* The base OWD is likely wrong, for instance due to
		* a channel change or clock drift, reset base OWD history
		*/
		clockDriftCompensation = 0;
		clockDriftCompensationInc = 0;
		queueDelayMinAvg = 0.0f;
		queueDelay = 0.0f;
		for (int n = 0; n < kBaseOwdHistSize; n++)
			baseOwdHist[n] = UINT32_MAX;
		baseOwd = UINT32_MAX;
		baseOwdHistMin = UINT32_MAX;
		baseOwdResetT_ntp = time_ntp;
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
	if ((time_ntp - lastAddToQueueDelayFractionHistT_ntp) >
		kQueueDelayFractionHistInterval_us) {

		/*
		* compute paceInterval, we assume a min bw of 50kbps and a min tp of 1ms
		* for stable operation
		* this function implements the packet pacing
		*/
		paceInterval = kMinPaceInterval;
		if ((queueDelayFractionAvg > 0.02f || isL4s || maxTotalBitrate > 0) && kEnablePacketPacing) {
			float pacingBitrate = std::max(1.0e5f, packetPacingHeadroom*getTotalTargetBitrate());
			if (maxTotalBitrate > 0) {
				pacingBitrate = std::min(pacingBitrate, maxTotalBitrate);
			}
			float tp = (mss * 8.0f) / pacingBitrate;
			paceInterval = std::max(kMinPaceInterval, tp);
		}
		paceInterval_ntp = (uint32_t)(paceInterval * 65536); // paceinterval converted to NTP domain (Q16)

		if (queueDelayMin < queueDelayMinAvg)
			queueDelayMinAvg = queueDelayMin;
		else
			queueDelayMinAvg = 0.001f*queueDelayMin + 0.999f*queueDelayMinAvg;
		queueDelayMin = 1000.0f;
		/*
		* Need to duplicate insertion incase the feedback is sparse
		*/
		int nIter = (int)(time_ntp - lastAddToQueueDelayFractionHistT_ntp) / kQueueDelayFractionHistInterval_us;
		for (int n = 0; n < nIter; n++) {
			queueDelayFractionHist[queueDelayFractionHistPtr] = getQueueDelayFraction();
			queueDelayFractionHistPtr = (queueDelayFractionHistPtr + 1) % kQueueDelayFractionHistSize;
		}

		if (time_ntp - initTime_ntp > 131072) { // 2s in NTP domain
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

		maxBytesInFlight =
			(maxBytesInFlightHi*(1.0f - queueDelayTrend) + maxBytesInFlightLo * queueDelayTrend)*
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
			float oh = queueDelayTargetMin * (queueDelaySbdMeanSh + sqrt(queueDelaySbdVar));
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
		lastAddToQueueDelayFractionHistT_ntp = time_ntp;
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
		lastCongestionDetectedT_ntp = time_ntp;

		inFastStart = false;
		wasLossEvent = true;
	}
	else if (ecnCeEvent) {
		/*
		* loss event detected, decrease congestion window
		*/
		if (isL4s) {
			cwnd = std::max(cwndMin, (int)((1.0f - l4sAlpha / 2.0f)*cwnd));
		}
		else {
			cwnd = std::max(cwndMin, (int)(ecnCeBeta*cwnd));
		}
		ecnCeEvent = false;
		lastCongestionDetectedT_ntp = time_ntp;

		inFastStart = false;
		wasLossEvent = true;
	}
	else {

		if (time_ntp - lastRttT_ntp > sRtt_ntp) {
			if (wasLossEvent)
				lossEventRate = 0.99f*lossEventRate + 0.01f;
			else
				lossEventRate *= 0.99f;
			wasLossEvent = false;
			lastRttT_ntp = time_ntp;
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
				float bytesInFlightMargin = 1.5f;
				if (bytesInFlight*bytesInFlightMargin + bytesNewlyAcked > cwnd) {
					cwnd += int(bytesNewlyAckedLimited);
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
				float bytesInFlightMargin = 1.2f;
				if (bytesInFlight*bytesInFlightMargin + bytesNewlyAcked > cwnd) {
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
				delta = std::min(delta, cwnd / 4.0f);
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
				lastCongestionDetectedT_ntp = time_ntp;
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

	cwnd = std::max(cwndMin, cwnd);

	/*
	* Make possible to enter fast start if OWD has been low for a while
	*/
	if (queueDelayTrend > 0.2) {
		lastCongestionDetectedT_ntp = time_ntp;
	}
	else if (time_ntp - lastCongestionDetectedT_ntp > 65536 && // 1s in NTP domain
		!inFastStart && kEnableConsecutiveFastStart) {
		/*
		* The queue delay trend has been low for more than 1.0s, resume fast start
		*/
		inFastStart = true;
		lastCongestionDetectedT_ntp = time_ntp;
	}
	bytesNewlyAcked = 0;
}

/*
* Update base OWD (if needed) and return the
* last estimated OWD (without offset compensation)
*/
void ScreamTx::estimateOwd(uint32_t time_ntp) {
	baseOwd = std::min(baseOwd, ackedOwd);
	if (time_ntp - lastBaseOwdAddT_ntp >= kBaseDelayUpdateInterval_ntp) {
		if (enableClockDriftCompensation) {
			/*
			* The clock drift compensation looks at how the baseOwd increases over time
			*  and calculates a compensation factor that advanced the sender clock stamps
			* Only positive drift (receiver side is faster) is compensated as negative
			* drift is already handled
			*/
			int ptr = baseOwdHistPtr;
			int k = 0;
			uint32_t bw0 = UINT32_MAX;
			for (k = 0; k < kBaseOwdHistSize - 1; k++) {
				if (baseOwdHist[ptr] != UINT32_MAX) {
					bw0 = baseOwdHist[ptr];
					ptr = ptr - 1;
					if (ptr < 0) ptr += kBaseOwdHistSize;
				}
				else
					break;
			}

			uint32_t bw1 = baseOwd;
			if (k > 0) {
				if ((bw1 > bw0))
					clockDriftCompensationInc = (bw1 - bw0) / (kBaseDelayUpdateInterval_ntp / 65536 * k);
				else
					clockDriftCompensationInc = 0;
				clockDriftCompensationInc = std::min(kMaxClockdriftCompensation, clockDriftCompensationInc);
				if (clockDriftCompensation == 0) {
					/*
					* Two update periods delayed with compensation so we add one update period worth of
					*  clock drift compensation
					*/
					clockDriftCompensation += clockDriftCompensationInc * (kBaseDelayUpdateInterval_ntp / 65536);
				}
			}
		}
		baseOwdHistPtr = (baseOwdHistPtr + 1) % kBaseOwdHistSize;
		baseOwdHist[baseOwdHistPtr] = baseOwd;
		lastBaseOwdAddT_ntp = time_ntp;
		baseOwd = UINT32_MAX;
		baseOwdHistMin = UINT32_MAX;
		for (int n = 0; n < kBaseOwdHistSize; n++)
			baseOwdHistMin = std::min(baseOwdHistMin, baseOwdHist[n]);
		/*
		* _Very_ long periods of congestion can cause the base delay to increase
		* with the effect that the queue delay is estimated wrong, therefore we seek to
		* refresh the whole thing by deliberately allowing the network queue to drain
		*/
		if (time_ntp - lastBaseDelayRefreshT_ntp > kBaseDelayUpdateInterval_ntp*(kBaseOwdHistSize - 1)) {
			lastBaseDelayRefreshT_ntp = time_ntp;
		}
	}
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
void ScreamTx::determineActiveStreams(uint32_t time_ntp) {
	float surplusBitrate = 0.0f;
	float sumPrio = 0.0;
	bool streamSetInactive = false;
	for (int n = 0; n < nStreams; n++) {
		if (time_ntp - streams[n]->lastFrameT_ntp > 65536 && streams[n]->isActive) { // 1s in NTP domain
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
					surplusBitrate * streams[n]->targetPriority / sumPrio);
			}
		}
	}
}

/*
* Add credit to streams that was not served
*/
void ScreamTx::addCredit(uint32_t time_ntp, Stream* servedStream, int transmittedBytes) {
	/*
	* Add a credit to stream(s) that did not get priority to transmit RTP packets
	*/
	if (nStreams == 1)
		/*
		* Skip if only one stream to save CPU
		*/
		return;
	int maxCredit = 5 * mss;
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
			}
		}
	}
}

/*
* Subtract credit from served stream
*/
void ScreamTx::subtractCredit(uint32_t time_ntp, Stream* servedStream, int transmittedBytes) {
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
* Adjust (enforce) proper prioritization between active streams
* at regular intervals. This is a necessary addon to mitigate
* issues that VBR media brings
* The function consists of equal measures or rational thinking and
* black magic, which means that there is no 100% guarantee that
* will always work.
*/
void ScreamTx::adjustPriorities(uint32_t time_ntp) {
	if (nStreams == 1 || time_ntp - lastAdjustPrioritiesT_ntp < 65536) { // 1s in NTP domain
		/*
		* Skip if only one stream or if adjustment done less than 5s ago
		*/
		return;
	}

	if (rateTransmitted > 0) {
		float totalPriority = 0.0f;
		for (int n = 0; n < nStreams; n++) {
			if (streams[n]->isActive)
				totalPriority += streams[n]->targetPriority;
		}

		for (int n = 0; n < nStreams; n++) {
			if (streams[n]->isActive) {
				float fairRate = rateTransmitted * streams[n]->targetPriority / totalPriority;
				if (streams[n]->rateRtp > fairRate*1.2f) {
					float scale = 1.0f - 0.1f*queueDelayTrend;
					streams[n]->targetBitrate = std::max(streams[n]->minBitrate, streams[n]->targetBitrate*scale);
				}
			}
		}
		lastAdjustPrioritiesT_ntp = time_ntp;
	}
}

/*
* Get the prioritized stream
*/
ScreamTx::Stream* ScreamTx::getPrioritizedStream(uint32_t time_ntp) {
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
	uint32_t maxDiff = 0;
	for (int n = 0; n < nStreams; n++) {
		Stream *tmp = streams[n];
		if (tmp->rtpQueue->sizeOfQueue() == 0) {
			/*
			* Queue empty
			*/
		}
		else {
			uint32_t diff = time_ntp - tmp->lastTransmitT_ntp;
			/*
			* Pick stream if it has the highest credit so far
			*/
			if (tmp->credit >= maxCredit && diff > maxDiff) {
				stream = tmp;
				maxDiff = diff;
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

ScreamTx::Stream::Stream(ScreamTx *parent_,
	RtpQueueIface *rtpQueue_,
	uint32_t ssrc_,
	float priority_,
	float minBitrate_,
	float startBitrate_,
	float maxBitrate_,
	float rampUpSpeed_,
	float rampUpScale_,
	float maxRtpQueueDelay_,
	float txQueueSizeFactor_,
	float queueDelayGuard_,
	float lossEventRateScale_,
	float ecnCeEventRateScale_,
	bool isAdaptiveTargetRateScale_) {
	parent = parent_;
	rtpQueue = rtpQueue_;
	ssrc = ssrc_;
	targetPriority = priority_;
	targetPriorityInv = 1.0f / targetPriority;
	minBitrate = minBitrate_;
	maxBitrate = maxBitrate_;
	targetBitrate = std::min(maxBitrate, std::max(minBitrate, startBitrate_));
	rampUpSpeed = rampUpSpeed_;
	rampUpScale = rampUpScale_;
	maxRtpQueueDelay = maxRtpQueueDelay_;
	txQueueSizeFactor = txQueueSizeFactor_;
	queueDelayGuard = queueDelayGuard_;
	lossEventRateScale = lossEventRateScale_;
	ecnCeEventRateScale = ecnCeEventRateScale_;
	isAdaptiveTargetRateScale = isAdaptiveTargetRateScale_;
	targetBitrateHistUpdateT_ntp = 0;
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
	rateCe = 0.0;
	lossEventFlag = false;
	ecnCeEventFlag = false;
	txSizeBitsAvg = 0.0f;
	lastRateUpdateT_ntp = 0;
	lastBitrateAdjustT_ntp = 0;
	lastTargetBitrateIUpdateT_ntp = 0;
	bytesRtp = 0;
	packetsRtp = 0;
	rateRtp = 0.0f;
	timeTxAck_ntp = 0;
	lastTransmitT_ntp = 0;
	numberOfUpdateRate = 0;
    cleared = 0;
    packetLost = 0;
	for (int n = 0; n < kRateUpDateSize; n++) {
		rateRtpHist[n] = 0.0f;
		rateAckedHist[n] = 0.0f;
		rateLostHist[n] = 0.0f;
		rateCeHist[n] = 0.0f;
		rateTransmittedHist[n] = 0.0f;
	}
	rateUpdateHistPtr = 0;
	for (int n = 0; n < kTargetBitrateHistSize; n++) {
		targetBitrateHist[n] = 0;
	}
	targetBitrateHistPtr = 0;
	targetRateScale = 1.0;
	isActive = false;
	lastFrameT_ntp = 0;
	initTime_ntp = 0;
	rtpQueueDiscard = false;
	lastRtpQueueDiscardT_ntp = 0;
	bytesLost = 0;
	bytesCe = 0;
	wasRepairLoss = false;
	repairLoss = false;
	for (int n = 0; n < kMaxTxPackets; n++)
		txPackets[n].isUsed = false;
	txPacketsPtr = 0;
	lossEpoch = false;
}

/*
* Update the estimated max media rate
*/
void ScreamTx::Stream::updateRate(uint32_t time_ntp) {
	if (lastRateUpdateT_ntp != 0 && parent->enableRateUpdate) {
		numberOfUpdateRate++;
		float tDelta = (time_ntp - lastRateUpdateT_ntp) * ntp2SecScaleFactor;
		rateTransmittedHist[rateUpdateHistPtr] = bytesTransmitted * 8.0f / tDelta;
		rateAckedHist[rateUpdateHistPtr] = bytesAcked * 8.0f / tDelta;
		rateLostHist[rateUpdateHistPtr] = bytesLost * 8.0f / tDelta;
		rateCeHist[rateUpdateHistPtr] = bytesCe * 8.0f / tDelta;
		rateRtpHist[rateUpdateHistPtr] = bytesRtp * 8.0f / tDelta;

		rateUpdateHistPtr = (rateUpdateHistPtr + 1) % kRateUpDateSize;
		rateTransmitted = 0.0f;
		rateAcked = 0.0f;
		rateLost = 0.0f;
		rateCe = 0.0f;
		rateRtp = 0.0f;
		for (int n = 0; n < kRateUpDateSize; n++) {
			rateTransmitted += rateTransmittedHist[n];
			rateAcked += rateAckedHist[n];
			rateLost += rateLostHist[n];
			rateCe += rateCeHist[n];
			rateRtp += rateRtpHist[n];
		}
		rateTransmitted /= kRateUpDateSize;
		rateAcked /= kRateUpDateSize;
		rateLost /= kRateUpDateSize;
		rateCe /= kRateUpDateSize;
		rateRtp /= kRateUpDateSize;
		if (rateRtp > 0 && isAdaptiveTargetRateScale && numberOfUpdateRate > kRateUpDateSize) {
			/*
			* Video coders are strange animals.. In certain cases the average bitrate is
			* consistently lower or higher than the target bitare. This additonal scaling compensates
			* for this anomaly.
			*/
			const float diff = targetBitrate / rateRtp;
			float alpha = 0.02f;
			targetRateScale *= (1.0f - alpha);
			targetRateScale += alpha * diff;
			targetRateScale = std::min(1.2f, std::max(0.5f, targetRateScale));
		}
		if (rateLost > 0) {
			lossEpoch = true;
		}
	}

	bytesTransmitted = 0;
	bytesAcked = 0;
	bytesRtp = 0;
	bytesLost = 0;
	bytesCe = 0;
	lastRateUpdateT_ntp = time_ntp;
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
ScreamTx::Stream* ScreamTx::getStream(uint32_t ssrc, int &streamId) {
	for (int n = 0; n < nStreams; n++) {
		if (streams[n]->isMatch(ssrc)) {
			streamId = n;
			return streams[n];
		}
	}
	streamId = -1;
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
	if (requestRefresh && !wasRepairLoss) {
		wasRepairLoss = true;
		return -1.0;
	}
	float rate = targetRateScale * targetBitrate;
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
* Update target bitrate, most of the time spent on developing SCReAM has been here.
* Video coders can behave very odd. The actual bitrate naturally varies around the target
*  bitrate by some +/-20% or even more, I frames can be real large all this is manageable
*  to at least some degree.
* What is more problematic is a systematic error where the video codec rate is consistently higher
*  or lower than the target rate.
* Various fixes in the code below (as well as the isAdaptiveTargetRateScale) seek to make
*  rate adaptive streaming with SCReAM work despite these anomalies.
*/
void ScreamTx::Stream::updateTargetBitrate(uint32_t time_ntp) {
	/*
	* Compute a maximum bitrate, this bitrates includes the RTP overhead
	*/

	float br = getMaxRate();
	float rateRtpLimit = std::max(rateRtp, br);
	if (initTime_ntp == 0) {
		/*
		* Initialize if the first time
		*/
		initTime_ntp = time_ntp;
		lastRtpQueueDiscardT_ntp = time_ntp;
	}

	if (lastBitrateAdjustT_ntp == 0) lastBitrateAdjustT_ntp = time_ntp;
	isActive = true;
	lastFrameT_ntp = time_ntp;
	if (lossEventFlag || ecnCeEventFlag) {
		/*
		* Loss event handling
		* Rate is reduced slightly to avoid that more frames than necessary
		* queue up in the sender queue
		*/
		if (time_ntp - lastTargetBitrateIUpdateT_ntp > 2000000) {
			/*
			* The timing constraint avoids that targetBitrateI
			*  is set too low in cases where a congestion event is prolonged.
			* An accurate targetBitrateI is not of extreme importance
			*  but helps to avoid jitter spikes when SCReAM operates
			*  over fixed bandwidth or slowly varying links.
			*/
			updateTargetBitrateI(br);
			lastTargetBitrateIUpdateT_ntp = time_ntp;
		}
		if (lossEventFlag)
			targetBitrate = std::max(minBitrate, targetBitrate*lossEventRateScale);
		else if (ecnCeEventFlag) {
			if (parent->isL4s) {
				/*
				 * scale backoff factor with RTT
				 */
				float backOff = parent->l4sAlpha / 2.0f;
				targetBitrate = std::max(minBitrate, targetBitrate*(1.0f - backOff));
			}
			else {
				targetBitrate = std::max(minBitrate, targetBitrate*ecnCeEventRateScale);
			}
		}
		float rtpQueueDelay = rtpQueue->getDelay(time_ntp * ntp2SecScaleFactor);
		if (rtpQueueDelay > maxRtpQueueDelay &&
			(time_ntp - lastRtpQueueDiscardT_ntp > kMinRtpQueueDiscardInterval_ntp)) {
			/*
			* RTP queue is cleared as it is becoming too large,
			* Function is however disabled initially as there is no reliable estimate of the
			* throughput in the initial phase.
			*/
            int seqNrOfNextRtp = rtpQueue->seqNrOfNextRtp();
            int seqNrOfLastRtp =  rtpQueue->seqNrOfLastRtp();
            int pak_diff = (seqNrOfLastRtp == -1) ? -1 : ((seqNrOfLastRtp >= hiSeqTx ) ? (seqNrOfLastRtp - hiSeqTx ) : seqNrOfLastRtp + 0xffff - hiSeqTx);

            int cur_cleared = rtpQueue->clear();
            cerr << log_tag << " rtpQueueDelay " << rtpQueueDelay << " too large 1 " << time_ntp / 65536.0f << " RTP queue " << cur_cleared  <<
                " packetes discarded for SSRC " << ssrc << " hiSeqTx " << hiSeqTx << " hiSeqAckendl " << hiSeqAck <<
                " seqNrOfNextRtp " << seqNrOfNextRtp <<  " seqNrOfLastRtp " << seqNrOfLastRtp << " diff " << pak_diff << endl;
            cleared += cur_cleared;
			rtpQueueDiscard = true;
			lossEpoch = true;

			lastRtpQueueDiscardT_ntp = time_ntp;
			targetRateScale = 1.0;
			txSizeBitsAvg = 0.0f;
		}

		lossEventFlag = false;
		ecnCeEventFlag = false;
		lastBitrateAdjustT_ntp = time_ntp;
	}
	else {
		if (time_ntp - lastBitrateAdjustT_ntp < kRateAdjustInterval_ntp)
			return;
		/*
		* A scale factor that is dependent on the inflection point
		* i.e the last known highest video bitrate
		* This reduces rate and delay overshoot
		* L4S allows for a faster path through the inflection point
		*/
		float sclI = (targetBitrate - targetBitrateI) / targetBitrateI;
		if (parent->isL4s)
			sclI *= 32;
		else
			sclI *= 4;
		sclI = sclI * sclI;
		sclI = std::max(0.25f, std::min(1.0f, sclI));
		float increment = 0.0f;

		/*
		* Size of RTP queue [bits]
		* As this function is called immediately after a
		* video frame is produced, we need to accept the new
		* RTP packets in the queue, we subtract a number of bytes correspoding to the size
		* of the last frame (including RTP overhead), this is simply the aggregated size
		* of the RTP packets with the highest RTP timestamp
		* txSizeBits is the number of bits in the RTP queue, this is limited
		* to just enable small adjustments of the bitrate when the RTP queue grows
		*/
		int lastBytes = rtpQueue->getSizeOfLastFrame();
		int txSizeBitsLimit = (int)(targetBitrate*0.02);
		int txSizeBits = std::max(0, rtpQueue->bytesInQueue() - lastBytes) * 8;
		txSizeBits = std::min(txSizeBits, txSizeBitsLimit);

		const float alpha = 0.5f;

		txSizeBitsAvg = txSizeBitsAvg * alpha + txSizeBits * (1.0f - alpha);
		/*
		* tmp is a local scaling factor that makes rate adaptation sligthly more
		* aggressive when competing flows (e.g file transfers) are detected
		*/
		float rampUpSpeedTmp = std::min(rampUpSpeed, targetBitrate*rampUpScale);
		if (parent->isCompetingFlows()) {
			rampUpSpeedTmp *= 2.0f;
		}

		float rtpQueueDelay = rtpQueue->getDelay(time_ntp * ntp2SecScaleFactor);
		if (rtpQueueDelay > maxRtpQueueDelay &&
			(time_ntp - lastRtpQueueDiscardT_ntp > kMinRtpQueueDiscardInterval_ntp)) {
			/*
			* RTP queue is cleared as it is becoming too large,
			* Function is however disabled initially as there is no reliable estimate of the
			* throughput in the initial phase.
			*/
            int seqNrOfNextRtp = rtpQueue->seqNrOfNextRtp();
            int seqNrOfLastRtp =  rtpQueue->seqNrOfLastRtp();
            int pak_diff = (seqNrOfLastRtp == -1) ? -1 : ((seqNrOfLastRtp >= hiSeqTx ) ? (seqNrOfLastRtp - hiSeqTx ) : seqNrOfLastRtp + 0xffff - hiSeqTx);

            int cur_cleared = rtpQueue->clear();
            cerr << log_tag << " rtpQueueDelay " << rtpQueueDelay << " too large 2 " << time_ntp / 65536.0f << " RTP queue " << cur_cleared  <<
                " packetes discarded for SSRC " << ssrc << " hiSeqTx " << hiSeqTx << " hiSeqAckendl " << hiSeqAck <<
                " seqNrOfNextRtp " << seqNrOfNextRtp <<  " seqNrOfLastRtp " << seqNrOfLastRtp << " diff " << pak_diff << endl;
            cleared += cur_cleared;
			rtpQueueDiscard = true;
			lossEpoch = true;
			lastRtpQueueDiscardT_ntp = time_ntp;
			targetRateScale = 1.0;
			txSizeBitsAvg = 0.0f;
		}
		else if (parent->inFastStart && rtpQueueDelay < 0.1f) {
			/*
			* Increment bitrate, limited by the rampUpSpeed
			*/
			increment = rampUpSpeedTmp * (kRateAdjustInterval_ntp * ntp2SecScaleFactor);
			/*
			* Limit increase rate near the last known highest bitrate or if priority is low
			*/
			increment *= sclI * sqrt(targetPriority);
			/*
			* Limited increase if the actual coder rate is lower than the target
			*/
			if (targetBitrate > rateRtpLimit) {
				/*
				 * Limit increase if the target bitrate is considerably higher than the actual
				 *  bitrate, this is an indication of an idle source.
				 * It can also be the case that the encoder consistently delivers a lower rate than
				 *  the target rate. We don't want to deadlock the bitrate rampup because of this so
				 *  we gradually reduce the increment the larger the difference is
				 */
				float scale = std::max(-1.0f, 2.0f*(rateRtpLimit / targetBitrate - 1.0f));
				increment *= (1.0 + scale);
			}
			/*
			* Add increment
			*/
			targetBitrate += increment;
			wasFastStart = true;
		}
		else {
			if (wasFastStart) {
				wasFastStart = false;
				if (time_ntp - lastTargetBitrateIUpdateT_ntp > 65536) { // 1s in NTP domain
					/*
					* The timing constraint avoids that targetBitrateI
					* is set too low in cases where a
					* congestion event is prolonged
					*/
					updateTargetBitrateI(br);
					lastTargetBitrateIUpdateT_ntp = time_ntp;
				}
			}

			/*
			* Update target rate
			*/

			float increment = br;
			if (!parent->isL4s || parent->l4sAlpha < 0.01) {
				/*
				* Apply the extra precaution with respect to queue delay and
				* RTP queue only if L4S is not running or when ECN marking does not occur for a longer period
				* scl is based on the queue delay trend
				*/
				float scl = queueDelayGuard * parent->getQueueDelayTrend();
				if (parent->isCompetingFlows())
					scl *= 0.05f;
				increment = increment * (1.0f - scl) - txQueueSizeFactor * txSizeBitsAvg;
			}
			increment -= targetBitrate;
			if (txSizeBits > 12000 && increment > 0)
				increment = 0;

			if (increment > 0) {
				wasFastStart = true;
				float incrementScale = 1.0f;
				if (parent->isL4s && parent->l4sAlpha > 0.01f) {
					/*
					 * In L4S mode we can boost the rate increase some extra
					 */
					incrementScale = 2.0f;
				}
				else {
					/*
					 * At very low bitrates it is necessary to actively try to push the
					 *  the bitrate up some extra
					 */
					incrementScale = 1.0f + 0.05f*std::min(1.0f, 50000.0f / targetBitrate);
				}
				increment *= incrementScale;
				if (!parent->isCompetingFlows()) {
					/*
					* Limit the bitrate increase so that it does not go faster than rampUpSpeedTmp
					* This limitation is not in effect if competing flows are detected
					*/
					increment *= sclI;
					increment = std::min(increment, (float)(rampUpSpeedTmp*(kRateAdjustInterval_ntp * ntp2SecScaleFactor)));
				}
				/*
				* Limited increase if the actual coder rate is lower than the target
				*/
				if (targetBitrate > rateRtpLimit) {
					/*
					 * Limit increase if the target bitrate is considerably higher than the actual
					 *  bitrate, this is an indication of an idle source.
					 * It can also be the case that the encoder consistently delivers a lower rate than
					 *  the target rate. We don't want to deadlock the bitrate rampup because of this so
					 *  we gradually reduce the increment the larger the difference is
					 */
					float scale = std::max(-1.0f, 2.0f*(rateRtpLimit / targetBitrate - 1.0f));
					increment *= (1.0 + scale);
				}
			}
			else {
				if (rateRtpLimit < targetBitrate) {
					/*
					 * Limit decrease if target bitrate is higher than actuall bitrate,
					 *  this a possible indication of an idle source, but it may also be the case
					 *  that the video coder consistently delivers a lower bitrate than the target
					 */
					float scale = std::max(-1.0f, 2.0f*(rateRtpLimit / targetBitrate - 1.0f));
					increment *= (1.0 + scale);
				}
				/*
				* Also avoid that the target bitrate is reduced if
				* the coder bitrate is higher
				* than the target.
				* The possible reason is that a large I frame is transmitted, another reason is
				* complex dynamic content.
				*/
				if (rateRtpLimit > targetBitrate*2.0f)
					increment = 0.0f;
			}
			targetBitrate += increment;

		}
		lastBitrateAdjustT_ntp = time_ntp;
	}

	targetBitrate = std::min(maxBitrate, std::max(minBitrate, targetBitrate));
}

bool ScreamTx::Stream::isRtpQueueDiscard() {
	bool tmp = rtpQueueDiscard;
	rtpQueueDiscard = false;
	return tmp;
}

bool ScreamTx::Stream::isLossEpoch() {
	bool tmp = lossEpoch;
	lossEpoch = false;
	return tmp;
}
