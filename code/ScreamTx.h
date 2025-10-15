#ifndef SCREAM_TX
#define SCREAM_TX

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
//#include <algorithm>
#include <math.h>
#include <cmath>
#include <cstdint>
extern "C" {
	/*
	* This module implements the sender side of SCReAM,
	*  see https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pdf
	*  for details on how it is integrated in audio/video platforms
	* A full implementation needs the additional code for
	*  + RTP queue(s), one queue per stream, see SCReAM description for interface description
	*  + Other obvious stuff such as RTP payload packetizer, video+audio capture, coders....
	*/

	/*
	* Internal time is represented as the mid 32 bits of the NTP timestamp (see RFC5905)
	* This means that the high order 16 bits is time in seconds and the low order 16 bits
	* is the fraction. The NTP time stamp is thus in Q16 i.e 1.0sec is represented
	* by the value 65536.
	* All internal time is measured in NTP time, this is done to avoid wraparound issues
	* that can otherwise occur every 18h hour or so
	*/

	// ==== Default parameters (if tuning necessary) ====
	// Connection related default parameters
	// CWND scale factor upon loss event
	static const float kLossBeta = 0.7f;
	// CWND scale factor upon ECN-CE event
	static const float kEcnCeBeta = 0.9f;
	// Min and max queue delay target
	static const float kQueueDelayTargetMin = 0.1f; //ms
	// Enable shared botleneck detection and queue delay target adjustement
	// good if SCReAM needs to compete with e.g FTP but
	// Can in some cases cause self-inflicted congestion
	//  i.e the e2e delay can become large even though
	//  there is no competing traffic present
	//  For some reason, self-inflicted congestion is easily triggered
	//  when an audio + video stream is run, so bottomline is that
	//  this feature is a bit shaky
	static const bool kEnableSbd = false;
	// CWND up and down gain factors
	static const float kGainUp = 1.0f;
	static const float kGainDown = 2.0f;

	// Stream related default parameters
	// Max video rampup speed in bps/s (bits per second increase per second)
	static const float kRampUpSpeed = 200000.0f; // bps/s
	// Max video rampup scale as fraction of the current target bitrate
	static const float kRampUpScale = 0.2f;
	// Max RTP queue delay, RTP queue is cleared if this value is exceeded
	static const float kMaxRtpQueueDelay = 0.1f;  // 0.1s
	// Compensation factor for RTP queue size
	// A higher value such as 0.2 gives less jitter esp. in wireless (LTE)
	// but potentially also lower link utilization
	static const float kTxQueueSizeFactor = 0.2f;
	// Compensation factor for detected congestion in rate computation
	// A higher value such as 0.5 gives less jitter esp. in wireless (LTE)
	// but potentially also lower link utilization
	static const float kQueueDelayGuard = 0.1f;
	// Video rate scaling due to loss events
	static const float kLossEventRateScale = 0.9f;
	// Video rate scaling due to ECN marking events
	static const float kEcnCeEventRateScale = 0.95f;
	// Headroom for packet pacing
	static const float kPacketPacingHeadRoom = 1.5f;
	static const float kMaxAdaptivePacingRateScale = 1.5;

	// Bytes in flight headroom
	static const float kBytesInFlightHeadRoom = 2.0f;
	// A multiplicative increase factor, 0.05 means that CWND can increase at most 5% per RTT
	static const float kMultiplicativeIncreaseScalefactor = 0.05f;
	// Packet reordering margin
	static const float kReorderTime = 0.03f;    

	static const float ntp2SecScaleFactor = 1.0f / 65536;
	static const uint32_t sec2NtpScaleFactor = 65536u;

	/*
	* Max number of RTP packets in flight
	* With an MSS = 1200 byte and an RTT = 50ms
	* this is enough to support media bitrates up to ~800Mbps
	* Note, 65536 % kMaxTxPackets must be zero
	*/
	static const int kMaxTxPackets = 4096;
	/*
	* Max number of streams
	*/
	static const int kMaxStreams = 10;
	/*
	* History vectors
	*/
	static const int kBaseOwdHistSize = 50;
	static const int kQueueDelayNormHistSize = 200;
	static const int kQueueDelayNormHistSizeSh = 50;
	static const int kQueueDelayFractionHistSize = 20;
	static const int kBytesInFlightHistSizeMax = 60;
	static const int kRateUpDateSize = 4;
	static const int kTargetBitrateHistSize = 3;
	static const int kLossRateHistSize = 10;
	static const int kSrttHistBins = 200;
	static const int kRelFrameSizeHistBins = 50;
	static const int kMaxBytesInFlightHistSize = 20;
	static const int kCwndIHistSize = 3;
	static const int kMssListSize = 10;

	enum StatisticsItem {
		AVG_RATE,          // [bps]
		LOSS_RATE,         // [%]
		LOSS_RATE_LONG,    // [%]
		CE_RATE,           // [%]
		CE_RATE_LONG,      // [%]
		AVG_RTT,           // [s]
		AVG_QUEUE_DELAY
	}; // [s]

	class ScreamTx {
	public:
		ScreamTx();
		~ScreamTx();

		/*
		* Statistics for the network congestion control and the
		*  stream[0]
		*/
		class Statistics {
		public:
			Statistics(ScreamTx* parent);
			void getSummary(float time, char s[]);
			void add(uint32_t time_ntp, float rateTx, float rateLost, float rateCe, float rtt, float queueDelay);
			void addEcn(uint8_t ecn);
			float getStatisticsItem(StatisticsItem item);
    		void printFinalSummary();
		private:
			float lossRateHist[kLossRateHistSize];
			float ceRateHist[kLossRateHistSize];
			float rateLostAcc;
			float rateCeAcc;
			int rateLostN;
			int lossRateHistPtr;
			float avgRateTx;
			float minRate;
			float maxRate;
			float avgRtt;
			float sumRtt;
			float minRtt;
			float maxRtt;
			float avgQueueDelay;
			float maxQueueDelay;
			float sumQueueDelay;
			float sumRateTx;
			float sumRateLost;
			float sumRateCe;
			float lossRate;
			float ceRate;
			float lossRateLong;
			float ceRateLong;
			int nStatisticsItems;
			int n00; // Number of not-ECT packets
			int n10; // Number of ECT(0) packets
			int n01; // Number of ECT(1) packets
			int n11; // Number of CE packets
			int nEcn; // Number of packets that are any of the ECN code points
			ScreamTx* parent;
		};
		Statistics* statistics;
	public:

		/*
		* Get overall simplified statistics
		*/
		void getStatistics(float time, char* s);

		/*
		* Get statisticts items indexed by enum StatisticsItem
		*/
		float getStatisticsItem(StatisticsItem item);

		/*
		* Print final summary on stdout
		*/
		void printFinalSummary();

		void setLogTag(char* logTag_) {
			logTag = logTag_;
		};
		const char* logTag = "";
	};

	class RtpQueueIface;
	class ScreamV2Tx : public ScreamTx {
	public:
		/*
		* Constructor
		* lossBeta sets how the congestion window should be scaled when packet loss is detected
		*  lossBeta = 1.0 means that packet losses are ignored by the congestion control
		* ecnCeBeta sets how the congestion window should be scaled when ECN-CE is detected
		*  this applies only to classic ECN
		* queueDelayTargetMin sets the queue delay targer
		* cwnd > 0 sets a different initial congestion window, for example it can be set to
		*  initialrate/8*rtt
		* packetPacingHeadroom sets how much faster the video frames are transmitted, relative to the target bitrate
		* maxAdaptivePacingRateScale > 1.0 enables adaptive scaling of the pacing rate to transmit large frames faster.
		*  This feature is mostly relevant with L4S and has the potential to reduce unecessary queue build-up in the
		*  RTP queue, but can also potentially increase network queue build-up
		* multiplicativeIncreaseScaleFactor, indicates how fast CWND can increase when link capacity
		*  increases. E.g 0.05 increases up tp 5% of the CWND per RTT.
		* isL4s = true changes congestion window reaction to ECN marking to a scalable function, similar to DCTCP
		* windowHeadroom, sets how much bytes in flight can exceed the congestion window
		* enableSbd increases the delay target if competing buffer building flows are detected
		* enableClockDriftCompensation = true compensates for the case where the endpoints clocks drift relative to one
		*  another. Note though that clock drift is not always montonous IRL
		*
		*/
		ScreamV2Tx(float lossBeta = kLossBeta,
			float ecnCeBeta = kEcnCeBeta,
			float queueDelayTargetMin = kQueueDelayTargetMin,
			int cwnd = 0,  // An initial cwnd larger than 2*mss
			float packetPacingHeadroom = kPacketPacingHeadRoom,
			float maxAdaptivePacingRateScale = kMaxAdaptivePacingRateScale,
			float bytesInFlightHeadRoom = kBytesInFlightHeadRoom,
			float multiplicativeIncreaseScalefactor = kMultiplicativeIncreaseScalefactor,
			bool isL4s = false,
			float windowHeadroom = 5.0f,
			bool enableSbd = kEnableSbd,
			bool enableClockDriftCompensation = false);

		~ScreamV2Tx();

		/*
		* Register a new stream {SSRC,PT} tuple,
		*  with a priority value in the range ]0.0..1.0]
		*  where 1.0 denotes the highest priority.
		* It is recommended that at least one stream has prioritity 1.0.
		* Bitrates are specified in bps
		* maxRtpQueueDelay sets how long RTP packets can be held in buffer before entire buffer is cleared
		* isAdaptiveTargetRateScale compensates for deviations from target bitrates
		* hysteresis sets how much the target rate should change for the getTargetBitrate() function to
		*  return a changed value. hysteresis = 0.1 sets a +10%/-5% hysteresis.
		*  This can benefit video encoders that become confused by too frequent rate updates.
		* enableFrameSizeOverhead (default true) adds additional safety margin 
		*  when frame sizes vary a lot.
		*/
		void registerNewStream(RtpQueueIface* rtpQueue,
			uint32_t ssrc,
			float priority,     // priority in range ]0.0 .. 1.0], 1.0 is highest
			float minBitrate,   // Min target bitrate
			float startBitrate, // Starting bitrate
			float maxBitrate,   // Max target bitrate
			float maxRtpQueueDelay = kMaxRtpQueueDelay,
			bool isAdaptiveTargetRateScale = true,
			float hysteresis = 0.0,
			bool enableFrameSizeOverhead=true);

		/*
		* Updates the min and max bitrates for an existing stream
		*/
		void updateBitrateStream(uint32_t ssrc,
			float minBitrate,
			float maxBitrate);

		/*
		* Access the configured RtpQueue of an existing stream
		*/
		RtpQueueIface* getStreamQueue(uint32_t ssrc);

		/*
		* Call this function for each new video frame
		*  Note : isOkToTransmit should be called after newMediaFrame
		*/
		void newMediaFrame(uint32_t time_ntp, uint32_t ssrc, int bytesRtp, bool isMarker);

		/*
		* Function determines if an RTP packet with SSRC can be transmitted
		* Return values :
		* 0.0  : RTP packet with SSRC can be immediately transmitted
		*  addTransmitted must be called if packet is transmitted as a result of this
		* >0.0 : Time [s] until this function should be called again
		*   This can be used to start a timer
		*   Note that a call to newMediaFrame or incomingFeedback should
		*    cause an immediate call to isOkToTransmit
		* -1.0 : No RTP packet available to transmit or send window is not large enough
		*/
		float isOkToTransmit(uint32_t time_ntp, uint32_t& ssrc);

		/*
		* Add packet to list of transmitted packets
		* should be called when an RTP packet transmitted
		* Return time until isOkToTransmit can be called again
		*/
		float addTransmitted(uint32_t timestamp_ntp, // Wall clock ts when packet is transmitted
			uint32_t ssrc,
			int size,
			uint16_t seqNr,
			bool isMark,
			float rtpQueueDelay = 0.0f,
			uint32_t timeStamp = 0
		);

		/* New incoming feedback, this function
		* triggers a CWND update
		* The SCReAM timestamp is in jiffies, where the frequency is controlled
		* by the timestamp clock frequency(default 1000Hz)
		* The ackVector indicates recption of the 64 RTP SN prior to highestSeqNr
		*  Note : isOkToTransmit should be called after incomingFeedback
		*
		* Parse standardized feedback according to
		* https://tools.ietf.org/wg/avtcore/draft-ietf-avtcore-cc-feedback-message/
		* Current implementation implements -02 version
		* It is assumed that SR/RR or other non-CCFB feedback is stripped
		*/
		void incomingStandardizedFeedback(uint32_t time_ntp,
			unsigned char* buf,
			int size);

		void incomingStandardizedFeedback(uint32_t time_ntp,
			int streamId,
			uint32_t timestamp,
			uint16_t seqNr,
			uint8_t ceBits,
			bool isLast);
		/*
		* Get the target bitrate for stream with SSRC
		* NOTE!, Because SCReAM operates on RTP packets, the target bitrate will
		*  also include the RTP overhead. This means that a subsequent call to set the
		*  media coder target bitrate must subtract an estimate of the RTP + framing
		*  overhead. This is not critical for Video bitrates but can be important
		*  when SCReAM is used to congestion control e.g low bitrate audio streams
		* Function returns -1 if a loss is detected, this signal can be used to
		*  request a new key frame from a video encoder
		*/
		float getTargetBitrate(uint32_t time_ntp, uint32_t ssrc);

		/*
		* Set target priority for a given stream, priority value should be in range ]0.0..1.0]
		*/
		void setTargetPriority(uint32_t ssrc, float aPriority);

		/*
		* Set maxTotalBitrate
		* This featire is useful if it is known that for instance a cellular modem does not support a higher uplink bitrate
		* than e.g. 50Mbps. When maxTotalBitrate is set to 50Mbps, then unnecessary bandwidth probing beyond this bitrate
		* is avoided, this reduces delay jitter.
		*/
		void setMaxTotalBitrate(float aMaxTotalBitrate) {
			maxTotalBitrate = aMaxTotalBitrate;
		}

		/*
		* Get maxTotalBitrate
		*/
		float getMaxTotalBitrate() {
			return maxTotalBitrate;
		}

		/*
		* Get verbose log information
		*/
		void getLog(float time, char* s, uint32_t ssrc, bool clear);

		/*
		* Get verbose log header
		*/
		void getLogHeader(char* s);

		/*
		* Get verbose log information
		*/
		void getShortLog(float time, char* s);

		/*
		* Get verbose log information
		*/
		void getVeryShortLog(float time, char* s);

		/*
		* Set file pointer for detailed per-ACK log
		*/
		void setDetailedLogFp(FILE* fp) {
			fp_log = fp;
		}

		/*
		* Set file pointer for TX and RX timestamop 
		*/
		void setTxRxLogFp(FILE* fp) {
			fp_txrxlog = fp;
		}

		void setTimeString(char* s) {
			strcpy(timeString, s);
		}

		/*
		* extra data to be appended to detailed log
		*/
		void setDetailedLogExtraData(char* s) {
			strcpy(detailedLogExtraData, s);
		}

		/*
		* Get the list of log items
		*/
		const char* getDetailedLogItemList() {
			return "\"Time [s]\",\"Estimated queue delay [s]\",\"RTT [s]\",\"Congestion window [byte]\",\"Bytes in flight [byte]\",\"Fast increase mode\",\"Total transmit bitrate [bps]\",\"Stream ID\",\"RTP SN\",\"Bytes newly ACKed\",\"Bytes newly ACKed and CE marked\",\"Media coder bitrate [bps]\",\"Transmitted bitrate [bps]\",\"ACKed bitrate [bps]\",\"Lost bitrate [bps]\",\"CE Marked bitrate [bps]\",\"Marker bit set\"";
		}

		/*
		* Log each ACKed packet,
		*/
		void useExtraDetailedLog(bool isUseExtraDetailedLog_) {
			isUseExtraDetailedLog = isUseExtraDetailedLog_;
		}

		/*
		* Get an approximate quality index [0..100%]
		* This index is computed based on the quality for stream 0
		*/
		float getQualityIndex(float time, float thresholdRate, float rttMin);

		/*
		* Set lowest possible cwndMin
		*/
		void setCwndMinLow(int aValue) {
			cwndMinLow = aValue;
		}

		/*
		* Enable/disable rate update. This function is used with the
		* SCReAM BW test tool to avoid that the periodic bitrate reduction
		*  messes up the rate estimation
		*/
		void setEnableRateUpdate(bool isEnableRateUpdate) {
			enableRateUpdate = isEnableRateUpdate;
		}

		/*
		* Return true if loss event has occured for the given ssrc
		*/
		bool isLossEpoch(uint32_t ssrc);

		/*
		* Enable/disable packet pacing
		*/
		void enablePacketPacing(bool isEnable) {
			isEnablePacketPacing = isEnable;
		}

		/*
		* Autotune min CWND, if true, the min CWND will be autotuned according to the
		*  equation minCwnd = sum(min bitrates)/8*sRttLow
		* Use this with care because it can potentially starve out parallel traffic
		*/
		void autoTuneMinCwnd(bool isAutotune) {
			isAutoTuneMinCwnd = isAutotune;
		}

		/*
		* Set recommended list of MSSs and a min number of packets in-flight 
		* This feature allows to adapt the MSS so that a min number of packets 
		* in-flight is maintained. This can improve L4S congestion control stability 
		* when the throughput is low.
		* The mssList should be in decreasing order, for instance 300,500,800,1300. 
		* The steps should be sufficiently large to avoid a case that mss jumps up and down.
		* Note that smaller mss comes at a cost of larger packet overhead
		*/
		void setMssListMinPacketsInFlight(int* mssList, int nMssListItems, int minPacketsInFlight);

		/**
		* Enable relaxed pacing. Pacing rate is increased considerably 
		* when media rate is close to the max. This reduces e2e delay when 
		* link capacity is high
		*/
		void enableRelaxedPacing(bool enable) {
			isEnableRelaxedPacing = enable;
		}

		/**
		* Set packet reordering margin [s]
		*/
		void setReorderTime(float val) {
			reorderTime = val;
			reorderTime_ntp = uint32_t(val * sec2NtpScaleFactor + 0.5f);
		}

		/*
		* Get recommended MSS
		*/
		int getRecommendedMss(uint32_t time_ntp);

		/*
		* Get CWND
		*/
		int getCwnd() {
			return cwnd;
		}

		/*
		* Get SRtt
		*/
		int getSRtt() {
			return sRtt;
		}

		/*
		* Enable or disable delay based congestion control. In certain scenarios where L4S (or ECN) is known to be 
		* supported along the transport path it can be beneficial to disable the delay based congestion control because
		* of the issues with clock drift and clock skipping that can harm performance if L4S is supported. 
		*/
		void enableDelayBasedCongestionControl(bool enable) {
			isEnableDelayBasedCongestionControl = enable;
		}

	private:
		/*
		* Struct for list of RTP packets in flight
		*/
		struct Transmitted {
			uint32_t timeTx_ntp;
			int size;
			uint16_t seqNr;
			uint32_t timeStamp;
			float rtpQueueDelay;
			bool isMark;
			bool isUsed;
			bool isAcked;
			bool isAfterReceivedEdge;
		};

		/*
		* One instance is created for each {SSRC,PT} tuple

		* Video encoders can become confused by frequent but still small changes in
		*  target bitrate. A hysteresis reduces the frequency of small rate changes.
		* A hysteresis value = 0.1 inhibits upowards rate changes when they are less
		*  than 10% over and 2.5% under the last update value.
		* Note, a large hysteresis can cause a deadlock in the rate uopdate,
		*  hysteresis = 0.1 or less appears to work however
		* enableFrameSizeOverhead (default true) adds additional safety margin 
		*  when frame sizes vary a lot.
		*/
		class Stream {
		public:
			Stream(ScreamV2Tx* parent,
				RtpQueueIface* rtpQueue,
				uint32_t ssrc,
				float priority,
				float minBitrate,
				float startBitrate,
				float maxBitrate,
				float maxRtpQueueDelay,
				bool isAdaptiveTargetRateScale,
				float hysteresis,
				bool enableFrameSizeOverhead);

			float getMaxRate();

			float getTargetBitrate();

			void newMediaFrame(uint32_t time_ntp, int bytesRtp, bool isMarker);

			void updateRate(uint32_t time_ntp);

			void updateTargetBitrate(uint32_t time_ntp);

			bool isRtpQueueDiscard();

			bool isMatch(uint32_t ssrc_) { return ssrc == ssrc_; };

			bool isLossEpoch();

			void setRateHysteresis(float aValue) {
				hysteresis = aValue;
			}

			/*
			* Set the timestamp clock rate for this stream, default 90000Hz
			*/
			void setTimeStampClockRate(float aValue) {
				timeStampClockRate = aValue;
			}

			/*
			* Get the high percentile relative frame size
			*/
			float getRelFrameSizeHigh() {
				return relFrameSizeHigh;
			}

			ScreamV2Tx* parent;
			RtpQueueIface* rtpQueue;      // RTP Packet queue
			uint32_t ssrc;            // SSRC of stream
			bool isAdaptiveTargetRateScale;
			float hysteresis;
			float maxRtpQueueDelay;
			bool enableFrameSizeOverhead;
			float timeStampClockRate;

			int credit;             // Credit that is received if another stream gets
			//  priority to transmit
			int creditLost;         // Amount of lost (unused) credit, input to
			//  adjustPriorities function
			float targetPriority;   // Stream target priority
			float targetPriorityInv;// Stream target priority inverted
			int bytesTransmitted;   // Number of bytes transmitted
			int bytesAcked;         // Number of ACKed bytes
			int bytesLost;          // Number of lost bytes
			uint64_t packetLost;    // Number of lost packets
			uint64_t packetsCe;     // Number of Ce marked packets
			int bytesCe;            // Number of Ce marked bytes
			float rateTransmitted;  // Transmitted rate
			float rateTransmittedAvg;// Transmitted rate
			float rateAcked;        // ACKed rate
			float rateLost;         // Lost packets (bit)rate
			float rateCe;           // Ce marked packets (bit)rate
			uint16_t hiSeqAck;      // Highest sequence number ACKed
			uint16_t hiSeqTx;       // Highest sequence number transmitted
			float minBitrate;       // Min bitrate
			float maxBitrate;       // Max bitrate
			float targetBitrate;    // Target bitrate
			float targetBitrateH;   // Target bitrate (with hysteresis)
			uint32_t lastBitrateAdjustT_ntp; // Last time rate was updated for this stream
			uint32_t lastRateUpdateT_ntp;    // Last time rate estimate was updated
			uint32_t lastTargetBitrateIUpdateT_ntp;    // Last time rate estimate was updated

			uint32_t timeTxAck_ntp;  // timestamp when higest ACKed SN was transmitted
			uint32_t timeStampAckHigh; // Highest ACKed timestamp
			uint32_t lastTransmitT_ntp;

			int bytesRtp;           // Number of RTP bytes from media coder
			uint64_t packetsRtp;
			float rateRtp;          // Media bitrate
			float rateRtpAvg;
			float rateRtpHist[kRateUpDateSize];
			int rateUpdateHistPtr;
			float targetRateScale;
			uint32_t numberOfUpdateRate;

			bool isActive;
			uint32_t lastFrameT_ntp;
			uint32_t initTime_ntp;
			bool rtpQueueDiscard;
			uint32_t lastRtpQueueDiscardT_ntp;
			bool wasRepairLoss;
			bool repairLoss;
			uint32_t lastFullWindowT_ntp;

			Transmitted txPackets[kMaxTxPackets];
			int txPacketsPtr;
			bool lossEpoch;
			uint64_t cleared;

			int frameSize;
			int frameSizeAcc;
			int frameSizePrev;
			float frameSizeAvg;
			float framePeriod;

			float adaptivePacingRateScale;

			float relFrameSizeHist[kRelFrameSizeHistBins];
			float relFrameSizeHigh;
			int nFrames;

			float rateShare;
			bool isMaxrate;

			float rtpQueueDelay;
		};

		/*
		* Initialize values
		*/
		void initialize(uint32_t time_ntp);

		/*
		* Mark ACKed RTP packets
		* Return true if CE
		*/
		bool markAcked(uint32_t time_ntp,
			struct Transmitted* txPackets,
			uint16_t seqNr,
			uint32_t timestamp,
			Stream* stream,
			uint8_t ceBits,
			int& encCeMarkedBytes,
			bool isLast,
			bool& isMark);

		/*
		* Get total target bitrate for all streams
		*/
		float getTotalTargetBitrate();

		/*
		* Get total max bitrate for all streams
		*/
		float getTotalMaxBitrate();

		/*
		* Update CWND
		*/
		void updateCwnd(uint32_t time_ntp);

		/*
		* Detect lost RTP packets
		*/
		void detectLoss(uint32_t time_ntp, struct Transmitted* txPackets, uint16_t highestSeqNr, Stream* stream);

		/*
		* Call this function at regular intervals to determine active streams
		*/
		void determineActiveStreams(uint32_t time_ntp);

		/*
		* Estimate one way delay [jiffy] and updated base delay
		* Base delay is not subtracted
		*/
		void estimateOwd(uint32_t time_ntp);

		/*
		* return base delay [jiffy]
		*/
		uint32_t getBaseOwd();

		/*
		* Compute indicators of shared bottleneck
		*/
		void computeSbd();

		/*
		* True if competing (TCP)flows detected
		*/
		bool isCompetingFlows();

		/*
		* Get stream with corresponding SSRC
		*/
		Stream* getStream(uint32_t ssrc, int& streamId);

		/*
		* Get the prioritized stream
		*  Return NULL if no stream with
		*  with RTP packets
		*/
		Stream* getPrioritizedStream(uint32_t time_ntp);

		/*
		* Add credit to unserved streams
		*/
		void addCredit(uint32_t time_ntp,
			Stream* servedStream,
			int transmittedBytes);

		/*
		* Subtract used credit
		*/
		void subtractCredit(uint32_t time_ntp,
			Stream* servedStream,
			int transmittedBytes);

		/*
		* Get the fraction between queue delay and the queue delay target
		*/
		float getQueueDelayFraction();

		/*
		* Update cwndI
		*/
		void updateCwndI(int cwndI);

		/*
		* Get the mss
		*/
		int getMss();

		/*
		* Variables for network congestion control
		*/

		/*
		* Related to computation of queue delay and target queuing delay
		*/
		float lossBeta;
		float ecnCeBeta;
		float queueDelayTargetMin;
		float packetPacingHeadroom;
		float maxAdaptivePacingRateScale;
		float bytesInFlightHeadRoom;
		float multiplicativeIncreaseScalefactor;
		bool isL4s;
		float windowHeadroom;
		bool enableSbd;
		bool enableClockDriftCompensation;

		bool isEnablePacketPacing;
		bool isAutoTuneMinCwnd;
		bool enableRateUpdate;
		bool isUseExtraDetailedLog;
		bool isEnableRelaxedPacing;

		float sRtt;
		uint32_t sRtt_ntp;
		uint32_t sRttSh_ntp;
		uint32_t sRttShPrev_ntp;
		float currRtt;
		uint32_t ackedOwd;
		uint32_t baseOwd;
		bool canResetBaseDelayHistory;

		float queueDelay;
		float queueDelayFractionAvg;
		float queueDelayTarget;
		bool isEnableDelayBasedCongestionControl;

		float queueDelaySbdVar;
		float queueDelaySbdMean;
		float queueDelaySbdMeanSh;
		float queueDelaySbdSkew;

		float queueDelayAvg;
		float queueDelayMax;
		float queueDelayMin;
		float queueDelayMinAvg;

		int bytesNewlyAcked;
		int bytesNewlyAckedCe;
		int bytesNewlyAckedLog;
		int ecnCeMarkedBytesLog;

		int mss; // Maximum Segment Size
		int prevMss;

		int mssList[kMssListSize];
		int nMssListItems;
		int mssIndex;
		int minPacketsInFlight;

		int cwnd; // congestion window
		int cwndMin;
		int cwndMinLow;
		int cwndI; // congestion window inflexion point
		float cwndRatio;
		int cwndIHist[kCwndIHistSize];
		int cwndIHistIx;

		int bytesInFlight;
		int bytesInFlightLog;
		int prevBytesInFlight;
		int maxBytesInFlight;
		int maxBytesInFlightPrev;
		float bytesInFlightRatio;

		int bytesMarkedThisRtt;
		int bytesDeliveredThisRtt;
		int packetsMarkedThisRtt;
		int packetsDeliveredThisRtt;

		bool lossEvent;
		bool wasLossEvent;
		float lossEventRate;
		bool ecnCeEvent;
		bool virtualCeEvent;
		bool isCeThisFeedback;
		float fractionMarked;
		float lastFractionMarked;
		float l4sAlpha;
		float l4sAlphaLim;
		float ceDensity;
		float virtualL4sAlpha;
		float postCongestionScale;
		uint32_t reorderTime_ntp;
		float reorderTime;
		
		float rateTransmitted;
		float rateRtpAvg;
		float maxRate;
		float maxTotalBitrate;
		float rateTransmittedAvg;

		float relFrameSizeHigh;
		bool isNewFrame;

		uint32_t paceInterval_ntp;
		float paceInterval;
		float adaptivePacingRateScale;

		uint32_t baseOwdHist[kBaseOwdHistSize];
		uint32_t baseOwdHistMin;
		int baseOwdHistPtr;
		float queueDelayNormHist[kQueueDelayNormHistSize];
		int queueDelayNormHistPtr;
		int maxBytesInFlightHist[kMaxBytesInFlightHistSize];
		int maxBytesInFlightHistIx;

		uint32_t clockDriftCompensation;
		uint32_t clockDriftCompensationInc;

		Stream* streams[kMaxStreams];
		int nStreams;

		FILE* fp_log;
		FILE* fp_txrxlog;
		bool completeLogItem;
		char timeString[100];
		char detailedLogExtraData[256];

		bool isInitialized;
		uint32_t initTime_ntp;
		uint32_t lastCongestionDetectedT_ntp;
		uint32_t lastRttT_ntp;
		uint32_t lastMssUpdateT_ntp;
		uint32_t lastSRttUpdateT_ntp;
		uint32_t lastBaseOwdAddT_ntp;
		uint32_t baseOwdResetT_ntp;
		uint32_t lastSlowUpdateT_ntp;
		uint32_t lastLossEventT_ntp;
		uint32_t lastCeEventT_ntp;
		uint32_t lastTransmitT_ntp;
		uint32_t nextTransmitT_ntp;

		uint32_t lastRateUpdateT_ntp;
		uint32_t lastCwndIUpdateT_ntp;
		uint32_t lastCwndUpdateT_ntp;

		uint32_t lastQueueDelayAvgUpdateT_ntp;
		uint32_t lastL4sAlphaUpdateT_ntp;
		uint32_t lastBaseDelayRefreshT_ntp;
		uint32_t lastRateLimitT_ntp;
		uint32_t lastMssChange_ntp;


	};
}
#endif
