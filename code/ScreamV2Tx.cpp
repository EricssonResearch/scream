#include "RtpQueue.h"
#include "ScreamTx.h"
#ifdef _WIN32
#define NOMINMAX
#include <winSock2.h>
#else
#include <arpa/inet.h>
#endif


// === Some good to have features, SCReAM works also
//     with these disabled
// Fast start can resume if little or no congestion detected

// Rate update interval
static const uint32_t kRateAdjustInterval_ntp = 1311; // 20ms in NTP domain

// ==== Less important tuning parameters ====
// Min pacing interval and min pacing rate
static const float kMinPaceInterval = 10e-6f;
// Initial MSS, this is set quite low in order to make it possible to
//  use SCReAM with audio only
static const int kInitMss = 100;

// Min and max queue delay target
static const float kQueueDelayTargetMax = 0.3f; //s

// Queue delay trend and shared bottleneck detection
static const uint32_t kSlowUpdateInterval_ntp = 3277; // 50ms in NTP domain
// video rate estimation update period
static const uint32_t kRateUpdateInterval_ntp = 3277; // 50ms in NTP domain
// Update period for MSS estimation
static const int kMssUpdateInterval_ntp = 65536;      // 1s in NTP
// Packet reordering margin (us)
static const uint32_t kReordertime_ntp = 1966;        // 30ms in NTP domain

// Update interval for base delay history
static const uint32_t kBaseDelayUpdateInterval_ntp = 655360; // 10s in NTP doain

// Reset interval for base delay history when not L4S active
static const uint32_t kBaseDelayResetInterval_ntp = 3932160; // 60s in NTP doain
static const int kNumRateLimitRtts = 5;

// L4S alpha gain factor, for scalable congestion control
static const float kL4sG = 1.0f / 32 ;

// L4S alpha max value, for scalable congestion control
static const float kL4sAlphaMax = 1.0;

// Min CWND in MSS
static const int kMinCwndMss = 3;

// Compensate for max 33/65536 = 0.05% clock drift
static const uint32_t kMaxClockdriftCompensation = 33;

// Time stamp scale
static const int kTimeStampAtoScale = 1024;

// Virtual SRTT, similar to Prague CC
static const float kSrttVirtual = 0.025f;

// Time constant for CE dentity averaging
static const float kCeDensityAlpha = 1.0f / 16;

// Max overshoot for bytes in flight
static const float kCwndOverhead = 1.5f;

// Min additive increase scaling
static const float kLowCwndScaleFactor = 1.0f;

// Time constant for queue delay average
static const float kQueueDelayAvgAlpha = 1.0f / 4;

// Packet overhead
static const int kPacketOverhead = 12 + 8; // RTP + UDP

// Excessive bytes in flight correction
static const float kBytesInFlightLimit = 0.9f;
static const float kMaxBytesInFlightLimitCompensation = 1.5f;

// Default recommended mss
static const int kRecommendedMss = 1200;

static const uint32_t kMssHoldTime = 10 * 65536;


ScreamV2Tx::ScreamV2Tx(float lossBeta_,
	float ecnCeBeta_,
	float queueDelayTargetMin_,
	int cwnd_,
	float packetPacingHeadroom_,
	float maxAdaptivePacingRateScale_,
	float bytesInFlightHeadRoom_,
	float multiplicativeIncreaseScalefactor_,
	bool isL4s_,
	bool openWindow_,
	bool enableSbd_,
	bool enableClockDriftCompensation_
) :
	ScreamTx(),
	lossBeta(lossBeta_),
	ecnCeBeta(ecnCeBeta_),
	queueDelayTargetMin(queueDelayTargetMin_),

	packetPacingHeadroom(packetPacingHeadroom_),
	maxAdaptivePacingRateScale(maxAdaptivePacingRateScale_),
	bytesInFlightHeadRoom(bytesInFlightHeadRoom_),
	multiplicativeIncreaseScalefactor(multiplicativeIncreaseScalefactor_),
	isL4s(isL4s_),
	openWindow(openWindow_),
	enableSbd(enableSbd_),
	enableClockDriftCompensation(enableClockDriftCompensation_),

	isEnablePacketPacing(true),
	isAutoTuneMinCwnd(false),
	enableRateUpdate(true),
	isUseExtraDetailedLog(false),

	sRtt(0.05f), // Init SRTT to 50ms
	sRtt_ntp(3277),
	sRttSh_ntp(3277),
	sRttShPrev_ntp(3277),
	currRtt(-1.0f),
	ackedOwd(0),
	baseOwd(UINT32_MAX),
	canResetBaseDelayHistory(false),

	queueDelay(0.0),
	queueDelayFractionAvg(0.0f),
	queueDelayTarget(queueDelayTargetMin),

	queueDelaySbdVar(0.0f),
	queueDelaySbdMean(0.0f),
	queueDelaySbdMeanSh(0.0f),
	queueDelaySbdSkew(0),

	queueDelayAvg(0.0f),
	queueDelayMax(0.0f),
	queueDelayMin(1000.0),
	queueDelayMinAvg(0.0f),

	bytesNewlyAcked(0),
	bytesNewlyAckedCe(0),
	bytesNewlyAckedLog(0),
	ecnCeMarkedBytesLog(0),

	mss(kInitMss),
	prevMss(kInitMss),
	nMssListItems(1),
	mssIndex(0),
	minPacketsInFlight(0),

	cwnd(kInitMss * 2),
	cwndMin(kInitMss * 2),
	cwndMinLow(0),
	cwndI(1),
	cwndRatio(0.001f),
	cwndIHistIx(0),

	bytesInFlight(0),
	bytesInFlightLog(0),
	prevBytesInFlight(0),
	maxBytesInFlight(0),
	maxBytesInFlightPrev(0),
	bytesInFlightRatio(0.0f),

	bytesMarkedThisRtt(0),
	bytesDeliveredThisRtt(0),
	packetsMarkedThisRtt(0),
	packetsDeliveredThisRtt(0),

	lossEvent(false),
	wasLossEvent(false),
	lossEventRate(0.0f),
	ecnCeEvent(false),
	virtualCeEvent(false),
	isCeThisFeedback(false),
	isL4sActive(false),
	fractionMarked(0.0f),
	lastFractionMarked(0.0f),
	l4sAlpha(0.1f),
	ceDensity(1.0f),
	virtualL4sAlpha(0.0f),
	postCongestionScale(1.0f),
	postCongestionDelay(0.1f),

	rateTransmitted(0.0f),
	rateRtpAvg(0.0f),
	maxRate(0.0f),
	maxTotalBitrate(0.0f),
	rateTransmittedAvg(0.0f),

	relFrameSizeHigh(1.0f),
	isNewFrame(false),

	paceInterval_ntp(0),
	paceInterval(0.0f),
	adaptivePacingRateScale(1.0f),

	baseOwdHistMin(UINT32_MAX),
	baseOwdHistPtr(0),
	queueDelayNormHistPtr(0),
	maxBytesInFlightHistIx(0),

	clockDriftCompensation(0),
	clockDriftCompensationInc(0),

	nStreams(0),

	fp_log(0),
	completeLogItem(false),

	isInitialized(false),
	initTime_ntp(0),
	lastCongestionDetectedT_ntp(0),
	lastRttT_ntp(0),
	lastMssUpdateT_ntp(0),
	lastSRttUpdateT_ntp(0),
	lastBaseOwdAddT_ntp(0),
	baseOwdResetT_ntp(0),
	lastSlowUpdateT_ntp(0),
	lastLossEventT_ntp(0),
	lastCeEventT_ntp(0),
	lastTransmitT_ntp(0),
	nextTransmitT_ntp(0),
	lastRateUpdateT_ntp(0),
	lastCwndIUpdateT_ntp(0),
	lastCwndUpdateT_ntp(0),
	lastQueueDelayAvgUpdateT_ntp(0),
	lastL4sAlphaUpdateT_ntp(0),
	lastBaseDelayRefreshT_ntp(0),
	lastRateLimitT_ntp(0)
{
	strcpy(detailedLogExtraData, "");
	strcpy(timeString, "");

	if (cwnd_ == 0) {
		cwnd = kInitMss * 2;
	}
	else {
		cwnd = cwnd_;
	}

	for (int n = 0; n < kBaseOwdHistSize; n++)
		baseOwdHist[n] = UINT32_MAX;
	for (int n = 0; n < kQueueDelayNormHistSize; n++)
		queueDelayNormHist[n] = 0.0f;
	for (int n = 0; n < kMaxStreams; n++)
		streams[n] = NULL;
	for (int n = 0; n < kMaxBytesInFlightHistSize; n++)
		maxBytesInFlightHist[n] = 0;
	for (int n = 0; n < kCwndIHistSize; n++) 
		cwndIHist[n] = 0;

	mssList[0] = kRecommendedMss;
}

