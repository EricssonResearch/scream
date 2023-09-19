#include "RtpQueue.h"
#include "ScreamTx.h"
#ifdef _MSC_VER
#define NOMINMAX
#include <winSock2.h>
#else
#include <arpa/inet.h>
#endif


// === Some good to have features, SCReAM works also
//     with these disabled
// Fast start can resume if little or no congestion detected
static const bool kEnableConsecutiveFastStart = true;

// Rate update interval
static const uint32_t kRateAdjustInterval_ntp = 1311; // 20ms in NTP domain

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
static float kL4sG = 0.125f;

// L4S alpha max value, for scalable congestion control
static const float kL4sAlphaMax = 1.0;

// Min CWND in MSS
static const int kMinCwndMss = 3;

// Compensate for max 33/65536 = 0.05% clock drift
static const uint32_t kMaxClockdriftCompensation = 33;

// Time stamp scale
static const int kTimeStampAtoScale = 1024;

// For computation of adaptivePacingRate
static const float kFrameSizeAvgAlpha = 1.0f / 32;

// For restricted behavior when mss/cwnd is very low
static const float kCwndRatioThLow = 0.05f;
static const float kCwndRatioThHigh = 0.20f;

// For SSRT histogram
static const float kSrttHistDecay = 1.0f / 128; // Roughly 3s decay time
static const float kSrttHistBinSize = 0.001;
static const float kSrttHighPercentile = 0.9f;
static const float kSrttLowPercentile = 0.1f;

static const float kSrttVirtual = 0.025f; // Virtual SRTT, similar to Prague CC

static const float kCeDensityAlpha = 1.0f / 16;

static const float kRelFrameSizeHistDecay = 1.0f / 1024;
static const float kRelFrameSizeHighPercentile = 0.75f;


ScreamV1Tx::ScreamV1Tx(float lossBeta_,
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
	bool enableClockDriftCompensation_,
	float maxAdaptivePacingRateScale_,
	bool isNewCc_
) :
	ScreamTx(),
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
	maxAdaptivePacingRateScale(maxAdaptivePacingRateScale_),
	isNewCc(isNewCc_),
	fastIncreaseFactor(1.0),
	sRtt(0.02f), // Init SRTT to 20ms
	sRtt_ntp(1311),
	sRttSh_ntp(1311),
	sRttLow(0.02f),
	sRttHigh(0.02f),
	ackedOwd(0),
	baseOwd(UINT32_MAX),
	queueDelay(0.0),
	queueDelayFractionAvg(0.0),
	queueDelayTrend(0.0),
	queueDelayTarget(queueDelayTargetMin),
	queueDelaySbdVar(0.0),
	queueDelaySbdMean(0.0),
	queueDelaySbdMeanSh(0.0),
	isEnablePacketPacing(true),

	bytesNewlyAcked(0),
	mss(kInitMss),
	cwnd(kInitMss * 2),
	cwndI(1),
	cwndMin(kInitMss * 2),
	cwndMinLow(0),
	lastBytesInFlightT_ntp(0),
	bytesInFlightMaxLo(0),
	bytesInFlightMaxHi(0),
	bytesInFlightHistLoMem(0),
	bytesInFlightHistHiMem(0),
	maxBytesInFlight(0.0f),
	isAutoTuneMinCwnd(false),

	lossEvent(false),
	wasLossEvent(false),
	lossEventRate(0.0),
	ecnCeEvent(false),
	isCeThisFeedback(false),
	l4sAlpha(0.1f),
	fractionMarked(0.0f),
	bytesMarkedThisRtt(0),
	bytesDeliveredThisRtt(0),
	lastL4sAlphaUpdateT_ntp(0),
	inFastStart(true),
	maxTotalBitrate(0.0f),
	postCongestionScale(1.0f),
	postCongestionDelay(0.1f),
	bytesInFlightRatio(0.0f),
	isL4sActive(true),
	ceDensity(1.0f),

	paceInterval_ntp(0),
	paceInterval(0.0f),
	rateTransmittedAvg(0.0f),
	adaptivePacingRateScale(1.0f),

	isInitialized(false),
	lastSRttUpdateT_ntp(0),
	lastBaseOwdAddT_ntp(0),
	lastCwndIUpdateT_ntp(0),
	baseOwdResetT_ntp(0),
	lastAddToQueueDelayFractionHistT_ntp(0),
	lastLossEventT_ntp(0),
	lastCeEventT_ntp(0),
	lastTransmitT_ntp(0),
	nextTransmitT_ntp(0),
	lastRateUpdateT_ntp(0),
	accBytesInFlightMax(0),
	nAccBytesInFlightMax(0),
	rateTransmitted(0.0f),
	rateAcked(0.0f),
	rateRtp(0.0f),
	rateTransmittedLog(0.0f),
	rateAckedLog(0.0f),
	rateRtpLog(0.0f),
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
	completeLogItem(false),

	cwndRatio(0.001f),
	cwndRatioScale(1.0f)

{
	if (isNewCc)
	  kL4sG = 1.0f/16; // Slower alpha in new algorithm
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
	isNewFrame = false;
	for (int n = 0; n < kMaxStreams; n++)
		streams[n] = NULL;

	queueDelayMax = 0.0f;
	queueDelayMinAvg = 0.0f;
	queueDelayMin = 1000.0;

	for (int n = 0; n < kSrttHistBins; n++) {
		sRttHist[n] = 0;
	}

}

ScreamV1Tx::~ScreamV1Tx() {
	for (int n = 0; n < nStreams; n++)
		delete streams[n];
}

/*
* Register new stream
*/
void ScreamV1Tx::registerNewStream(RtpQueueIface *rtpQueue,
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
	bool isAdaptiveTargetRateScale,
    float hysteresis) {

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
		isAdaptiveTargetRateScale,
	  hysteresis);
	streams[nStreams++] = stream;
}

void ScreamV1Tx::updateBitrateStream(uint32_t ssrc,
	float minBitrate,
	float maxBitrate) {
	int id;
	Stream *stream = getStream(ssrc, id);
	stream->minBitrate = minBitrate;
	stream->maxBitrate = maxBitrate;
}

