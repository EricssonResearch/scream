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

// Rate update interval
static const uint32_t kRateAdjustInterval_ntp = 1311; // 20ms in NTP domain

// ==== Less important tuning parameters ====
// Min pacing interval and min pacing rate
static const float kMinPaceInterval = 10e-6f;
// Initial MSS, this is set quite low in order to make it possible to
//  use SCReAM with audio only
static const int kInitMss = 100;

// Min and max queue delay target
static const float kQueueDelayTargetMax = 0.3f; //ms

// Queue delay trend and shared bottleneck detection
static const uint32_t kQueueDelayFractionHistInterval_us = 3277; // 50ms in NTP domain
// video rate estimation update period
static const uint32_t kRateUpdateInterval_ntp = 3277;  // 50ms in NTP domain

// Packet reordering margin (us)
static const uint32_t kReordertime_ntp = 655;  // 10ms in NTP domain

// Update interval for base delay history
static const uint32_t kBaseDelayUpdateInterval_ntp = 655360; // 10s in NTP doain

// L4S alpha gain factor, for scalable congestion control
static const float kL4sG = 1.0f/16;

// L4S alpha max value, for scalable congestion control
static const float kL4sAlphaMax = 1.0;

// Min CWND in MSS
static const int kMinCwndMss = 3;

// Compensate for max 33/65536 = 0.05% clock drift
static const uint32_t kMaxClockdriftCompensation = 33;

// Time stamp scale
static const int kTimeStampAtoScale = 1024;


// For SSRT histogram
static const float kSrttHistDecay = 1.0f / 512; // Roughly 10s decay time
static const float kSrttHistBinSize = 0.001f;
static const float kSrttHighPercentile = 0.5f;
static const float kSrttLowPercentile = 0.1f;

static const float kSrttVirtual = 0.025f; // Virtual SRTT, similar to Prague CC

static const float kCeDensityAlpha = 1.0f / 16;

static const float kCwndOverheadL4s = 2.0f;
static const float kCwndOverheadNoL4s = 1.0f;

static const float kLowCwndScaleFactor = 0.1f;

static const int kMssUpdateInterval_ntp = 65536;

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
enableSbd(enableSbd_),
packetPacingHeadroom(packetPacingHeadroom_),
bytesInFlightHeadRoom(bytesInFlightHeadRoom_),
isL4s(isL4s_),
openWindow(openWindow_),
enableClockDriftCompensation(enableClockDriftCompensation_),
maxAdaptivePacingRateScale(maxAdaptivePacingRateScale_),
multiplicativeIncreaseScalefactor(multiplicativeIncreaseScalefactor_),
sRtt(0.02f), // Init SRTT to 20ms
sRtt_ntp(1311),
sRttSh_ntp(1311),
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
bytesNewlyAckedCe(0),
mss(kInitMss),
cwnd(kInitMss * 2),
cwndMin(kInitMss * 2),
cwndMinLow(0),
cwndI(1),
lastMssUpdateT_ntp(0),
isAutoTuneMinCwnd(false),
prevBytesInFlight(0),
maxBytesInFlight(0),
maxBytesInFlightPrev(0),

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
baseOwdResetT_ntp(0),
lastAddToQueueDelayFractionHistT_ntp(0),
lastLossEventT_ntp(0),
lastCeEventT_ntp(0),
lastTransmitT_ntp(0),
nextTransmitT_ntp(0),
lastRateUpdateT_ntp(0),
lastCwndIUpdateT_ntp(0),
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
packetsMarkedThisRtt(0),
packetsDeliveredThisRtt(0),
initTime_ntp(0),
lastCongestionDetectedT_ntp(0),
lastRttT_ntp(0),
queueDelaySbdSkew(0)
{
	strcpy(detailedLogExtraData, "");
	strcpy(timeString, "");

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
	nStreams = 0;
	isNewFrame = false;
	for (int n = 0; n < kMaxStreams; n++)
		streams[n] = NULL;

	for (int n = 0; n < kMaxBytesInFlightHistSize; n++)
		maxBytesInFlightHist[n] = NULL;
	maxBytesInFlightHistIx = 0;

	queueDelayMax = 0.0f;
	queueDelayMinAvg = 0.0f;
	queueDelayMin = 1000.0;
}