ScreamV2Tx::~ScreamV2Tx() {
	for (int n = 0; n < nStreams; n++)
		delete streams[n];
}


/*
* Register new stream
*/
void ScreamV2Tx::registerNewStream(RtpQueueIface* rtpQueue,
	uint32_t ssrc,
	float priority,
	float minBitrate,
	float startBitrate,
	float maxBitrate,
	float maxRtpQueueDelay,
	bool isAdaptiveTargetRateScale,
	float hysteresis,
	bool enableFrameSizeOverhead) {
	Stream* stream = new Stream(this,
		rtpQueue,
		ssrc,
		priority,
		minBitrate,
		startBitrate,
		maxBitrate,
		maxRtpQueueDelay,
		isAdaptiveTargetRateScale,
		hysteresis,
		enableFrameSizeOverhead);
	streams[nStreams++] = stream;
}

void ScreamV2Tx::updateBitrateStream(uint32_t ssrc,
	float minBitrate,
	float maxBitrate) {
	int id;
	Stream* stream = getStream(ssrc, id);
	stream->minBitrate = minBitrate;
	stream->maxBitrate = maxBitrate;
}

RtpQueueIface* ScreamV2Tx::getStreamQueue(uint32_t ssrc) {
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
void ScreamV2Tx::newMediaFrame(uint32_t time_ntp, uint32_t ssrc, int bytesRtp, bool isMarker) {
	if (!isInitialized) initialize(time_ntp);

	if (isMarker) {
		isNewFrame = true;
	}

	int id;
	Stream* stream = getStream(ssrc, id);

	stream->newMediaFrame(time_ntp, bytesRtp, isMarker);
	stream->updateTargetBitrate(time_ntp);

	if (!isL4sActive && (time_ntp - lastBaseDelayRefreshT_ntp < sRtt_ntp * 2 && time_ntp > sRtt_ntp * 2)) {
		/*
		* _Very_ long periods of congestion can cause the base delay to increase
		* with the effect that the queue delay is estimated wrong, therefore we seek to
		* refresh the whole thing by deliberately allowing the network queue to drain
		* Clear the RTP queue for 2 RTTs, this will allow the queue to drain so that we
		* get a good estimate for the min queue delay.
		* This funtion is executed very seldom so it should not affect overall experience too much
		* This function is disabled when L4S is active as congestion queue build up is then
		* very limited
		*/
		int cur_cleared = stream->rtpQueue->clear();
		if (cur_cleared) {
			std::cerr << logTag << " refresh " << time_ntp / 65536.0f << " RTP queue " << cur_cleared << " packets discarded for SSRC " << ssrc << std::endl;
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
		cwndMin = std::max(cwndMinLow, 2 * getMss());
		cwnd = std::max(cwnd, cwndMin);
	}
}

/*
* Determine if OK to transmit RTP packet
*/
float ScreamV2Tx::isOkToTransmit(uint32_t time_ntp, uint32_t& ssrc) {
	if (!isInitialized) initialize(time_ntp);
	/*
	* Update rate estimated
	*/
	uint32_t tmp = kRateUpdateInterval_ntp;

	if (time_ntp - lastRateUpdateT_ntp > tmp) {
		rateRtpAvg = 0.0f;
		rateTransmitted = 0.0f;
		for (int n = 0; n < nStreams; n++) {
			streams[n]->updateRate(time_ntp);
			rateTransmitted += streams[n]->rateTransmitted;
			rateRtpAvg += streams[n]->rateRtpAvg;
			if (n == 0)
				statistics->add(streams[0]->rateTransmitted, streams[0]->rateLost, streams[0]->rateCe, sRtt, queueDelay);
		}
		rateTransmittedAvg = 0.8f * rateTransmittedAvg + 0.2f * rateTransmitted;
		lastRateUpdateT_ntp = time_ntp;

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
	Stream* stream = getPrioritizedStream(time_ntp);

	if (stream == NULL)
		/*
		* No RTP packets to transmit
		*/
		return -1.0f;
	ssrc = stream->ssrc;

	/*
	* Update bytes in flight history for congestion window validation
	*/
	if (time_ntp - lastMssUpdateT_ntp > kMssUpdateInterval_ntp) {
		/*
		* In addition, reset MSS, this is useful in case for instance
		* a video stream is put on hold, leaving only audio packets to be
		* transmitted
		*/
		prevMss = mss;
		mss = kInitMss;
		cwndMin = std::max(cwndMinLow, kMinCwndMss * getMss());
		cwnd = std::max(cwnd, cwndMin);

		/*
		* Add a small clock drift compensation
		* for the case that the receiver clock is faster
		*/
		clockDriftCompensation += clockDriftCompensationInc;

		lastMssUpdateT_ntp = time_ntp;
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
	if (!openWindow) {
		exit = (bytesInFlight + sizeOfNextRtp) > cwnd * kCwndOverhead * relFrameSizeHigh + getMss();
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
float ScreamV2Tx::addTransmitted(uint32_t time_ntp,
	uint32_t ssrc,
	int size,
	uint16_t seqNr,
	bool isMark,
	float rtpQueueDelay,
	uint32_t timeStamp) {
	if (!isInitialized)
		initialize(time_ntp);

	int id;
	Stream* stream = getStream(ssrc, id);

	int ix = seqNr % kMaxTxPackets;
	Transmitted* txPacket = &(stream->txPackets[ix]);
	stream->hiSeqTx = seqNr;
	txPacket->timeTx_ntp = time_ntp;
	txPacket->rtpQueueDelay = rtpQueueDelay;
	txPacket->size = size;
	txPacket->seqNr = seqNr;
	txPacket->timeStamp = timeStamp;
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
	cwndMin = std::max(cwndMinLow, 2 * getMss());
	cwnd = std::max(cwnd, cwndMin);

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
void ScreamV2Tx::incomingStandardizedFeedback(uint32_t time_ntp,
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
		* they are quite rate but will mess up the entire
		* feedback handling if they are not taken care of.
		*/
		int streamId = -1;
		Stream* stream = getStream(ssrc, streamId);
		if (stream == 0) {
			/*
			* Bogus RTCP?, the SSRC is wrong anyway, Skip
			*/
			return;
		}

		uint16_t diff = end_seq - stream->hiSeqAck;
		bool isAckOoo = false;
		if (diff > 65000 && stream->hiSeqAck != 0 && stream->timeTxAck_ntp != 0) {
			/*
			* Out of order received ACKs are ignored but we allow some slack for OOO packets
			*/
			uint16_t diff = stream->hiSeqAck - end_seq;
			isAckOoo = true;
			if (diff > 512) {
				return;
			}
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
			int pak_diff = (Last == -1) ? -1 : ((Last >= stream->hiSeqTx) ? (Last - stream->hiSeqTx) : Last + 0xffff - stream->hiSeqTx);
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
void ScreamV2Tx::incomingStandardizedFeedback(uint32_t time_ntp,
	int streamId,
	uint32_t timestamp,
	uint16_t seqNr,
	uint8_t ceBits,
	bool isLast) {

	Stream* stream = streams[streamId];
	Transmitted* txPackets = stream->txPackets;
	completeLogItem = false;
	prevBytesInFlight = bytesInFlight;
	maxBytesInFlight = std::max(bytesInFlight, maxBytesInFlight);


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
		if (time_ntp - lastLossEventT_ntp > std::min(1966u, sRtt_ntp)) { // CE event at least every 30ms
			if (isCeThisFeedback) {
				ecnCeEvent = true;
				lastLossEventT_ntp = time_ntp;
				lastCeEventT_ntp = time_ntp;
			}
		}

		if (!isEnablePacketPacing) {
			/*
			* The CE density is a metric of the fraction of updated RTCP
			* feedback that indicates CE marking when congestion occurs
			* This scales down the CE mark fraction when packet pacing
			* is disabled
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
			* of ECN marked packets
			*/
			if (time_ntp - lastL4sAlphaUpdateT_ntp > std::min(655u, sRtt_ntp)) { // Update at least every 10ms
				lastL4sAlphaUpdateT_ntp = time_ntp;
				fractionMarked = 0.0f;
				if (bytesDeliveredThisRtt > 0) {
					fractionMarked = float(packetsMarkedThisRtt) / float(packetsDeliveredThisRtt);
					
					if (fractionMarked == 1.0f) {
						/*
						* Likely a fast reduction in throughput, reset ceDensity
						* so that CWND is reduced properly
						*/
						ceDensity = 1.0;
					}

					/*
					* Scale down fractionMarked if packet pacing is disabled
					*/
					fractionMarked *= ceDensity;

					/*
					* L4S alpha (backoff factor) is averaged and limited
					* It can make sense to limit the backoff because
					*   1) source is rate limited
					*   2) delay estimation algorithm also works in parallel
					*   3) L4S marking algorithm can lag behind a little and potentially overmark
					*/
					l4sAlpha = std::min(kL4sAlphaMax, kL4sG * fractionMarked + (1.0f - kL4sG) * l4sAlpha);

					bytesDeliveredThisRtt = 0;
					bytesMarkedThisRtt = 0;
					packetsDeliveredThisRtt = 0;
					packetsMarkedThisRtt = 0;
					lastFractionMarked = fractionMarked;
				}
			}
		}

		if (time_ntp - lastQueueDelayAvgUpdateT_ntp > std::min(1966u, sRtt_ntp)) {
			if (queueDelay < queueDelayAvg) {
				queueDelayAvg = queueDelay;
			}
			else {
				queueDelayAvg = (1.0f - kQueueDelayAvgAlpha) * queueDelayAvg + kQueueDelayAvgAlpha * queueDelay;
			}
			lastQueueDelayAvgUpdateT_ntp = time_ntp;
		}

		/*
		* Compute an average expected l4sAlpha if the channel is congested at this bitrate
		* And allow fake CE marking
		*/
		float l4sAlphaLim = 2 / getTotalTargetBitrate() * getMss() * 8 / sRtt;
		if ((l4sAlpha < l4sAlphaLim * 0.1 || !isL4sActive)) {
			/*
			* This code fakes ECN-CE events when either ECN or L4S is not enabled or in case packets are not
			* marked in the network
			* The l4sAlphaLim condition is to avoid to use this when we are reasonable sure
			* that packets are L4S marked. The reason is that the queue delay can sometimes suffer from issues with
			* clock drift
			* Use the average queue delay to avoid over reaction to lower later retransmissions
			*/

			if ((queueDelayAvg > queueDelayTarget / 4.0f) && time_ntp - lastLossEventT_ntp > std::min(1966u, sRtt_ntp)) {
				virtualCeEvent = true;
				/*
				* A virtual L4S alpha is calculated based on the estimated queue delay
				*/
				virtualL4sAlpha = std::min(1.0f, l4sAlphaLim + std::max(0.0f, (queueDelayAvg - queueDelayTarget / 4.0f) / (3* queueDelayTarget / 4)));
			}
		}

		if (sRttShPrev_ntp > sRttSh_ntp && fractionMarked == 1.0f) {
			/*
			* L4S marking may have too high marking thresholds, the result is that queues
			* can become large that CE marking stays too long. This inhibits CE marking if the RTT reduces, which 
			* is a reasonably safe sign that queues begin to deplete
			*/
			ecnCeEvent = false;
		}

		if (lossEvent || ecnCeEvent || virtualCeEvent) {
			lastLossEventT_ntp = time_ntp;
		}

		if (lastCwndUpdateT_ntp == 0)
			lastCwndUpdateT_ntp = time_ntp;

		if (time_ntp - lastCwndUpdateT_ntp > std::min(1966u, sRtt_ntp) ||
			lossEvent || ecnCeEvent || virtualCeEvent || isNewFrame) {
			/*
			* There is no gain with a too frequent CWND update
			* An update every 10ms is fast enough even at very high high bitrates
			* Expections are loss or CE events
			* or when a new frame arrives, in which case the packet pacing rate needs an update
			*/
			bytesInFlightRatio = std::min(1.0f, float(prevBytesInFlight) / cwnd);
			updateCwnd(time_ntp);
			for (int n = 0; n < nStreams; n++) {
				Stream* tmp = streams[n];
				tmp->updateTargetBitrate(time_ntp);
			}
			ecnCeEvent = false;
			virtualCeEvent = false;
			lastCwndUpdateT_ntp = time_ntp;
			isNewFrame = false;
		}

	}
	isL4sActive = isL4s && (time_ntp - lastCeEventT_ntp < (5 * 65535)) && lastCeEventT_ntp != 0; // L4S enabled and at least one CE event the last 10 seconds

	float time = time_ntp * ntp2SecScaleFactor;

	if (isUseExtraDetailedLog || isLast || isMark) {
		if (fp_log && completeLogItem) {			
			fprintf(fp_log, " %d,%d,%d,%1.0f,%d,%d,%d,%d,%1.0f,%1.0f,%1.0f,%1.0f,%1.0f,%d,%1.0f,%3.3f %d",
				cwnd, bytesInFlight, 0, rateTransmittedAvg, streamId, seqNr, bytesNewlyAckedLog, ecnCeMarkedBytesLog,
				stream->rateRtpAvg, stream->rateTransmittedAvg, stream->rateAcked, stream->rateLost, stream->rateCe,
				isMark, stream->targetBitrate, stream->rtpQueueDelay, cwndI); //rtpQueue->getDelay(time));
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
bool ScreamV2Tx::markAcked(uint32_t time_ntp,
	struct Transmitted* txPackets,
	uint16_t seqNr,
	uint32_t timestamp,
	Stream* stream,
	uint8_t ceBits,
	int& encCeMarkedBytesLog,
	bool isLast,
	bool& isMark) {

	bool isCe = false;
	int ix = seqNr % kMaxTxPackets;
	if (txPackets[ix].isUsed) {
		/*
		* RTP packet is in flight
		*/
		Transmitted* tmp = &txPackets[ix];

		/*
		* Receiption of packet given by seqNr
		*/
		if ((tmp->seqNr == seqNr) && !tmp->isAcked) {
			bytesDeliveredThisRtt += tmp->size;
			packetsDeliveredThisRtt += 1;
			isMark = tmp->isMark;
			statistics->addEcn(ceBits);
			if (ceBits == 0x03) {
				/*
				* Packet was CE marked, increase counter
				*/
				bytesNewlyAckedCe += tmp->size;
				encCeMarkedBytesLog += tmp->size;
				bytesMarkedThisRtt += tmp->size;
				packetsMarkedThisRtt += 1;
				stream->bytesCe += tmp->size;
				stream->packetsCe++;
				isCe = true;
			}
			/*
			* Wrap-around safe update of timeStampAckHigh 
			*/
			uint32_t diff = tmp->timeStamp - stream->timeStampAckHigh;
			if (diff < 50000) {
				stream->timeStampAckHigh = tmp->timeStamp;
			}

			stream->rtpQueueDelay = tmp->rtpQueueDelay;
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
				* reduce the clock drift compensation and restore qDel to 0
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
				* no need to refresh the base delay for a foreseeable future
				*/
				lastBaseDelayRefreshT_ntp = time_ntp - 3 * sRtt_ntp;
			}


			uint32_t rtt = time_ntp - tmp->timeTx_ntp;
			currRtt = rtt*ntp2SecScaleFactor;

			if (fp_log && (isUseExtraDetailedLog || isLast || isMark)) {
				fprintf(fp_log, "%s,%1.4f,%1.4f,", timeString, queueDelay, rtt * ntp2SecScaleFactor);
				completeLogItem = true;
			}
			
			if (rtt < 1000000 && isLast) {
				sRttShPrev_ntp = sRttSh_ntp;
				sRttSh_ntp = (7 * sRttSh_ntp + rtt) / 8;
				if (time_ntp - lastSRttUpdateT_ntp > sRttSh_ntp) {
					sRtt_ntp = (7 * sRtt_ntp + sRttSh_ntp) / 8;
					lastSRttUpdateT_ntp = time_ntp;
					sRtt = sRtt_ntp * ntp2SecScaleFactor;
				}
			}
			stream->timeTxAck_ntp = tmp->timeTx_ntp;
		}
	}
	else {
		unused++;
	}
	return isCe;
}


/*
* Detect lost RTP packets
*/
void ScreamV2Tx::detectLoss(uint32_t time_ntp, struct Transmitted* txPackets, uint16_t highestSeqNr, Stream* stream) {
	/*
	* Loop only through the packets that are covered by the last highest ACK - 512, this saves complexity
	* There is a faint possibility that we miss to detect large bursts of lost packets with this fix
	*/
	int ix1 = highestSeqNr; 
	ix1 = ix1 % kMaxTxPackets;
	int ix0 = stream->hiSeqAck;
	ix0 = ix0 % kMaxTxPackets;
	ix0 -= 512;
	int diff = std::min(uint16_t(32),uint16_t(highestSeqNr - stream->hiSeqAck));

	if (ix0 < 0) ix0 += kMaxTxPackets;
	int ixL0 = ix0-diff;
	if (ixL0 < 0) {
		ixL0 += kMaxTxPackets;
		ix0 += kMaxTxPackets;
	}
	while (ix1 < ix0) {
		ix1 += kMaxTxPackets;
	}
	stream->hiSeqAck = highestSeqNr;

	/*
	* Mark unacked packets outside window as lost forever
	*/
	for (int m = ixL0; m < ix0; m++) {
		int n = m % kMaxTxPackets;
		if (txPackets[n].isUsed) {
			Transmitted* tmp = &txPackets[n];
			if (!tmp->isAcked) {
				if (time_ntp - lastLossEventT_ntp > sRtt_ntp && lossBeta < 1.0f) {
					lossEvent = true;
				}
				stream->bytesLost += tmp->size;
				stream->packetLost++;
				tmp->isUsed = false;
				stream->repairLoss = true;
			}
			tmp->isUsed = false;
		}
	}

	/*
	* Mark late packets as lost
	*/
	for (int m = ix0; m <= ix1; m++) {
		int n = m % kMaxTxPackets;
		/*
		* Loop through TX packets
		*/
		if (txPackets[n].isUsed) {
			Transmitted* tmp = &txPackets[n];
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

			/*
			* Determine if RTP packet is ACKed beyond the allowed reording delay, or just lost
			* It is necessary to compensate for that the transmission events jump because of periodic video frames
			* the tsDiffCorrection makes a reference to transmission of earlier frames with same timestamp
			* to determine if a packet is lost.
			*/
			uint32_t tsDiffCorrection = (uint32_t)(65536.0f * (stream->timeStampAckHigh - tmp->timeStamp) / stream->timeStampClockRate);
			if (tmp->timeTx_ntp + kReordertime_ntp + tsDiffCorrection < stream->timeTxAck_ntp && !tmp->isAcked) {
				/*
				* Packet ACK is delayed more than kReordertime_ntp after an ACK of a higher SN packet, 
				* compensated for timestamp jumps for new frames.
				* Raise a loss event and remove from TX list
				*/
				if (time_ntp - lastLossEventT_ntp > sRtt_ntp && lossBeta < 1.0f) {
					lossEvent = true;
				}
				stream->bytesLost += tmp->size;
				stream->packetLost++;
				tmp->isUsed = false;
				//printf("%2.3f Late LOSS \n", time_ntp / 65536.0);
				stream->repairLoss = true;
			}
			else if (tmp->isAcked) {
				tmp->isUsed = false;
			}
		}
	}
}

float ScreamV2Tx::getTargetBitrate(uint32_t time_ntp, uint32_t ssrc) {
	int id;
	float rate = getStream(ssrc, id)->getTargetBitrate();
	/*
	* Check if queue delay is constantly high either because of clock drift
	* or a standing queue. If that is the case, base delay history is reset.
	*/
	if (!isL4sActive || isL4sActive && queueDelayMinAvg > queueDelayTarget / 8) {
		/*
		* The base delay may slowly creep up when SCReAM operates only on 
		* detection of estimated one way delay. The reason can be clock drift 
		* or standing queue. A short period with reduced target bitrate and 
		* simultaneous reset on the base delay history fixes this problem
		*/
		if (time_ntp - lastRateLimitT_ntp > kBaseDelayResetInterval_ntp) {
			lastRateLimitT_ntp = time_ntp;
		}
		uint32_t tmp = time_ntp - lastRateLimitT_ntp;
		uint32_t tmp2 = std::max(6554u,kNumRateLimitRtts*sRtt_ntp); // At least 100ms reduced rate
		if (tmp < tmp2) {
			rate *= 0.5f;
			canResetBaseDelayHistory = true;
		} else {
			if (canResetBaseDelayHistory) {;
				/*
				* Reset base delay history
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
				canResetBaseDelayHistory = false;
			}	
		}
	}

	return  rate;
}

void ScreamV2Tx::setTargetPriority(uint32_t ssrc, float priority) {
	int id;
	Stream* stream = getStream(ssrc, id);
	stream->targetPriority = priority;
	stream->targetPriorityInv = 1.0f / priority;
}

void ScreamV2Tx::getLogHeader(char* s) {
	sprintf(s,
		"LogName,queueDelay,queueDelayMax,queueDelayMinAvg,sRtt,cwnd,bytesInFlightLog,rateTransmitted,isInFastStart,rtpQueueDelay,bytes,size,targetBitrate,rateRtp,packetsRtp,rateTransmittedStream,rateAcked,rateLost,rateCe,packetsCe,hiSeqTx,hiSeqAck,SeqDiff,packetetsRtpCleared,packetsLost");
}

void ScreamV2Tx::getLog(float time, char* s, uint32_t ssrc, bool clear) {
	int inFlightMax = bytesInFlight;
	sprintf(s, "%s Log, %4.3f, %4.3f, %4.3f, %4.3f, %6d, %6d, %6.0f, %1d, ",
		logTag, queueDelay, queueDelayMax, queueDelayMinAvg, sRtt,
		cwnd, bytesInFlightLog, rateTransmittedAvg / 1000.0f, 0);
	bytesInFlightLog = bytesInFlight;
	queueDelayMax = 0.0;
	for (int n = 0; n < nStreams; n++) {
		Stream* tmp = streams[n];
		if (ssrc != 0 && tmp->ssrc != ssrc) {
			continue;
		}
		char s2[200];
		sprintf(s2, "%4.3f, %d,%d,%6.0f, %6.0f, %lu, %6.0f, %6.0f, %5.0f, %5.0f, %lu, %5d, %5d, %d, %lu,%lu",
			//std::max(0.0f, tmp->rtpQueue->getDelay(time)),
			std::max(0.0f, tmp->rtpQueueDelay),
			tmp->rtpQueue->bytesInQueue(),
			tmp->rtpQueue->sizeOfQueue(),
			tmp->targetBitrate / 1000.0f, tmp->rateRtpAvg / 1000.0f,
			tmp->packetsRtp,
			tmp->rateTransmittedAvg / 1000.0f, tmp->rateAcked / 1000.0f,
			tmp->rateLost / 1000.0f, tmp->rateCe / 1000.0f,
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

void ScreamV2Tx::getShortLog(float time, char* s) {
	int inFlightMax = bytesInFlight;
	sprintf(s, "%s %4.3f, %4.3f, %6d, %6d, %6.0f, %1d, ",
		logTag, queueDelay, sRtt,
		cwnd, bytesInFlightLog, rateTransmittedAvg / 1000.0f, 0);
	bytesInFlightLog = bytesInFlight;
	queueDelayMax = 0.0;
	for (int n = 0; n < nStreams; n++) {
		Stream* tmp = streams[n];
		char s2[200];
		sprintf(s2, "%4.3f, %6.0f, %6.0f, %6.0f, %5.0f, %5.0f,",
			std::max(0.0f, tmp->rtpQueueDelay),// tmp->rtpQueue->getDelay(time)),
			tmp->targetBitrate / 1000.0f, tmp->rateRtpAvg / 1000.0f,
			tmp->rateTransmittedAvg / 1000.0f,
			tmp->rateLost / 1000.0f, tmp->rateCe / 1000.0f);
		strcat(s, s2);
	}
}

void ScreamV2Tx::getVeryShortLog(float time, char* s) {
	int inFlightMax = bytesInFlight;
	sprintf(s, "%s %4.3f, %4.3f, %6d, %6d, %6.0f, ",
		logTag, queueDelay, sRtt,
		cwnd, bytesInFlightLog, rateTransmittedAvg / 1000.0f);
	bytesInFlightLog = bytesInFlight;
	queueDelayMax = 0.0;
	for (int n = 0; n < 1; n++) {
		Stream* tmp = streams[n];
		char s2[200];
		sprintf(s2, "%5.0f, %5.0f, ",
			tmp->rateLost / 1000.0f, tmp->rateCe / 1000.0f);
		strcat(s, s2);
	}
}

float ScreamV2Tx::getQualityIndex(float time, float thresholdRate, float rttMin) {
	/*
	* The quality index is an approximate value of the streaming quality and takes the bitrate
	* the RTT and the RTP queue delay into account.
	* The quality index is a value between 0 and 100% and gives an impression of the
	* experienced quality e.g in remote control applications.
	* Note that this is not a MOS score!.
	*/
	float qualityIndex = std::max(0.0f, std::min(1.0f, (rateTransmittedAvg - thresholdRate * 0.1f) / (thresholdRate * 0.9f)));
	qualityIndex *= std::max(0.0f, (0.1f - streams[0]->rtpQueue->getDelay(time)) / 0.1f);
	qualityIndex *= std::max(0.0f, std::min(1.0f, (4 * rttMin - (sRtt - rttMin)) / (4 * rttMin)));
	return qualityIndex * 100.0f;
}

bool ScreamV2Tx::isLossEpoch(uint32_t ssrc) {
	int id;
	return  getStream(ssrc, id)->isLossEpoch();
}

void ScreamV2Tx::initialize(uint32_t time_ntp) {
	isInitialized = true;
	lastSRttUpdateT_ntp = time_ntp;
	lastBaseOwdAddT_ntp = time_ntp;
	baseOwdResetT_ntp = time_ntp;
	lastSlowUpdateT_ntp = time_ntp;
	lastLossEventT_ntp = time_ntp;
	lastCeEventT_ntp = 0;
	lastTransmitT_ntp = time_ntp;
	nextTransmitT_ntp = time_ntp;
	lastRateUpdateT_ntp = time_ntp;
	lastRttT_ntp = time_ntp;
	lastBaseDelayRefreshT_ntp = time_ntp - 1;
	lastL4sAlphaUpdateT_ntp = time_ntp;
	lastMssUpdateT_ntp = time_ntp;
	initTime_ntp = time_ntp;
	lastCongestionDetectedT_ntp = 0;
	lastQueueDelayAvgUpdateT_ntp = time_ntp;
	lastRateLimitT_ntp = time_ntp;
	lastMssChange_ntp = time_ntp;
}

float ScreamV2Tx::getTotalTargetBitrate() {
	float totalTargetBitrate = 0.0f;
	for (int n = 0; n < nStreams; n++) {
		totalTargetBitrate += streams[n]->targetBitrate;
	}
	return totalTargetBitrate;
}

float ScreamV2Tx::getTotalMaxBitrate() {
	float totalMaxBitrate = 0.0f;
	for (int n = 0; n < nStreams; n++) {
		totalMaxBitrate += streams[n]->maxBitrate;
	}
	return totalMaxBitrate;
}


/*
* Determine CWND inflexion point based on median filtering
*/
void ScreamV2Tx::updateCwndI(int cwndI_) {

	if (cwndIHist[0] == 0) {
		for (int n = 0; n < kCwndIHistSize; n++)
			cwndIHist[n] = cwndI_;
	}
	cwndIHist[cwndIHistIx] = cwndI_;
	cwndIHistIx = (cwndIHistIx + 1) % kCwndIHistSize;

	int tmp[kCwndIHistSize];
	for (int n = 0; n < kCwndIHistSize; n++)
		tmp[n] = cwndIHist[n];

	for (int n = 0; n < kCwndIHistSize - 1; n++) {
		for (int m = n + 1; m < kCwndIHistSize; m++) {
			if (tmp[n] > tmp[m]) {
				int tmp2 = tmp[n];
				tmp[n] = tmp[m];
				tmp[m] = tmp2;
			}
		}
	}

	cwndI = tmp[kCwndIHistSize / 2];
}
/*
* Update the  congestion window
*/

void ScreamV2Tx::updateCwnd(uint32_t time_ntp) {
	if (lastCwndUpdateT_ntp == 0)
		lastCwndUpdateT_ntp = time_ntp;

	float lossDistanceScale = std::max(0.0f,std::min(1.0f,
                             ((time_ntp - lastLossEventT_ntp)/65536.0f/sRtt-4.0f)/4.0f));
	postCongestionScale = std::max(0.0f, 
	     std::min(1.0f, 
             ((time_ntp - lastCongestionDetectedT_ntp) / 65536.0f-0.0f*postCongestionDelay)/postCongestionDelay));

	queueDelayMin = std::min(queueDelayMin, queueDelay);

	queueDelayMax = std::max(queueDelayMax, queueDelay);

	float time = time_ntp * ntp2SecScaleFactor;

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
	if (isEnablePacketPacing) {
		/*
		* Compute adaptivePacingRateScale across all streams, the goal is to compute
		* a best guess of the appropriate adaptivePacingRateScale given the individual streams
		* adaptivePacingRateScale and their respective rates.
		* The scaling with the individual rates for the streams is based on the rationale that
		* a low bitrate stream does not contribute much to the total packet pacing rate.
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

		float headroom = packetPacingHeadroom * adaptivePacingRateScale;
		if (lastCongestionDetectedT_ntp == 0) {
			/*
			* Increase packet pacing headroom until 1st congestion event, reduces problem
			* with initial queueing in the RTP queue
			*/
			headroom = std::max(headroom, 1.5f);
		}

		float pacingBitrate = std::max(getTotalTargetBitrate(), rateRtpAvg);
	
		pacingBitrate = std::max(50e3f, headroom * pacingBitrate);
		if (maxTotalBitrate > 0) {
			pacingBitrate = std::min(pacingBitrate, maxTotalBitrate * packetPacingHeadroom);
		}
		float tp = (getMss() * 8.0f) / pacingBitrate;
		paceInterval = std::max(kMinPaceInterval, tp);
	}
	paceInterval_ntp = (uint32_t)(paceInterval * 65536); // paceinterval converted to NTP domain (Q16)


	/*
	* Less frequent updates here
	* + Save to queue delay fraction history
	*   used in computeQueueDelayTrend()
	* + Update queueDelayTarget
	*/
	if ((time_ntp - lastSlowUpdateT_ntp) >
		kSlowUpdateInterval_ntp) {

		determineActiveStreams(time_ntp);
#ifdef ENABLE_BASE_OWD_RESET_2		
		if ((queueDelayMinAvg > 0.5f * queueDelayTarget || queueDelayMinAvg > 0.9f*sRtt) && time_ntp - baseOwdResetT_ntp > 655360) {
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
#endif
		/*
		* Calculate queueDelayMinAvg as a ~10s average.
		* A slow attack, fast decay filter is used
		*/
		if (queueDelayMin < queueDelayMinAvg)
			queueDelayMinAvg = queueDelayMin;
		else
			queueDelayMinAvg = 0.005f * queueDelayMin + 0.995f * queueDelayMinAvg;
		queueDelayMin = 1000.0f;

		float queueDelayNorm = queueDelay / queueDelayTargetMin;

		if (enableSbd) {
			/*
			* Shared bottleneck detection,
			*/
			queueDelayNormHist[queueDelayNormHistPtr] = queueDelayNorm;
			queueDelayNormHistPtr = (queueDelayNormHistPtr + 1) % kQueueDelayNormHistSize;

			/*
			* Compute shared bottleneck detection and update queue delay target
			* if queue delta variance is sufficienctly low
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

		maxBytesInFlightHist[maxBytesInFlightHistIx] = maxBytesInFlight;
		maxBytesInFlightHistIx++;
		if (maxBytesInFlightHistIx == kMaxBytesInFlightHistSize)
			maxBytesInFlightHistIx = 0;

		if (getTotalTargetBitrate() > getTotalMaxBitrate() * 0.95f) {
			/*
			* CWND is allowed to grow considerably larger than bytes in flight
			*  at ramp-up, this to avoid that the ramp-up locks to low rates
			* However, when bitrate is almost max, it is better to constrain CWND
			*  to avoid large queue build up when congestion occurs
			*/
			int tmp = maxBytesInFlight;
			for (int n = 0; n < kMaxBytesInFlightHistSize; n++) {
				tmp = std::max(tmp, maxBytesInFlightHist[n]);
			}

			/*
			* Additionally compute updated CWND from totalMaxBitrate and rtt 
			* with extra headroom, to avoid that the target bitrates 
			* varies unnecessarily near the max rate
			*/
			tmp = std::max(tmp, (int)(packetPacingHeadroom * relFrameSizeHigh * getTotalMaxBitrate() / 8 * 
				                (sRtt + 0.001f)));

			/*
			* Limit CWND
			*/
			cwnd = std::min(cwnd, tmp);
		}

		if (time_ntp - initTime_ntp > 2 * 65536) {
			/*
			* Update cwndMinLow if isAutoTuneMinCwnd == true
			*/
			if (isAutoTuneMinCwnd) {
				float minBitrateSum = 0.0f;
				for (int n = 0; n < nStreams; n++) {
					minBitrateSum += streams[n]->minBitrate;
				}
				cwndMinLow = (int)(minBitrateSum / 8.0f * sRtt);
			}

			/*
			* Update max bytes in flight
			*/
			maxBytesInFlightPrev = maxBytesInFlight;
			maxBytesInFlight = 0;
		}

		/*
		* Updated relFrameSizeHigh based on individual streams' histogram
		*/
		relFrameSizeHigh = 0.0f;
		float sumPrio = 0.0f;
		for (int n = 0; n < nStreams; n++) {
			relFrameSizeHigh += streams[n]->getRelFrameSizeHigh()*streams[n]->targetPriority;
			sumPrio += streams[n]->targetPriority;
		}
		relFrameSizeHigh /= sumPrio;

		lastSlowUpdateT_ntp = time_ntp;
	}

	float cwndScaleFactorBase = kLowCwndScaleFactor;
	float cwndScaleFactorExt = (multiplicativeIncreaseScalefactor * cwnd)/ getMss();

	/*
	* Compute scale factor based on relation between CWND and last known 
	* max CWND
	*/
	float sclI = 1.0;
	float tmp = (cwnd - cwndI) / float(cwndI);
	tmp *= 4;
	tmp = tmp * tmp;
	sclI = std::max(0.1f, std::min(1.0f, tmp));

	/*
	* At very low congestion windows (just a few MSS) the up and down scaling
	* of CWND is scaled down. This is needed as there are just a few packets in flight
	* and the congestion marking this gets more unsteady.
	* The drawback is that reaction to congestion becomes slower.
	* Reducing the MSS when bitrate is low is the best option if fast reaction to congestion
	* is desired
	*/
	if (lossEvent) {
		/*
		* Update inflexion point
		*/
		if (time_ntp - lastCwndIUpdateT_ntp > 32768) {
			lastCwndIUpdateT_ntp = time_ntp;
			updateCwndI(cwnd);
		}
		/*
		* loss event detected, decrease congestion window
		*/
		cwnd = std::max(cwndMin, (int)(lossBeta * cwnd));
		lossEvent = false;
		lastCongestionDetectedT_ntp = time_ntp;

		wasLossEvent = true;
		postCongestionScale = 0.0f;
	}
	else if (ecnCeEvent || virtualCeEvent) {
		/*
		* Update inflexion point
		*/
		if (time_ntp - lastCwndIUpdateT_ntp > sRtt_ntp) {
			lastCwndIUpdateT_ntp = time_ntp;
			updateCwndI(cwnd);
		}
		/*
		* CE event detected, decrease congestion window
		*/
		if (isL4s && !virtualCeEvent) {
			float backOff = l4sAlpha / 2.0f;

			/*
			* Limit reduction when CWND becomes small, this is complemented
    		* with a corresponding reduction in CWND growth
		    */
			backOff *= std::max(0.5f, 1.0f - cwndRatio);

			/*
			 * Scale down backoff if close to the last known max
			 * This is complemented with a scale down of the CWND increase
			 * Don't scale down back off if queueDelay is large
			 */
			if (queueDelay < queueDelayTarget * 0.25f) {
				backOff *= std::max(0.25f, sclI);
			}

			if (time_ntp - lastCongestionDetectedT_ntp > 65536 * 5.0f) {
				/*
				* A long time since last congested because link throughput
				* exceeds max video bitrate.
				* There is a certain risk that CWND has increased way above
				* bytes in flight, so we reduce it here to get it better on track
				* and thus the congestion episode is shortened
				*/
				cwnd = std::min(cwnd, maxBytesInFlightPrev);
				/*
				* Also, we back off a little extra if needed
				* because alpha is quite likely very low
				* This can in some cases be an over-reaction though
				* but as this function should kick in relatively seldom
				* it should not be to too big concern
				*/
				backOff = std::max(backOff, 0.25f);

				/*
				* In addition, bump up l4sAlpha to a more credible value
				* This may over react but it is better than
				* excessive queue delay
				*/
				l4sAlpha = std::max(l4sAlpha,0.25f);

			}
			/*
			* Scale down CWND based on l4sAlpha
			*/
			cwnd = std::max(cwndMin, (int)((1.0f - backOff) * cwnd));
		}
		else if (virtualCeEvent) {
			/*
			* Scale down CWND based on virtualL4sAlpha
			*/
			float backOff = virtualL4sAlpha / 2.0f;
			cwnd = std::max(cwndMin, (int)((1.0f - backOff) * cwnd));
		}
		else {
			/*
			* Scale down CWND based on fixed Beta
			*/
			cwnd = std::max(cwndMin, (int)(ecnCeBeta * cwnd));
		}
		ecnCeEvent = false;
		lastCongestionDetectedT_ntp = time_ntp;

		wasLossEvent = true;
		postCongestionScale = 0.0;
	}
	cwndRatio = float(getMss()) / cwnd;

	if (time_ntp - lastRttT_ntp > sRtt_ntp) {
		if (wasLossEvent)
			lossEventRate = 0.98f * lossEventRate + 0.02f;
		else
			lossEventRate *= 0.98f;
		wasLossEvent = false;
		lastRttT_ntp = time_ntp;
	}

	/*
	* Compute max increment based on bytes acked
	*  but subtract number of CE marked bytes
	*/
	int bytesAckedMinusCe = bytesNewlyAcked - bytesNewlyAckedCe;
	float increment = (kGainUp * bytesAckedMinusCe) * cwndRatio;

	/*
	 * Scale the increment more cautious when close the last
	 * know max CWND. This essentially scales the CWND increase so that it can be as
	 * small as 0.1MSS per RTT.
	 */
	if (isL4sActive) {
		/*
		* This downscaling is complemented with a similar downscaling of
		* the CWND back off.
		*/
		increment *= std::max(0.25f, sclI);
	} else {
		increment *= sclI;
	}

	/*
	* Reduce increment for very small RTTs
	*/
	tmp = std::min(1.0f, sRtt / kSrttVirtual);
	increment *= tmp;

	/*
	 * Limit on CWND growth speed further for small CWND
	 * This is complemented with a corresponding restriction on CWND
	 * reduction
	 */
	increment *= std::max(0.5f, 1.0f - cwndRatio);

	/*
	* Calculate relative growth of CWND
	*/
	float tmp2 = cwndScaleFactorBase+cwndScaleFactorExt;
	/*
	* Limit multiplicative increase when congestion occured
	* recently
	*/
	if (tmp2 > cwndScaleFactorBase && postCongestionDelay > 0.2f) {
		tmp2 = cwndScaleFactorBase + ((tmp2 - cwndScaleFactorBase) * postCongestionScale) * sclI;
	}
	increment *= tmp2;

	/*
	* Increase CWND only if bytes in flight is large enough
	* Quite a lot of slack is allowed here to avoid that bitrate locks to
	* low values.
	*/
	double maxAllowed = getMss() + std::max(maxBytesInFlight, maxBytesInFlightPrev) * bytesInFlightHeadRoom;
	int cwndTmp = cwnd + (int)(increment + 0.5f);
	if (cwndTmp <= maxAllowed)
		cwnd = cwndTmp;

	cwnd = std::max(cwndMin, cwnd);

	if (maxTotalBitrate > 0) {
		cwnd = std::min(cwnd, int(maxTotalBitrate * sRtt / 8));
	}

	bytesNewlyAcked = 0;
	bytesNewlyAckedCe = 0;

	/*
	* Compute rate share among streams, take into account that some streams reach the
	* max bitrate, and therefore more is shared among the other streams that
	* have not reached the max bitrate.
	* 1ms extra is addded to sRTT to address a problem that system clocks can have limited 
	* accuracy that in turn can lead to a too high media bitrate. It is thus better to slightly over 
	* estimate sRtt than under estimate it.
	*/
	float rateLeft = 8 * cwnd / std::max(0.001f, std::min(0.2f, sRtt + 0.001f));

	/*
	* Make the rate estimation more cautious when the window is almost full or overfilled
	* This is only enabled when L4S is neither enabled, nor active
	*/
	if (!isL4sActive && bytesInFlightRatio > kBytesInFlightLimit) {
		rateLeft /= std::min(kMaxBytesInFlightLimitCompensation, bytesInFlightRatio / kBytesInFlightLimit);
	}

	/*
	* Scale down rate slighty when the congestion window is very small compared to mss
	* This helps to avoid unnecessary RTP queue build up
	* Note that at very low bitrates it is necessary to reduce the MTU also
	*/
	rateLeft *= 1.0f - std::min(0.2f, std::max(0.0f, cwndRatio - 0.1f));

	/*
	* Scale down based on weigthed average high percentile of frame sizes
	*/
	rateLeft /= std::max(1.1f,relFrameSizeHigh);

	/*
	* Compensation for packetization overhead, important when MSS is small
	* This should actually be done per stream but is done here for simplicity
	*/
	double overheadScale = getMss() / float(getMss() + kPacketOverhead);
	rateLeft *= overheadScale;

	float prioritySum = 0.0f;
	/*
	* Calculate sum of priorities
	*/
	for (int n = 0; n < nStreams; n++) {
		Stream* stream = streams[n];
		stream->isMaxrate = false;
		stream->rateShare = 0.0f;
		if (stream->isActive)
			prioritySum += stream->targetPriority;
	}

	/*
	* Walk through and allocate rates according to priority 
	* until nothing left
	*/
	while (rateLeft > 1.0f && prioritySum > 0.01f) {
		/*
		* Allocate rates
		*/
		for (int n = 0; n < nStreams; n++) {
			Stream* stream = streams[n];
			if (stream->isActive && !stream->isMaxrate) {
				float tmp = rateLeft * stream->targetPriority / prioritySum;
				stream->rateShare += tmp;
			}
		}
		/*
		* Walk through again to see if some rate shares are higher than max bitrate
		* and allocate surplus for next iteration
		*/
		rateLeft = 0.0f;
		for (int n = 0; n < nStreams; n++) {
			Stream* stream = streams[n];
			if (stream->rateShare > stream->maxBitrate && !stream->isMaxrate) {
				float tmp = stream->rateShare - stream->maxBitrate;
				rateLeft += tmp;
				stream->rateShare = stream->maxBitrate;
				prioritySum -= stream->targetPriority;
				stream->isMaxrate = true;
			}
		}
	}

}

/*
* Update base OWD (if needed) and return the
* last estimated OWD (without offset compensation)
*/
void ScreamV2Tx::estimateOwd(uint32_t time_ntp) {
	baseOwd = std::min(baseOwd, ackedOwd);
	if (time_ntp - lastBaseOwdAddT_ntp >= kBaseDelayUpdateInterval_ntp) {
		if (enableClockDriftCompensation) {
			/*
			* The clock drift compensation looks at how the baseOwd increases over time
			* and calculates a compensation factor that advanced the sender clock stamps
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
					* clock drift compensation
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
		if (time_ntp - lastBaseDelayRefreshT_ntp > kBaseDelayUpdateInterval_ntp * (kBaseOwdHistSize - 1)) {
			lastBaseDelayRefreshT_ntp = time_ntp;
		}
	}
}

/*
* Get the base one way delay
*/
uint32_t ScreamV2Tx::getBaseOwd() {
	return  std::min(baseOwd, baseOwdHistMin);
}

/*
* Get the queue delay fraction
*/
float ScreamV2Tx::getQueueDelayFraction() {
	return queueDelay / queueDelayTarget;
}

/*
* Compute indicators of shared bottleneck
*/
void ScreamV2Tx::computeSbd() {
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
bool ScreamV2Tx::isCompetingFlows() {
	return queueDelayTarget > queueDelayTargetMin;
}

/*
* Determine active streams
*/
void ScreamV2Tx::determineActiveStreams(uint32_t time_ntp) {
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
void ScreamV2Tx::addCredit(uint32_t time_ntp, Stream* servedStream, int transmittedBytes) {
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
		Stream* tmp = streams[n];
		if (tmp != servedStream) {
			int credit = (int)(transmittedBytes * tmp->targetPriority * servedStream->targetPriorityInv);
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
void ScreamV2Tx::subtractCredit(uint32_t time_ntp, Stream* servedStream, int transmittedBytes) {
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
* Get the prioritized stream
*/
ScreamV2Tx::Stream* ScreamV2Tx::getPrioritizedStream(uint32_t time_ntp) {
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
	Stream* stream = NULL;
	/*
	* Pick a stream with credit higher or equal to
	* the size of the next RTP packet in queue for the given stream.
	*/
	uint32_t maxDiff = 0;
	for (int n = 0; n < nStreams; n++) {
		Stream* tmp = streams[n];
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
		Stream* tmp = streams[n];
		float priority = tmp->targetPriority;
		if (tmp->rtpQueue->sizeOfQueue() > 0 && priority > maxPrio) {
			maxPrio = priority;
			stream = tmp;
		}
	}
	return stream;
}

int ScreamV2Tx::getMss() {
	return std::max(mss, prevMss);
}

void ScreamV2Tx::setMssListMinPacketsInFlight(int* mssList_, int nMssListItems_, int minPacketsInFlight_) {
	minPacketsInFlight = minPacketsInFlight_;
	nMssListItems = nMssListItems_;
	memcpy(mssList, mssList_, nMssListItems*sizeof(int));
	mssIndex = nMssListItems - 1;
}

int ScreamV2Tx::getRecommendedMss(uint32_t time_ntp) {
	if (minPacketsInFlight == 0) {
		mssIndex = nMssListItems - 1;
	} else {
		/*
		* Compute a max mss given the current cwnd and the min desired packets in flight
		*/
		double maxMss = cwnd / ((double)minPacketsInFlight);
		/*
		* Step with a hysteresis to avoid that mss jumps up and down often
		*/
		if (maxMss > mssList[mssIndex]) {
			/*
			* Step up only if the preferred mss is higher than the next higher value
			*/
			if (time_ntp - lastMssChange_ntp > kMssHoldTime &&
				mssIndex < nMssListItems - 1 && 
				maxMss > mssList[mssIndex + 1]) {
				mssIndex++;
				lastMssChange_ntp = time_ntp;
			}
		} else if (maxMss < mssList[mssIndex]) {
			/*
			* Step down only if the preferred mss is higher than the next lower value
			*/
			if (mssIndex > 0 && maxMss < mssList[mssIndex - 1]) {
				mssIndex--;
				lastMssChange_ntp = time_ntp;
			}
		}
	}

	return mssList[mssIndex];
}