RtpQueueIface * ScreamV1Tx::getStreamQueue(uint32_t ssrc) {
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
void ScreamV1Tx::newMediaFrame(uint32_t time_ntp, uint32_t ssrc, int bytesRtp, bool isMarker) {
	if (!isInitialized) initialize(time_ntp);

	if (isMarker) {
		isNewFrame = true;
	}

	int id;
	Stream *stream = getStream(ssrc, id);

	stream->newMediaFrame(time_ntp, bytesRtp, isMarker);
	stream->updateTargetBitrate(time_ntp);

	if (time_ntp - lastCwndUpdateT_ntp < 32768) { // 32768 = 0.5s in NTP domain
		/*
		* We expect feedback at least every 500ms
		* to update the target rate.
		*/
		stream->updateTargetBitrate(time_ntp);
	}
	if (!isL4sActive && (time_ntp - lastBaseDelayRefreshT_ntp < sRtt_ntp * 2 && time_ntp > sRtt_ntp * 2)) {
		/*
		* _Very_ long periods of congestion can cause the base delay to increase
		*  with the effect that the queue delay is estimated wrong, therefore we seek to
		*  refresh the whole thing by deliberately allowing the network queue to drain
		* Clear the RTP queue for 2 RTTs, this will allow the queue to drain so that we
		*  get a good estimate for the min queue delay.
		* This funtion is executed very seldom so it should not affect overall experience too much
		* This function is disabled when L4S is active as congestion queue build up is then
		*  very limited
		*/
		int cur_cleared = stream->rtpQueue->clear();
        if (cur_cleared) {
            cerr << logTag << " refresh " << time_ntp / 65536.0f << " RTP queue " << cur_cleared  << " packets discarded for SSRC " << ssrc << endl;
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
float ScreamV1Tx::isOkToTransmit(uint32_t time_ntp, uint32_t &ssrc) {
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
		rateRtp = 0.0f;
		rateTransmittedLog = 0.0f;
		rateAckedLog = 0.0f;
		rateRtpLog = 0.0f;
		for (int n = 0; n < nStreams; n++) {
			streams[n]->updateRate(time_ntp);
			rateTransmitted += streams[n]->rateTransmitted;
			rateTransmittedLog += streams[n]->rateTransmittedLog;
			rateRtp += streams[n]->rateRtp;
			rateRtpLog += streams[n]->rateRtpLog;
			rateTransmittedAvg = 0.9f*rateTransmittedAvg + 0.1f*rateTransmitted;
			rateAcked += streams[n]->rateAcked;
			rateAckedLog += streams[n]->rateAckedLog;
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
		if (maxTotalBitrate > 0 && !isNewCc) {
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
	bool condition = queueDelay < queueDelayTarget;
	if (isNewCc)
	   condition = inFastStart;

	if (condition) {
		exit = (bytesInFlight + sizeOfNextRtp) > cwnd*adaptivePacingRateScale + mss;
	} else {
		exit = (bytesInFlight + sizeOfNextRtp) > cwnd*adaptivePacingRateScale;
	}
	/*
	* Enforce packet pacing
	*/
	float retVal = 0.0f;
	uint32_t tmp_l = nextTransmitT_ntp - time_ntp;
	if (isEnablePacketPacing && (nextTransmitT_ntp > time_ntp) && (tmp_l < 0xFFFF0000)) {
		retVal = (nextTransmitT_ntp - time_ntp) * ntp2SecScaleFactor;
	}

	/*
	* A retransmission time out mechanism to avoid deadlock
	*/
	if (time_ntp - lastTransmitT_ntp > 32768 && lastTransmitT_ntp < time_ntp) {
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
float ScreamV1Tx::addTransmitted(uint32_t time_ntp,
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
	if (isEnablePacketPacing)
		nextTransmitT_ntp = time_ntp + paceInterval_ntp;
	else
		nextTransmitT_ntp = time_ntp;
	return paceInterval;
}

//extern uint32_t getTimeInNtp();
static uint32_t    unused;
static uint32_t time_ntp_prev = 0;
void ScreamV1Tx::incomingStandardizedFeedback(uint32_t time_ntp,
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
				   logTag, pak_diff,
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
void ScreamV1Tx::incomingStandardizedFeedback(uint32_t time_ntp,
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
	int prevBytesInFlight = bytesInFlight;
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
		if (isCeThisFeedback && time_ntp - lastLossEventT_ntp > std::min(1966u, sRtt_ntp)) { // CE event at least every 30ms
			ecnCeEvent = true;
			lastLossEventT_ntp = time_ntp;
			lastCeEventT_ntp = time_ntp;

		}

		if (!isEnablePacketPacing && !inFastStart && isNewCc) {
			/*
			 * The CE density is a metric of the fraction of updated RTCP
			 *  feedback that indicates CE marking when congestion occurs
			 * This scales down the CE mark fraction when packet pacing
			 *  is disabled
			 *
			 */
			if (isCeThisFeedback)
				ceDensity += kCeDensityAlpha;
			ceDensity *= 1.0f - kCeDensityAlpha;
			ceDensity = std::max(0.25f, ceDensity);
		}
		isCeThisFeedback = false;
		if (isL4s) {
			/*
			* L4S mode compute a congestion scaling factor that is dependent on the fraction
			*  of ECN marked packets
			*/
			if (time_ntp - lastL4sAlphaUpdateT_ntp > std::min(1966u,sRtt_ntp)) { // Update at least every 30ms
				lastL4sAlphaUpdateT_ntp = time_ntp;
				fractionMarked = 0.0f;
				if (bytesDeliveredThisRtt > 0) {
					fractionMarked = float(bytesMarkedThisRtt) / float(bytesDeliveredThisRtt);
					if (fractionMarked == 1.0) {
						/*
						 * Likely a fast reduction in throughput, reset ceDensity
						 *  so that CWND is reduced properly
						 */
						ceDensity = 1.0;
					}

					if (isNewCc) {
						fractionMarked *= cwndRatioScale * ceDensity;
					}

					/*
					 * L4S alpha (backoff factor) is averaged and limited
					 * It can make sense to limit the backoff because
					 *   1) source is rate limited
					 *   2) delay estimation algorithm also works in parallel
					 *   3) L4S marking algorithm can lag behind a little and potentially overmark
					 */
					l4sAlpha = std::min(kL4sAlphaMax, kL4sG * fractionMarked + (1.0f - kL4sG)*l4sAlpha);

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

		if (time_ntp - lastCwndUpdateT_ntp > 655u || lossEvent || ecnCeEvent || isNewFrame) {
			/*
			* There is no gain with a too frequent CWND update
			* An update every 10ms is fast enough even at very high high bitrates
			* Expections are loss or CE events
			*  or when a new frame arrives, in which case the packet pacing rate needs an update
			*/
			bytesInFlightRatio = std::min(1.0f,float(prevBytesInFlight) / cwnd);
			updateCwnd(time_ntp);
			/*
			* update target bitrate for s
			*/
			if (lossEvent || ecnCeEvent) {
				for (int n = 0; n < nStreams; n++) {
					Stream* tmp = streams[n];
					tmp->updateTargetBitrate(time_ntp);
				}
				ecnCeEvent = false;
			}
			lastCwndUpdateT_ntp = time_ntp;
			isNewFrame = false;
		}

	}
	isL4sActive = isL4s && (time_ntp - lastCeEventT_ntp < (10 * 65535)); // L4S enabled and at least one CE event the last 10 seconds

	if (isUseExtraDetailedLog || isLast || isMark) {
		if (fp_log && completeLogItem) {
			fprintf(fp_log, " %d,%d,%d,%1.0f,%d,%d,%d,%d,%1.0f,%1.0f,%1.0f,%1.0f,%1.0f,%d",
                    cwnd, bytesInFlight, inFastStart, rateTransmittedLog, streamId, seqNr, bytesNewlyAckedLog, ecnCeMarkedBytesLog, stream->rateRtpLog, stream->rateTransmittedLog, stream->rateAckedLog, stream->rateLostLog, stream->rateCeLog, isMark);
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
bool ScreamV1Tx::markAcked(uint32_t time_ntp,
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
		if ((tmp->seqNr == seqNr) && !tmp->isAcked) {
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
			if (isL4sActive || queueDelay < 0.001f) {
				/*
				* Either L4S marking or that a long standing queue is avoided
				*  no need to refresh the base delay for a foreseeable future
				*/
				lastBaseDelayRefreshT_ntp = time_ntp - 3*sRtt_ntp;
			}


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
void ScreamV1Tx::detectLoss(uint32_t time_ntp, struct Transmitted *txPackets, uint16_t highestSeqNr, Stream *stream) {
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
				// cerr << log_tag << " LOSS detected by reorder timer SSRC=" << stream->ssrc << " SN=" << tmp->seqNr << endl;
				stream->repairLoss = true;
			}
			else if (tmp->isAcked) {
				tmp->isUsed = false;
			}
		}
	}
}

float ScreamV1Tx::getTargetBitrate(uint32_t ssrc) {
	int id;
	return  getStream(ssrc, id)->getTargetBitrate();
}

void ScreamV1Tx::setTargetPriority(uint32_t ssrc, float priority) {
	int id;
	Stream *stream = getStream(ssrc, id);
	if (queueDelayFractionAvg > 0.1 || !inFastStart) {
		stream->targetBitrate *= priority / stream->targetPriority;
		stream->targetBitrate = std::min(std::max(stream->targetBitrate, stream->minBitrate), stream->maxBitrate);
	}
	stream->targetPriority = priority;
	stream->targetPriorityInv = 1.0f / priority;
}

void ScreamV1Tx::getLogHeader(char *s) {
	sprintf(s,
            "LogName,queueDelay,queueDelayMax,queueDelayMinAvg,sRtt,cwnd,bytesInFlightLog,rateTransmitted,isInFastStart,rtpQueueDelay,bytes,size,targetBitrate,rateRtp,packetsRtp,rateTransmittedStream,rateAcked,rateLost,rateCe, packetsCe,hiSeqTx,hiSeqAck,SeqDiff,packetetsRtpCleared,packetsLost");
}

void ScreamV1Tx::getLog(float time, char *s, uint32_t ssrc, bool clear) {
	int inFlightMax = std::max(bytesInFlight, bytesInFlightHistHiMem);
	sprintf(s, "%s Log, %4.3f, %4.3f, %4.3f, %4.3f, %6d, %6d, %6.0f, %1d, ",
		logTag, queueDelay, queueDelayMax, queueDelayMinAvg, sRtt,
		cwnd, bytesInFlightLog, rateTransmittedLog / 1000.0f, isInFastStart());
	bytesInFlightLog = bytesInFlight;
	queueDelayMax = 0.0;
	for (int n = 0; n < nStreams; n++) {
		Stream *tmp = streams[n];
        if (ssrc != 0 && tmp->ssrc != ssrc) {
            continue;
        }
		char s2[200];
        sprintf(s2, "%4.3f, %d,%d,%6.0f, %6.0f, %lu, %6.0f, %6.0f, %5.0f, %5.0f, %lu, %5d, %5d, %d, %lu,%lu",
			std::max(0.0f, tmp->rtpQueue->getDelay(time)),
            tmp->rtpQueue->bytesInQueue(),
            tmp->rtpQueue->sizeOfQueue(),
			tmp->targetBitrate / 1000.0f, tmp->rateRtp / 1000.0f,
            tmp->packetsRtp,
			tmp->rateTransmittedLog / 1000.0f, tmp->rateAckedLog / 1000.0f,
			tmp->rateLostLog / 1000.0f, tmp->rateCeLog / 1000.0f,
            tmp->packetsCe,
            tmp->hiSeqTx,
            tmp->hiSeqAck,
            tmp->hiSeqTx - tmp->hiSeqAck,
            tmp->cleared, tmp->packetLost);
		strcat(s, s2);
        if (clear) {
            tmp->packetsRtp = 0;
            tmp->packetsCe = 0;
            tmp->cleared = 0;
            tmp->packetLost = 0;
        }
	}

}

void ScreamV1Tx::getShortLog(float time, char *s) {
	int inFlightMax = std::max(bytesInFlight, bytesInFlightHistHiMem);
	sprintf(s, "%s %4.3f, %4.3f, %6d, %6d, %6.0f, %1d, ",
		logTag, queueDelay, sRtt,
		cwnd, bytesInFlightLog, rateTransmitted / 1000.0f, isInFastStart());
	bytesInFlightLog = bytesInFlight;
	queueDelayMax = 0.0;
	for (int n = 0; n < nStreams; n++) {
		Stream *tmp = streams[n];
		char s2[200];
		sprintf(s2, "%4.3f, %6.0f, %6.0f, %6.0f, %5.0f, %5.0f,",
			std::max(0.0f, tmp->rtpQueue->getDelay(time)),
			tmp->targetBitrate / 1000.0f, tmp->rateRtp / 1000.0f,
			tmp->rateTransmitted / 1000.0f,
			tmp->rateLost / 1000.0f, tmp->rateCe / 1000.0f);
		strcat(s, s2);
	}
}

void ScreamV1Tx::getVeryShortLog(float time, char *s) {
	int inFlightMax = std::max(bytesInFlight, bytesInFlightHistHiMem);
	sprintf(s, "%s %4.3f, %4.3f, %6d, %6d, %6.0f, ",
		logTag, queueDelay, sRtt,
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

float ScreamV1Tx::getQualityIndex(float time, float thresholdRate, float rttMin) {
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

bool ScreamV1Tx::isLossEpoch(uint32_t ssrc) {
	int id;
	return  getStream(ssrc, id)->isLossEpoch();
}

void ScreamV1Tx::initialize(uint32_t time_ntp) {
	isInitialized = true;
	lastSRttUpdateT_ntp = time_ntp;
	lastBaseOwdAddT_ntp = time_ntp;
	baseOwdResetT_ntp = time_ntp;
	lastAddToQueueDelayFractionHistT_ntp = time_ntp;
	lastLossEventT_ntp = time_ntp;
	lastCeEventT_ntp = time_ntp;
	lastTransmitT_ntp = time_ntp;
	nextTransmitT_ntp = time_ntp;
	lastRateUpdateT_ntp = time_ntp;
	lastAdjustPrioritiesT_ntp = time_ntp;
	lastRttT_ntp = time_ntp;
	lastBaseDelayRefreshT_ntp = time_ntp - 1;
	lastL4sAlphaUpdateT_ntp = time_ntp;
	initTime_ntp = time_ntp;
	lastCwndIUpdateT_ntp = time_ntp;
	lastCongestionDetectedT_ntp = 0;
}

float ScreamV1Tx::getTotalTargetBitrate() {
	float totalTargetBitrate = 0.0f;
	for (int n = 0; n < nStreams; n++) {
		totalTargetBitrate += streams[n]->targetBitrate;
	}
	return totalTargetBitrate;
}

/*
* Update the  congestion window
*/
void ScreamV1Tx::updateCwnd(uint32_t time_ntp) {
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
		bytesNewlyAckedLimited = std::min(bytesNewlyAckedLimited, 1.25f * maxRate * dT / 8.0f);
	else
		bytesNewlyAckedLimited = 2.0f * mss;

	queueDelayMin = std::min(queueDelayMin, queueDelay);

	queueDelayMax = std::max(queueDelayMax, queueDelay);

	float time = time_ntp * ntp2SecScaleFactor;
	if (queueDelayMinAvg > 0.25f * queueDelayTarget && time_ntp - baseOwdResetT_ntp > 1310720) { // 20s in NTP domain
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
	queueDelayFractionAvg = 0.9f * queueDelayFractionAvg + 0.1f * getQueueDelayFraction();

	/*
	* Compute paceInterval
	*/
	paceInterval = kMinPaceInterval;
	adaptivePacingRateScale = 1.0;
	if ((queueDelayFractionAvg > 0.02f || isL4s || maxTotalBitrate > 0) && isEnablePacketPacing) {
		float headroom = packetPacingHeadroom;
		if (lastCongestionDetectedT_ntp == 0) {
			/*
			* Increase packet pacing headroom until 1st congestion event, reduces problem
			*  with initial queueing in the RTP queue
			*/
			headroom = std::max(headroom, 1.5f);
		}

		/*
		* Compute adaptivePacingRateScale across all streams, the goal is to compute
		*  a best guess of the appropriate adaptivePacingRateScale given the individual streams
		*  adaptivePacingRateScale and their respective rates.
		* The scaling with the individual rates for the streams is based on the rationale that
		*  a low bitrate stream does not contribute much to the total packet pacing rate.
		*/
		float tmp1 = 0.0f;
		float tmp2 = 0.0f;
		for (int n = 0; n < nStreams; n++) {
			if (streams[n]->adaptivePacingRateScale > 1.0) {
				tmp1 += streams[n]->adaptivePacingRateScale * streams[n]->targetBitrate;
				tmp2 += streams[n]->targetBitrate;
			}
		}
		if (tmp2 > 0) {
			/*
			* Scale up the packet pacing headroom
			*/
			adaptivePacingRateScale = tmp1 / tmp2;
		}

		headroom *= adaptivePacingRateScale;

		float pacingBitrate = std::max(getTotalTargetBitrate(), rateRtp);
		pacingBitrate = std::max(1.0e5f, headroom * pacingBitrate);
		if (maxTotalBitrate > 0) {
			pacingBitrate = std::min(pacingBitrate, maxTotalBitrate);
		}
		float tp = (mss * 8.0f) / pacingBitrate;
		paceInterval = std::max(kMinPaceInterval, tp);
	}
	paceInterval_ntp = (uint32_t)(paceInterval * 65536); // paceinterval converted to NTP domain (Q16)


	/*
	* Less frequent updates here
	* + Save to queue delay fraction history
	*   used in computeQueueDelayTrend()
	* + Update queueDelayTarget
	*/
	if ((time_ntp - lastAddToQueueDelayFractionHistT_ntp) >
		kQueueDelayFractionHistInterval_us) {

		determineActiveStreams(time_ntp);

		if (queueDelayMin < queueDelayMinAvg)
			queueDelayMinAvg = queueDelayMin;
		else
			queueDelayMinAvg = 0.001f * queueDelayMin + 0.999f * queueDelayMinAvg;
		queueDelayMin = 1000.0f;
		/*
		* Need to duplicate insertion incase the feedback is sparse
		*/
		int nIter = (int)(time_ntp - lastAddToQueueDelayFractionHistT_ntp) / kQueueDelayFractionHistInterval_us;
		for (int n = 0; n < nIter; n++) {
			queueDelayFractionHist[queueDelayFractionHistPtr] = getQueueDelayFraction();
			queueDelayFractionHistPtr = (queueDelayFractionHistPtr + 1) % kQueueDelayFractionHistSize;
		}

		if (time_ntp - initTime_ntp > 131072) {
			/*
			* Queue delay trend calculations are reliable after ~2s
			*/
			computeQueueDelayTrend();
		}

		queueDelayTrendMem = std::max(queueDelayTrendMem * 0.98f, queueDelayTrend);

		/*
		* Compute bytes in flight limitation
		*/
		int maxBytesInFlightHi = (int)(std::max(bytesInFlightMaxHi, bytesInFlightHistHiMem));
		int maxBytesInFlightLo = (int)(std::max(bytesInFlight + bytesNewlyAcked, bytesInFlightHistLoMem));

		maxBytesInFlight =
			(maxBytesInFlightHi * (1.0f - queueDelayTrend) + maxBytesInFlightLo * queueDelayTrend) *
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
				queueDelayTarget = std::min(kQueueDelayTargetMax, oh * 1.5f);
			}
			else {
				if (queueDelaySbdVar < 0.2 && queueDelaySbdSkew < 0.05) {
					queueDelayTarget = std::max(queueDelayTargetMin, std::min(kQueueDelayTargetMax, oh));
				}
				else {
					if (oh < queueDelayTarget)
						queueDelayTarget = std::max(queueDelayTargetMin, std::max(queueDelayTarget * 0.99f, oh));
					else
						queueDelayTarget = std::max(queueDelayTargetMin, queueDelayTarget * 0.999f);
				}
			}
		}


		if (time_ntp - initTime_ntp > 2 * 65536) {
			/*
			 * Update the SRTT histogram and compute an upper percentile SRTT
			 *  that is in turn used to compute a more stable bitrate
			 *  when L4S is inactive (new congestion control algorithm)
			 * Updated is allowed when sRtt has stabilized
			 */
			for (int n = 0; n < kSrttHistBins; n++) {
				sRttHist[n] *= (1.0 - kSrttHistDecay);
			}
			int index = std::max(0, (std::min(kSrttHistBins - 1, int(sRtt / kSrttHistBinSize - 1 + 0.5f))));
			sRttHist[index]++;
			float sRttSum = 0.0;
			for (int n = 0; n < kSrttHistBins; n++) {
				sRttSum += sRttHist[n];
			}
			float sRttHighMark = sRttSum * kSrttHighPercentile;
			float sRttLowMark = sRttSum * kSrttLowPercentile;
			sRttSum = 0;
			int indexHigh = 0;
			int indexLow = 0;
			do {
				sRttSum += sRttHist[indexHigh];
				indexHigh++;
				if (sRttSum < sRttLowMark)
					indexLow++;
			} while (sRttSum < sRttHighMark && indexHigh < kSrttHistBins);

			sRttHigh = (indexHigh + 1) * kSrttHistBinSize;
			sRttLow = (indexLow + 1) * kSrttHistBinSize;

			/*
			* Update cwndMinLow if isAutoTuneMinCwnd == true
			*/
			if (isAutoTuneMinCwnd) {
				float minBitrateSum = 0.0f;
				for (int n = 0; n < nStreams; n++) {
					minBitrateSum += streams[n]->minBitrate;
				}
				cwndMinLow = minBitrateSum / 8.0f * sRttLow;
			}
		}
		/*
		 * Scale down the impact of L4S marking when MSS becomes
		 *  larger in relation to the congestion window
		 *
		 */
		cwndRatioScale = 1.0f - std::min(1.0f, std::max(0.0f,
			cwndRatio * kSrttVirtual / sRttLow - kCwndRatioThLow) / (kCwndRatioThHigh - kCwndRatioThLow));

		/*
		* l4sAlpha min can actually be calculated from the assumption that 2 packets are marked per RTT
		*  at steady state
		* This speeds up the rate reduction somewhat when throughput drops dramatically
		*  as l4sAlpha does not need to average up from nearly zero
		* The min value is limited to 0.2
		*/
		if (isNewCc) {
			float tmp = getTotalTargetBitrate() * std::max(sRtt, kSrttVirtual) / 8 / mss;
			float l4sAlphaMin = std::min(0.2f,2.0f / tmp);
			l4sAlpha = std::max(l4sAlpha, l4sAlphaMin);
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
		cwnd = std::max(cwndMin, (int)(lossBeta * cwnd));
		lossEvent = false;
		lastCongestionDetectedT_ntp = time_ntp;

		inFastStart = false;
		wasLossEvent = true;
		postCongestionScale = 0.0;
	}
	else if (ecnCeEvent) {
		/*
		* CE event detected
		*
		* Update inflexion point
		*/
		if (time_ntp - lastCwndIUpdateT_ntp > 16384) {
			lastCwndIUpdateT_ntp = time_ntp;
			cwndI = cwnd;
		}
		/*
		* CE event detected, decrease congestion window
		*/
		if (isL4s) {
			if (isNewCc) {
				cwnd = std::max(cwndMin, (int)((1.0f - l4sAlpha / 2.0) * cwnd) + mss);
				if (time_ntp - lastCongestionDetectedT_ntp > 5 * 65536) {
					/*
					* First congestion event for a long while. It is likely that the congestion window
					*  has inflated. The latter is to cope with video encoders that can lock to
					*  low bitrates.
					* Reduce the congestion window to a more sensible level (twice the bytes in flight history)
					*  to make congestion reaction faster.
					*/
					cwnd = std::min(cwnd, (int)(bytesInFlightHistHiMem * 2.0f));
				}
			}
			else {
				cwnd = std::max(cwndMin, (int)((1.0f - l4sAlpha / (2.0 * packetPacingHeadroom)) * cwnd) + mss);
			}
		}
		else {
			cwnd = std::max(cwndMin, (int)(ecnCeBeta * cwnd));
		}
		ecnCeEvent = false;
		lastCongestionDetectedT_ntp = time_ntp;

		inFastStart = false;
		wasLossEvent = true;
		postCongestionScale = 0.0;
	}
	else {
		postCongestionScale = std::max(0.0f, std::min(1.0f, (time_ntp - lastCongestionDetectedT_ntp) / (postCongestionDelay * 65536.0f)));
		cwndRatio = float(mss) / cwnd;

		if (time_ntp - lastRttT_ntp > sRtt_ntp) {
			if (wasLossEvent)
				lossEventRate = 0.99f * lossEventRate + 0.01f;
			else
				lossEventRate *= 0.99f;
			wasLossEvent = false;
			lastRttT_ntp = time_ntp;
		}


		float cwndRatioAdjusted = cwndRatio;
		if (isNewCc) {
			/*
			 * Limit how fast CWND can increase when the bitrate is low and
			 *  when L4S marking occurs.
			 */
			cwndRatioAdjusted = std::min(0.1f, cwndRatio) * (1.0f - fractionMarked);

			/*
			 * Also limit how fast CWND can increase when SRTT is low
			 * This should be identical to TCP Prague
			 */
			double tmp = std::min(1.0f, sRtt / kSrttVirtual);
			cwndRatioAdjusted *= tmp;
			if (!inFastStart)
				cwndRatioAdjusted *= 0.1f + cwndRatioScale * 0.9f;
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
				* Congestion window increase is limited for the 1st few seconds after congestion
				*/
				float bytesInFlightMargin = 1.5f;
				if (isL4s && isNewCc) {
					/*
					* Allow the congestion window to inflate. This makes it possible to handle
					*  cases when the video encoders lock to low bitrates
					*/
					bytesInFlightMargin = 3.0f;
				}
				if ((bytesInFlight + bytesNewlyAcked) * bytesInFlightMargin > cwnd) {
					if (isNewCc) {
						/*
						* sclI scales down the cwnd increase when cwnd is close to the last
						* known max that caused a congestion event
						*/
						float sclI = (cwnd - cwndI) / float(cwndI); sclI *= 2.0;
						sclI = std::max(1.0f / (fastIncreaseFactor + 1.0f), std::min(1.0f, sclI * sclI));
						/*
						* Calculate CWND increment
						*/
						float increment = sclI * postCongestionScale * fastIncreaseFactor *
							bytesNewlyAckedLimited * cwndRatioAdjusted;
						/*
						* Additionally scale increment with the RTT
						*/
						float sRttScale = std::max(1.0f, sRttLow / kSrttVirtual);
						sRttScale = sRttScale * sRttScale;
						increment *= sRttScale;

						cwnd = cwnd + (int)(increment + 0.5);
					}
					else {
						cwnd += int(postCongestionScale * bytesNewlyAckedLimited);
					}
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
				if ((bytesInFlight + bytesNewlyAcked) * bytesInFlightMargin > cwnd) {
					float increment = gainUp * offTarget * bytesNewlyAckedLimited * cwndRatioAdjusted;
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
				float delta = -(gainDown * offTarget * bytesNewlyAcked * cwndRatio);
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
						Stream* tmp = streams[n];
						tmp->targetBitrate = std::max(tmp->minBitrate,
							std::min(tmp->maxBitrate,
								tmp->targetBitrate * rateAdjustFactor));
					}
				}
				lastCongestionDetectedT_ntp = time_ntp;
			}
		}
	}
	/*
	* Congestion window validation, checks that the congestion window is
	* not considerably higher than the actual number of bytes in flight
	* Disable congestion window validation if L4S is active because the
	*  L4S marking will keep the congestion window bounded
	*/

	if (!isNewCc && maxBytesInFlight > 5000 && lastCongestionDetectedT_ntp > 0) {
		cwnd = std::min(cwnd, (int)maxBytesInFlight);
	}
	if (isNewCc && maxTotalBitrate > 0) {
		cwnd = std::min(cwnd, int(packetPacingHeadroom*kMaxBytesInFlightHeadRoom*maxTotalBitrate * sRtt / 8));
	}

	cwnd = std::max(cwndMin, cwnd);

	/*
	* Make possible to enter fast start if OWD has been low for a while
	*/
	if (queueDelayTrend > 0.2) {
		lastCongestionDetectedT_ntp = time_ntp;
	}
	else if (time_ntp - lastCongestionDetectedT_ntp > 16384 &&
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
void ScreamV1Tx::estimateOwd(uint32_t time_ntp) {
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
uint32_t ScreamV1Tx::getBaseOwd() {
	return  std::min(baseOwd, baseOwdHistMin);
}

/*
* Get the queue delay fraction
*/
float ScreamV1Tx::getQueueDelayFraction() {
	return queueDelay / queueDelayTarget;
}

/*
* Compute congestion indicator
*/
void ScreamV1Tx::computeQueueDelayTrend() {
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
void ScreamV1Tx::computeSbd() {
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
bool ScreamV1Tx::isCompetingFlows() {
	return queueDelayTarget > queueDelayTargetMin;
}

/*
* Get queue delay trend
*/
float ScreamV1Tx::getQueueDelayTrend() {
	return queueDelayTrend;
}

/*
* Determine active streams
*/
void ScreamV1Tx::determineActiveStreams(uint32_t time_ntp) {
	return;
	float surplusBitrate = 0.0f;
	float sumPrio = 0.0;
	bool streamSetInactive = false;
	for (int n = 0; n < nStreams; n++) {
		if (time_ntp - streams[n]->lastFrameT_ntp > 65536 && streams[n]->isActive) {
			streams[n]->isActive = false;
			surplusBitrate += streams[n]->targetBitrate;
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
void ScreamV1Tx::addCredit(uint32_t time_ntp, Stream* servedStream, int transmittedBytes) {
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
			}
		}
	}
}

/*
* Subtract credit from served stream
*/
void ScreamV1Tx::subtractCredit(uint32_t time_ntp, Stream* servedStream, int transmittedBytes) {
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
*  at regular intervals. This is a necessary addon to mitigate
*  issues that VBR media brings
* The function consists of equal measures or rational thinking and
*  black magic, which means that there is no 100% guarantee that
*  will always work.
* Furthermure the function will not ensure perfect
*  fairness.
*/
void ScreamV1Tx::adjustPriorities(uint32_t time_ntp) {
	if (isNewCc) {
		/*
		* Function does not have any meaning when the new CC algo is used
		*/
		return;
	}

	if (nStreams == 1 || time_ntp - lastAdjustPrioritiesT_ntp < 65536 ||
		queueDelayTrend < 0.2 && !isL4sActive) {
		/*
		* Skip if not only one stream or less than 1 second since last adjustment or
		*  if not congested
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
				if (streams[n]->rateRtp > fairRate*1.05f) {
					//float scale = std::max(0.8f, 1.0f - 0.1f * queueDelayTrend - l4sAlpha);//*(1.0f - postCongestionScale));
					streams[n]->targetBitrate = std::max(streams[n]->minBitrate, streams[n]->targetBitrate*0.9f);
				} else if (streams[n]->rateRtp < fairRate*0.9) {
					streams[n]->targetBitrate = std::min(streams[n]->maxBitrate, streams[n]->targetBitrate * 1.1f);
				}
			}
		}
		lastAdjustPrioritiesT_ntp = time_ntp;
	}
}

/*
* Get the prioritized stream
*/
ScreamV1Tx::Stream* ScreamV1Tx::getPrioritizedStream(uint32_t time_ntp) {
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

ScreamV1Tx::Stream::Stream(ScreamV1Tx *parent_,
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
	bool isAdaptiveTargetRateScale_,
  float hysteresis_) {
	parent = parent_;
	rtpQueue = rtpQueue_;
	ssrc = ssrc_;
	targetPriority = priority_;
	targetPriorityInv = 1.0f / targetPriority;
	minBitrate = minBitrate_;
	maxBitrate = maxBitrate_;
	targetBitrate = std::min(maxBitrate, std::max(minBitrate, startBitrate_));
	targetBitrateH = targetBitrate;
	rampUpSpeed = rampUpSpeed_;
	rampUpScale = rampUpScale_;
	maxRtpQueueDelay = maxRtpQueueDelay_;
	txQueueSizeFactor = txQueueSizeFactor_;
	queueDelayGuard = queueDelayGuard_;
	lossEventRateScale = lossEventRateScale_;
	ecnCeEventRateScale = ecnCeEventRateScale_;
	isAdaptiveTargetRateScale = isAdaptiveTargetRateScale_;
	hysteresis = hysteresis_;
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
	rateCe = 0.0f;
	rateTransmittedLog = 0.0f;
	rateAckedLog = 0.0f;
	rateLostLog = 0.0f;
	rateCeLog = 0.0f;
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
    packetsCe  = 0;
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
	lastFullWindowT_ntp = 0;
	bytesLost = 0;
	bytesCe = 0;
	wasRepairLoss = false;
	repairLoss = false;
	for (int n = 0; n < kMaxTxPackets; n++)
		txPackets[n].isUsed = false;
	txPacketsPtr = 0;
	lossEpoch = false;
	frameSize = 0;
	frameSizeAcc = 0;
	frameSizeAvg = 0.0f;
	adaptivePacingRateScale = 1.0;
	l4sOverShootScale = 0.0;
	framePeriod = 0.02;
	rtpQOverShoot = 0.0f;
	lastTargetBitrateHUpdateT_ntp = 0;

	relFrameSizeHist[0] = 1.0f;
	for (int n = 1; n < kRelFrameSizeHistBins; n++) {
		relFrameSizeHist[n] = 0.0f;
	}
	relFrameSizeHigh = 1.0f;

}

/*
* Update the estimated max media rate
*/
void ScreamV1Tx::Stream::updateRate(uint32_t time_ntp) {
	if (lastRateUpdateT_ntp != 0 && parent->enableRateUpdate) {
		numberOfUpdateRate++;
		float tDelta = (time_ntp - lastRateUpdateT_ntp) * ntp2SecScaleFactor;
		rateTransmittedHist[rateUpdateHistPtr] = bytesTransmitted * 8.0f / tDelta;
		rateAckedHist[rateUpdateHistPtr] = bytesAcked * 8.0f / tDelta;
		rateLostHist[rateUpdateHistPtr] = bytesLost * 8.0f / tDelta;
		rateCeHist[rateUpdateHistPtr] = bytesCe * 8.0f / tDelta;
		rateRtpHist[rateUpdateHistPtr] = bytesRtp * 8.0f / tDelta;

		rateTransmittedLog = rateTransmittedHist[rateUpdateHistPtr];
		rateAckedLog = rateAckedHist[rateUpdateHistPtr];
		rateLostLog = rateLostHist[rateUpdateHistPtr];
		rateCeLog = rateCeHist[rateUpdateHistPtr];
		rateRtpLog = rateRtpHist[rateUpdateHistPtr];

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
			const float diff = targetBitrate * targetRateScale / rateRtp;
			float alpha = 0.02f;
			targetRateScale *= (1.0f - alpha);
			targetRateScale += alpha * diff;
			targetRateScale = std::min(1.1f, std::max(0.8f, targetRateScale));
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
float ScreamV1Tx::Stream::getMaxRate() {
	return std::max(rateTransmitted, rateAcked);
}

/*
* Get the stream that matches SSRC
*/
ScreamV1Tx::Stream* ScreamV1Tx::getStream(uint32_t ssrc, int &streamId) {
	for (int n = 0; n < nStreams; n++) {
		if (streams[n]->isMatch(ssrc)) {
			streamId = n;
			return streams[n];
		}
	}
	streamId = -1;
	return NULL;
}

void ScreamV1Tx::Stream::newMediaFrame(uint32_t time_ntp, int bytesRtp, bool isMarker) {
	if (frameSizeAcc == 0) {
		/*
		 * New frame, compute RTP overshoot based on amount of data from old frame
		 * still in RTP queue
		 */
		rtpQOverShoot = std::max(0.0f,
			std::min(2.0f, (rtpQueue->bytesInQueue() - relFrameSizeHigh*frameSizeAvg) / frameSizeAvg));
	}

	frameSizeAcc += bytesRtp;
	/*
	 * Compute an adaptive pacing rate scale that allows the pacing rate to follow the frame sizes
	 *  so that packets are paced out faster when a large frame is generated by the encoder.
	 * This reduces the RTP queue but can potentially give a larger queue or more L4S marking in the network
	 * Pacing rate scaling is also increased if the RTP queue grows
	 */
	if (isMarker) {
		/*
		* Compute average frame period
		*/
		const float alpha = 0.1f;
		if (lastFrameT_ntp !=0) {
			float dT = (time_ntp - lastFrameT_ntp)*ntp2SecScaleFactor;
			framePeriod = dT * (1 - alpha) + framePeriod * alpha;
		}
		lastFrameT_ntp = time_ntp;

		if (frameSizeAvg == 0.0f)
			frameSizeAvg = frameSizeAcc;
		else
			frameSizeAvg = frameSizeAcc * kFrameSizeAvgAlpha + frameSizeAvg * (1.0 - kFrameSizeAvgAlpha);
		frameSize = std::max(rtpQueue->bytesInQueue(),frameSizeAcc);
		/*
		 * Calculate a histogram over how much the frame sizes exceeds the average. This helps to avoid that
		 *  the RTP queue builds up when the video encoder generates frames with very varying sizes.
		 */
		if (frameSizeAcc > frameSizeAvg) {
			int ix = std::max(0, std::min(kRelFrameSizeHistBins - 1, (int)((frameSizeAcc - frameSizeAvg) / frameSizeAvg * kRelFrameSizeHistBins)));
			relFrameSizeHist[ix]++;
			for (int n = 0; n < kRelFrameSizeHistBins; n++) {
				relFrameSizeHist[n] *= (1.0f - kRelFrameSizeHistDecay);
			}
			float sum = 0.0f;
			for (int n = 0; n < kRelFrameSizeHistBins; n++) {
				sum += relFrameSizeHist[n];
			}
			float relFrameSizeHighMark = sum * kRelFrameSizeHighPercentile;
			ix = 1;
			sum = relFrameSizeHist[0];
			while (sum < relFrameSizeHighMark && ix < kRelFrameSizeHistBins) {
				sum += relFrameSizeHist[ix];ix++;
			}
			ix--;
			relFrameSizeHigh = 1.0f+((float)ix) / kRelFrameSizeHistBins;
		}

		frameSizeAcc = 0;

		if (frameSizeAvg > 500.0f) {
			adaptivePacingRateScale = std::min(parent->maxAdaptivePacingRateScale, std::max(1.0f, frameSize / frameSizeAvg));
		}
		else {
			adaptivePacingRateScale = 1.0f;
		}
	}
}

/*
* Get the target bitrate.
* This function returns a value -1 if loss of RTP packets is detected,
*  either because of loss in network or RTP queue discard
*/
float ScreamV1Tx::Stream::getTargetBitrate() {

	bool requestRefresh = isRtpQueueDiscard() || repairLoss;
	repairLoss = false;
	if (requestRefresh && !wasRepairLoss) {
		wasRepairLoss = true;
		return -1.0;
	}
	float rate = targetRateScale * targetBitrateH;
	wasRepairLoss = false;
	return rate;
}

/*
* A small history of past max bitrates is maintained and the max value is picked.
* This solves a problem where consequtive rate decreases can give too low
*  targetBitrateI values.
*/
void ScreamV1Tx::Stream::updateTargetBitrateI(float br) {
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
void ScreamV1Tx::Stream::updateTargetBitrate(uint32_t time_ntp) {
	if (parent->isNewCc) {
		updateTargetBitrateNew(time_ntp);
	} else {
		updateTargetBitrateOld(time_ntp);
	}
}

void ScreamV1Tx::Stream::updateTargetBitrateNew(uint32_t time_ntp) {
	isActive = true;

	if (initTime_ntp == 0) {
		/*
		 * Initialize if the first time
		 */
		initTime_ntp = time_ntp;
		lastRtpQueueDiscardT_ntp = time_ntp;
		lastTargetBitrateHUpdateT_ntp = time_ntp;
	}
	if (lastBitrateAdjustT_ntp == 0) {
		lastBitrateAdjustT_ntp = time_ntp;
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
		int seqNrOfLastRtp = rtpQueue->seqNrOfLastRtp();
		int pak_diff = (seqNrOfLastRtp == -1) ? -1 : ((seqNrOfLastRtp >= hiSeqTx) ? (seqNrOfLastRtp - hiSeqTx) : seqNrOfLastRtp + 0xffff - hiSeqTx);

		int cur_cleared = rtpQueue->clear();
		cerr << parent->logTag << " rtpQueueDelay " << rtpQueueDelay << " too large 1 " << time_ntp / 65536.0f << " RTP queue " << cur_cleared <<
			" packets discarded for SSRC " << ssrc << " hiSeqTx " << hiSeqTx << " hiSeqAckendl " << hiSeqAck <<
			" seqNrOfNextRtp " << seqNrOfNextRtp << " seqNrOfLastRtp " << seqNrOfLastRtp << " diff " << pak_diff << endl;
		cleared += cur_cleared;
		rtpQueueDiscard = true;
		lossEpoch = true;

		lastRtpQueueDiscardT_ntp = time_ntp;
		targetRateScale = 1.0;
		rtpQueueDelay = rtpQueue->getDelay(time_ntp * ntp2SecScaleFactor);
	}


	/*
	* Rate is computed based CWND and RTT with some extra bells and whistles for the
	* case that L4S is either not enabled or that the network does not mark packets
	*/

	/*
	* In the multistream solution, the CWND is split between the streams according to the
	* priorities
	*  cwndShare = cwnd * priority[stream] /sum(priority[0]..priority[N-1])
	*/
	float cwndShare = parent->cwnd;
	float prioritySum = 0.0f;
	float priorityShare = 1.0;
	for (int n = 0; n < parent->nStreams; n++) {
		Stream* stream = parent->streams[n];
		if (stream->isActive)
			prioritySum += stream->targetPriority;
	}
	if (prioritySum > 0) {
		priorityShare = targetPriority / prioritySum;
		cwndShare *= priorityShare;
	}


	/*
	* Scale the target bitrate down in relation to maxBytesInFlightHeadroom and packet pacing headroom
	* In addition, scale down even more if the RTP queue increases
	* All this has to be balanced to avoid that SCReAM is starved out by competing
	* TCP Prague flows.
	*/
	float scl = 0.0;
	if (parent->isInFastStart()) {
		/*
		* Variable scale to avoid that rate jumps when switching to fast start
		*/
		float deltaT = (time_ntp - parent->lastCongestionDetectedT_ntp) * ntp2SecScaleFactor;
		scl = std::min(1.0f, deltaT / 5.0f);
	}

	float tmp = 1.0;
	if (parent->isInFastStart()) {
		/*
		* Variable scale down in fast start
		*/
		tmp /= 1.0 + (1.0f - scl) * (kMaxBytesInFlightHeadRoom - 1.0f);
	}
	else {
		tmp /= kMaxBytesInFlightHeadRoom;
	}

	/*
	 * Compensate for the case where the video coder produce wildly varying frame sizes
	 * This can make SCReAM starve when there are competing scalable flows over the same
	 *  bottleneck, but as the key goal is to ensure low latency there is not much of a choice.
	 */
	tmp /= relFrameSizeHigh;

	/*
	 * Scale down if RTP queue grows, this should happen seldom
	 */
	tmp /= (1.0 + 0.5*rtpQOverShoot);

	if (parent->isEnablePacketPacing) {
		/*
		 * Scale down proportional to the packet pacing headroom
		 */
		tmp /= parent->packetPacingHeadroom;
	}
	else {
		/*
		 * A reasonable down scaling to ensure that the RTP queue
		 *  does not build up esp. when virtual queue is used in network
		 */
		tmp /= 1.5;
	}

	/*
	* Use the high SRTT percentile computed from histogram to avoid oscillation
	* when L4S is not active
	*/
	float sRtt = parent->sRtt;
	if (!parent->isL4sActive) {
		sRtt = std::max(sRtt, (1.0f - scl) * parent->sRttHigh + scl * sRtt);
	}
	/*
	* Compute target bitrate.
	*/
	tmp *= 8 * cwndShare / std::max(0.01f, std::min(0.2f, sRtt));
	targetBitrate = tmp;

	lossEventFlag = false;
	ecnCeEventFlag = false;

	targetBitrate = std::min(maxBitrate, std::max(minBitrate, targetBitrate));

	/*
	* Update targetBitrateH
	*/
	float diff = (targetBitrate * targetRateScale - targetBitrateH) / targetBitrateH;
	if (diff > hysteresis || diff < -hysteresis / 4) {
		/*
		* Update target bitrate to video encoder only if bitrate varies
		*  enough, this prevents excessive rate signaling to the video encoder
		*  that can mess up the rate control loop in the encoder
		* In addition ensure at least 100ms between each rate increase
		*/
		if (time_ntp - lastTargetBitrateHUpdateT_ntp > 6554 || diff < 0) {
			targetBitrateH = targetBitrate * targetRateScale;
			lastTargetBitrateHUpdateT_ntp = time_ntp;
		}
	}
}


void ScreamV1Tx::Stream::updateTargetBitrateOld(uint32_t time_ntp) {
	/*
	* Compute a maximum bitrate, this bitrates includes the RTP overhead
	*/

	float br = getMaxRate();
	float rateRtpLimit = std::max(rateRtp, br);

	float cwndRatio = 0.0f;
	if (parent->isL4s)
		cwndRatio = parent->mss / float(parent->cwnd);
	if (initTime_ntp == 0) {
		/*
		* Initialize if the first time
		*/
		initTime_ntp = time_ntp;
		lastRtpQueueDiscardT_ntp = time_ntp;
	}

	if (lastBitrateAdjustT_ntp == 0) lastBitrateAdjustT_ntp = time_ntp;
	   isActive = true;
	if (lossEventFlag || ecnCeEventFlag) {
		/*
		* Loss event handling
		* Rate is reduced slightly to avoid that more frames than necessary
		* queue up in the sender queue
		*/
     	updateTargetBitrateI(br);
	    lastTargetBitrateIUpdateT_ntp = time_ntp;
		if (lossEventFlag)
			targetBitrate = std::max(minBitrate, targetBitrate*lossEventRateScale);
		else if (ecnCeEventFlag) {
			if (parent->isL4s) {
				/*
				 * Compensate for that the cwnd is always a little higher than 'averagely' necessary because of the
				 *  kMaxBytesInFlightHeadRoom and that frames are pushed out a bit faster than the target bitrate because of
			     *
				 * In addition scale the bitrate down less if the window is sparsely used, should be quite
				 *  rare but there is for instance a possibility that the L4S marking lags behind and
				 *  implements some filtering
				 */
				float backOff = parent->l4sAlpha / 2.0f;

				targetBitrate = std::min(maxBitrate, std::max(minBitrate, targetBitrate*(1.0f - backOff)+targetBitrate*cwndRatio));
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
            cerr << parent->logTag << " rtpQueueDelay " << rtpQueueDelay << " too large 1 " << time_ntp / 65536.0f << " RTP queue " << cur_cleared  <<
                " packets discarded for SSRC " << ssrc << " hiSeqTx " << hiSeqTx << " hiSeqAckendl " << hiSeqAck <<
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
		if (parent->bytesInFlightRatio > 0.9 && time_ntp - lastFullWindowT_ntp > 6554) {
			/*
			 * Window is still full after a congestion event, this is an indication that
			 *  the media bitrate is too high with the risk that the RTP queue begins to grow
			 * Nudge the target bitrate down slightly and allow for some grace time until next reduction
			 *  as media coders can sometimes be a bit slow in the reaction
			 */
			targetBitrate *= 0.95;
			lastFullWindowT_ntp = time_ntp;
		}

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
			sclI *= 4;
		else
			sclI *= 4;
		sclI = sclI * sclI;
		sclI = std::max(0.2f, std::min(1.0f, sclI));
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
            cerr << parent->logTag << " rtpQueueDelay " << rtpQueueDelay << " too large 2 " << time_ntp / 65536.0f << " RTP queue " << cur_cleared  <<
                " packets discarded for SSRC " << ssrc << " hiSeqTx " << hiSeqTx << " hiSeqAckendl " << hiSeqAck <<
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
				float scale = std::max(-1.0f, 1.0f*(rateRtpLimit / targetBitrate - 1.0f));
				increment *= (1.0 + scale);
			}
			/*
			 * Take it easy with the ramp-up after congestion
			 */
			increment *= parent->postCongestionScale;

			/*
			* Add increment
			*/
			targetBitrate += increment;
			wasFastStart = true;
		}
		else {
			if (wasFastStart) {
				wasFastStart = false;
				if (time_ntp - lastTargetBitrateIUpdateT_ntp > 32768) {
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
			/*
  			 * Apply the extra precaution with respect to queue delay
			 */
			float scl = queueDelayGuard * parent->getQueueDelayTrend();
			if (parent->isCompetingFlows())
				scl *= 0.05f;
			increment = increment * (1.0f - scl) - txQueueSizeFactor * txSizeBitsAvg;
			increment -= targetBitrate;
			if (txSizeBits > 12000 && increment > 0)
				increment = 0;

			if (increment > 0) {
				wasFastStart = true;
    			/*
				 * At very low bitrates it is necessary to actively try to push the
				 *  the bitrate up some extra
				 */
				float incrementScale = 1.0f + cwndRatio + 0.05f*std::min(1.0f, 50000.0f / targetBitrate);
				increment *= incrementScale;
				if (!parent->isCompetingFlows()) {
					/*
					* Limit the bitrate increase so that it does not go faster than rampUpSpeedTmp
					* This limitation is not in effect if competing flows are detected
					*/
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
					float scale = std::max(-1.0f, 2.0f * (rateRtpLimit / targetBitrate - 1.0f));
					increment *= (1.0 + scale);
				}
			}
			else {
				if (rateRtpLimit < targetBitrate) {
					/*
					 * Limit decrease if target bitrate is higher than actual bitrate,
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
	float diff = (targetBitrate-targetBitrateH)/targetBitrateH;
	if (diff > hysteresis || diff < -hysteresis/4) {
		/*
		* Update bitrate communicated to media encoder if the change is large enough
		*/
		targetBitrateH = targetBitrate;
	}
}

bool ScreamV1Tx::Stream::isRtpQueueDiscard() {
	bool tmp = rtpQueueDiscard;
	rtpQueueDiscard = false;
	return tmp;
}

bool ScreamV1Tx::Stream::isLossEpoch() {
	bool tmp = lossEpoch;
	lossEpoch = false;
	return tmp;
}