ScreamV2Tx::~ScreamV2Tx() {
	for (int n = 0; n < nStreams; n++)
		delete streams[n];
}


/*
* Register new stream
*/
void ScreamV2Tx::registerNewStream(RtpQueueIface *rtpQueue,
	uint32_t ssrc,
	float priority,
	float minBitrate,
	float startBitrate,
	float maxBitrate,
	float maxRtpQueueDelay,
	bool isAdaptiveTargetRateScale,
  float hysteresis) {
	Stream *stream = new Stream(this,
		rtpQueue,
		ssrc,
		priority,
		minBitrate,
		startBitrate,
		maxBitrate,
		maxRtpQueueDelay,
		isAdaptiveTargetRateScale,
	  hysteresis);
	streams[nStreams++] = stream;
}

void ScreamV2Tx::updateBitrateStream(uint32_t ssrc,
	float minBitrate,
	float maxBitrate) {
	int id;
	Stream *stream = getStream(ssrc, id);
	stream->minBitrate = minBitrate;
	stream->maxBitrate = maxBitrate;
}

RtpQueueIface * ScreamV2Tx::getStreamQueue(uint32_t ssrc) {
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
	Stream *stream = getStream(ssrc, id);

	stream->newMediaFrame(time_ntp, bytesRtp, isMarker);
	stream->updateTargetBitrate(time_ntp);

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
		cwnd = std::max(cwnd, cwndMin);
	}
}

/*
* Determine if OK to transmit RTP packet
*/
float ScreamV2Tx::isOkToTransmit(uint32_t time_ntp, uint32_t &ssrc) {
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
		mss = kInitMss;
		if (!openWindow)
			cwndMin = std::max(cwndMinLow, kMinCwndMss * mss);
		cwnd = max(cwnd, cwndMin);

		/*
		* Add a small clock drift compensation
		*  for the case that the receiver clock is faster
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
		float cwndOverhead = kCwndOverheadNoL4s;
		if (isL4sActive) {
			cwndOverhead = kCwndOverheadL4s;
		}
		exit = (bytesInFlight + sizeOfNextRtp) > cwnd * cwndOverhead + mss;
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
void ScreamV2Tx::incomingStandardizedFeedback(uint32_t time_ntp,
	int streamId,
	uint32_t timestamp,
	uint16_t seqNr,
	uint8_t ceBits,
	bool isLast) {

	Stream *stream = streams[streamId];
	Transmitted *txPackets = stream->txPackets;
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
		if (isCeThisFeedback && time_ntp - lastLossEventT_ntp > std::min(1966u, sRtt_ntp)) { // CE event at least every 30ms
			ecnCeEvent = true;
			lastLossEventT_ntp = time_ntp;
			lastCeEventT_ntp = time_ntp;

		}

		if (!isEnablePacketPacing) {
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
					//fractionMarked = float(bytesMarkedThisRtt) / float(bytesDeliveredThisRtt);
					fractionMarked = float(packetsMarkedThisRtt) / float(packetsDeliveredThisRtt);
					if (fractionMarked == 1.0) {
						/*
						 * Likely a fast reduction in throughput, reset ceDensity
						 *  so that CWND is reduced properly
						 */
						ceDensity = 1.0;
					}

					fractionMarked *= ceDensity;

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
					packetsDeliveredThisRtt = 0;
					packetsMarkedThisRtt = 0;
				}
			}
		}

		if (lossEvent || ecnCeEvent) {
			lastLossEventT_ntp = time_ntp;
		}

		if (lastCwndUpdateT_ntp == 0)
			lastCwndUpdateT_ntp = time_ntp;

		if (time_ntp - lastCwndUpdateT_ntp > 1966u || lossEvent || ecnCeEvent || isNewFrame) {
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
	isL4sActive = true ||  isL4s && (time_ntp - lastCeEventT_ntp < (10 * 65535)); // L4S enabled and at least one CE event the last 10 seconds

	if (isUseExtraDetailedLog || isLast || isMark) {
		if (fp_log && completeLogItem) {
			fprintf(fp_log, " %d,%d,%d,%1.0f,%d,%d,%d,%d,%1.0f,%1.0f,%1.0f,%1.0f,%1.0f,%d",
                    cwnd, bytesInFlight, 0, rateTransmittedLog, streamId, seqNr, bytesNewlyAckedLog, ecnCeMarkedBytesLog, stream->rateRtpLog, stream->rateTransmittedLog, stream->rateAckedLog, stream->rateLostLog, stream->rateCeLog, isMark);
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
			packetsDeliveredThisRtt += 1;
			isMark = tmp->isMark;
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
void ScreamV2Tx::detectLoss(uint32_t time_ntp, struct Transmitted *txPackets, uint16_t highestSeqNr, Stream *stream) {
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

float ScreamV2Tx::getTargetBitrate(uint32_t ssrc) {
	int id;
	return  getStream(ssrc, id)->getTargetBitrate();
}

void ScreamV2Tx::setTargetPriority(uint32_t ssrc, float priority) {
	int id;
	Stream *stream = getStream(ssrc, id);
	if (queueDelayFractionAvg > 0.1) {
		stream->targetBitrate *= priority / stream->targetPriority;
		stream->targetBitrate = std::min(std::max(stream->targetBitrate, stream->minBitrate), stream->maxBitrate);
	}
	stream->targetPriority = priority;
	stream->targetPriorityInv = 1.0f / priority;
}

void ScreamV2Tx::getLogHeader(char *s) {
	sprintf(s,
            "LogName,queueDelay,queueDelayMax,queueDelayMinAvg,sRtt,cwnd,bytesInFlightLog,rateTransmitted,isInFastStart,rtpQueueDelay,bytes,size,targetBitrate,rateRtp,packetsRtp,rateTransmittedStream,rateAcked,rateLost,rateCe, packetsCe,hiSeqTx,hiSeqAck,SeqDiff,packetetsRtpCleared,packetsLost");
}

void ScreamV2Tx::getLog(float time, char *s, uint32_t ssrc, bool clear) {
	int inFlightMax = bytesInFlight;
	sprintf(s, "%s Log, %4.3f, %4.3f, %4.3f, %4.3f, %6d, %6d, %6.0f, %1d, ",
		logTag, queueDelay, queueDelayMax, queueDelayMinAvg, sRtt,
		cwnd, bytesInFlightLog, rateTransmittedLog / 1000.0f, 0);
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

void ScreamV2Tx::getShortLog(float time, char *s) {
	int inFlightMax = bytesInFlight;
	sprintf(s, "%s %4.3f, %4.3f, %6d, %6d, %6.0f, %1d, ",
		logTag, queueDelay, sRtt,
		cwnd, bytesInFlightLog, rateTransmitted / 1000.0f, 0);
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

void ScreamV2Tx::getVeryShortLog(float time, char *s) {
	int inFlightMax = bytesInFlight;
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

float ScreamV2Tx::getQualityIndex(float time, float thresholdRate, float rttMin) {
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

bool ScreamV2Tx::isLossEpoch(uint32_t ssrc) {
	int id;
	return  getStream(ssrc, id)->isLossEpoch();
}

void ScreamV2Tx::initialize(uint32_t time_ntp) {
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
	lastRttT_ntp = time_ntp;
	lastBaseDelayRefreshT_ntp = time_ntp - 1;
	lastL4sAlphaUpdateT_ntp = time_ntp;
	lastMssUpdateT_ntp = time_ntp;
	initTime_ntp = time_ntp;
	lastCongestionDetectedT_ntp = 0;
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
* Update the  congestion window
*/
void ScreamV2Tx::updateCwnd(uint32_t time_ntp) {
	if (lastCwndUpdateT_ntp == 0)
		lastCwndUpdateT_ntp = time_ntp;

	postCongestionScale = std::max(0.0f, std::min(1.0f, (time_ntp - lastCongestionDetectedT_ntp) / (postCongestionDelay * 65536.0f)));

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

		float headroom = packetPacingHeadroom * adaptivePacingRateScale;
		if (lastCongestionDetectedT_ntp == 0) {
			/*
			* Increase packet pacing headroom until 1st congestion event, reduces problem
			*  with initial queueing in the RTP queue
			*/
			headroom = std::max(headroom, 1.5f);
		}

		float pacingBitrate = std::max(getTotalTargetBitrate(), rateRtp);
		pacingBitrate = std::max(50e3f, headroom * pacingBitrate);
		if (maxTotalBitrate > 0) {
			pacingBitrate = std::min(pacingBitrate, maxTotalBitrate * packetPacingHeadroom);
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

		lastAddToQueueDelayFractionHistT_ntp = time_ntp;
	}

	/*
	* offTarget is a normalized deviation from the queue delay target
	*/
	float offTarget = (queueDelayTarget - queueDelay) / float(queueDelayTarget);

	/*
	 * At very low congestion windows (just a few MSS) the up and down scaling
	 * of CWND is scaled down. This is needed as there are just a few packets in flight
	 * and the congestion marking this gets more unsteady.
	 * The drawback is that reaction to congestion becomes slower.
	 * Reducing the MSS when bitrate is low is the best option if fast reaction to congestion
	 * is desired.
	 */
	float cwndScaleFactor = (kLowCwndScaleFactor + (multiplicativeIncreaseScalefactor * cwnd) / mss);

	if (lossEvent) {
		/*
		* Update inflexion point
		*/
		if (time_ntp - lastCwndIUpdateT_ntp > 16384) {
			lastCwndIUpdateT_ntp = time_ntp;
			cwndI = cwnd;
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
	else if (ecnCeEvent) {
		/*
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
			float backOff = l4sAlpha / 2.0f;
			/*
			* Additional compensation is needed to compensate for 
			* small CWND as well as the packet pacing headroom
			* The drawback is that reaction to congestion can become 
			*  reduced, especially at very low bitrates
			*/

			/*
			 * Compensate for very small cwndRatios
			 * i.e CWND is almost as small as MSS
			 */
			backOff *= std::min(1.0f, cwndScaleFactor);

			/*
			* Hmm.. Yes.. This is a bit of voodoo magic.. and
			*  actually on top of previous scaling 
			* This extra compensation is to make sure that bitrate does not 
			*  become too low at high CE rates == low CWND/MSS
			*/
			backOff *= std::max(0.8f,1.0f-cwndRatio*2.0f);

			/*
			 * Scale down back off to so that the bitrate follows the
			 *   CWND = 2/pMark, considering that an equal scale down
			 *   is done in the rate control
			 * With this trick it is possible to keep a decent packet pacing
			 *  headroom and still have a high link utilization
			 */
			backOff /= 1.0f + (packetPacingHeadroom - 1.0f) / 2;

			if (time_ntp - lastCongestionDetectedT_ntp > 65536 * 5.0f) {
				/*
				 * A long time since last congested because link throughput
				 *  exceeds max video bitrate.
				 * There is a certain risk that CWND has increased way above
				 *  bytes in flight, so we reduce it here to get it better on track
				 *  and thus the congestion episode is shortened
				 */
				cwnd = std::min(cwnd, maxBytesInFlightPrev);
				/*
				 * Also, we back off a little extra if needed
				 *  because alpha is quite likely very low
				 * This can in some cases be an over-reaction though
				 *  but as this function should kick in relatively seldom
				 *  it should not be to too big concern
				 */
				backOff = std::max(backOff, 0.25f);
			}
			/*
			* Scale down CWND
			*/
			cwnd = std::max(cwndMin, (int)((1.0f - backOff) * cwnd));
		}
		else {
			/*
			* Scale down CWND
			*/
			cwnd = std::max(cwndMin, (int)(ecnCeBeta * cwnd));
		}
		ecnCeEvent = false;
		lastCongestionDetectedT_ntp = time_ntp;

		wasLossEvent = true;
		postCongestionScale = 0.0;
	}
	cwndRatio = float(mss) / cwnd;

	if (time_ntp - lastRttT_ntp > sRtt_ntp) {
		if (wasLossEvent)
			lossEventRate = 0.99f * lossEventRate + 0.01f;
		else
			lossEventRate *= 0.99f;
		wasLossEvent = false;
		lastRttT_ntp = time_ntp;
	}

	float halfPacketPacingHeadroom = 1.0f + (packetPacingHeadroom - 1) / 2;
	if (offTarget > 0.0f) {
		/*
		* Compute max increment based on bytes acked
		*  but subtract number of CE marked bytes
		*/
		int bytesAckedMinusCe = bytesNewlyAcked - bytesNewlyAckedCe;
		float increment = (kGainUp * offTarget * bytesAckedMinusCe) * cwndRatio;

		/*
		* Reduce increment for very small RTTs
		*/
		float tmp = std::min(1.0f, sRtt / kSrttVirtual);
		increment *= tmp * tmp;

		if (!isL4sActive) {
			float sclI = 1.0;
			sclI = float(cwnd - cwndI) / cwndI;
			sclI *= 4;
			sclI = sclI * sclI;
			sclI = std::max(0.1f, std::min(1.0f, sclI));
			increment *= sclI;
		}

		/*
		 * Slow down CWND increase when CWND is only a few MSS
		 * This goes hand in hand with that the down scaling is also
		 * slowed down then
		 */
		float tmp2 = cwndScaleFactor;
		/*
		* Furher limit multiplicative increase when congestion occured
		*  recently
		*/
		if (tmp2 > 1.0 && postCongestionDelay > 0.2f) {
			tmp2 = 1.0 + ((tmp2 - 1.0) * postCongestionScale);
		}
		increment *= tmp2;
		/*
		* Increase CWND only if bytes in flight is large enough
		* Quite a lot of slack is allowed here to avoid that bitrate locks to 
		*  low values.
		*/
		double maxAllowed = mss + std::max(maxBytesInFlight, maxBytesInFlightPrev) * bytesInFlightHeadRoom;
		int cwndTmp = cwnd + (int)(increment + 0.5f);
		if (cwndTmp <= maxAllowed)
			cwnd = cwndTmp;
	} else {
		/*
		* Update inflexion point
		*/
		if (time_ntp - lastCwndIUpdateT_ntp > 16384) {
			lastCwndIUpdateT_ntp = time_ntp;
			cwndI = cwnd;
		}
		/*
		* Queue delay above target.
		* Limit the CWND reduction to at most a quarter window
		*  this avoids unduly large reductions for the cases
		*  where data is queued up e.g because of retransmissions
		*  on lower protocol layers.
		*/
		float delta = -(kGainDown * offTarget * bytesNewlyAcked * cwndRatio);
		delta = std::min(delta, cwnd / 4.0f);

		cwnd -= (int)(delta);

		lastCongestionDetectedT_ntp = time_ntp;
	}

	cwnd = std::max(cwndMin, cwnd);

	if (maxTotalBitrate > 0) {
		if (isL4sActive) {
			/*
			* Scale up one half pacing headroom to compensate for the same
			*  downscaling in the stream rate control
			*/
			cwnd = std::min(cwnd, int(maxTotalBitrate * halfPacketPacingHeadroom * sRtt / 8));
		} else {

			/*
			* Scale up one full pacing headroom to compensate for the same
			*  downscaling in the stream rate control
			*/
			cwnd = std::min(cwnd, int(maxTotalBitrate * packetPacingHeadroom * sRtt / 8));
		}
	}


	bytesNewlyAcked = 0;
	bytesNewlyAckedCe = 0;
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
* Compute congestion indicator
*/
void ScreamV2Tx::computeQueueDelayTrend() {
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
* Get queue delay trend
*/
float ScreamV2Tx::getQueueDelayTrend() {
	return queueDelayTrend;
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
