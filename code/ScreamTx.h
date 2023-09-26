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
	using namespace std;

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
	static const float kLossBeta = 0.8f;
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

	static const float ntp2SecScaleFactor = 1.0f / 65536;

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
	static const int kRateUpDateSize = 8;
	static const int kTargetBitrateHistSize = 3;
	static const int kLossRateHistSize = 10;
	static const int kSrttHistBins = 200;
	static const int kRelFrameSizeHistBins = 20;

	static const int kMaxBytesInFlightHistSize = 10;


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
			Statistics(ScreamTx *parent);
			void getSummary(float time, char s[]);
			void add(float rateTx, float rateLost, float rtt, float queueDelay);
		private:
			float lossRateHist[kLossRateHistSize];
			float rateLostAcc;
			int rateLostN;
			int lossRateHistPtr;
			float avgRateTx;
			float avgRtt;
			float avgQueueDelay;
			float sumRateTx;
			float sumRateLost;
			ScreamTx *parent;
		};
		Statistics* statistics;

		/*
		* Get overall simplified statistics
		*/
		void getStatistics(float time, char* s);

		void setLogTag(char* logTag_) {
			logTag = logTag_;
		};
		const char* logTag = "";
	};

	class RtpQueueIface;
	class ScreamV1Tx : public ScreamTx {
	public:
		/*
		* Constructor
		* lossBeta sets how the congestion window should be scaled when packet loss is detected
		*  lossBeta = 1.0 means that packet losses are ignored by the congestion control
		* ecnCeBeta sets how the congestion window should be scaled when ECN-CE is detected
		*  this applies only to classic ECN
		* queueDelayTargetMin sets the queue delay targer
		* enableSbd increases the delay target if competing buffer building flows are detected
		* cwnd > 0 sets a different initial congestion window, for example it can be set to
		*  initialrate/8*rtt
		* packetPacingHeadroom sets how much faster the video frames are transmitted, relative to the target bitrate
		* bytesInFlightHistSize can be set to a larger value than 5(s) for enhanced robustness to media coders that 
		*  are idle for long periods
		* isL4s = true changes congestion window reaction to ECN marking to a scalable function, similar to DCTCP
		* openWindow = true removes the congestion window limitation, this feature may be useful in combination with L4S
		* enableClockDriftCompensation = true compensates for the case where the endpoints clocks drift relative to one 
		*  another. Note though that clock drift is not always montonous IRL
		* maxAdaptivePacingRateScale > 1.0 enables adaptive scaling of the pacing rate to transmit large frames faster. 
		*  This feature is mostly relevant with L4S and has the potential to reduce unecessary queue build-up in the 
		*  RTP queue, but can also potentially increase network queue build-up 
		* isNewCc == true enables a new algorithm that is more stable when subject to competing L4S flows 
		*  in the same boottleneck queue
		* 
		*/
		ScreamV1Tx(float lossBeta = kLossBeta,
			float ecnCeBeta = kEcnCeBeta,
			float queueDelayTargetMin = kQueueDelayTargetMin,
			bool enableSbd = kEnableSbd,
			float gainUp = kGainUp,
			float gainDown = kGainDown,
			int cwnd = 0,  // An initial cwnd larger than 2*mss
			float packetPacingHeadroom = kPacketPacingHeadRoom,
			int bytesInFlightHistSize = 30,
			bool isL4s = false,
			bool openWindow = false,
			bool enableClockDriftCompensation = false,
			float maxAdaptivePacingRateScale = 1.0,
			bool isNewCc = false); 

		~ScreamV1Tx();

		/*
		* Register a new stream {SSRC,PT} tuple,
		*  with a priority value in the range ]0.0..1.0]
		*  where 1.0 denotes the highest priority.
		* It is recommended that at least one stream has prioritity 1.0.
		* Bitrates are specified in bps
		* rampUpSpeed sets how fast the bitrate can be increased [bps/s]
		* rampUpScale sets how fast the bitrate can be increased relative to the current target bitrate
		* maxRtpQueueDelay sets how long RTP packets can be held in buffer before entire buffer is cleared
		* txQueueSizeFactor and queueDelayGuard sets the sensitivity to delay related congestion 
		* lossEventRateScale sets how the congestion window should be scaled when packet loss is detected
		* ecnCeEventRateScale sets how the congestion window should be scaled when ECN CE is detected
		*  this applies only to classic ECN
		* isAdaptiveTargetRateScale compensates for deviations from target bitrates
		* hysteresis sets how much the target rate should change for the getTargetBitrate() function to 
		*  return a changed value. hysteresis = 0.1 sets a +10%/-5% hysteresis. 
		*  This can benefit video encoders that become confused by too frequent rate updates
		*/
		void registerNewStream(RtpQueueIface *rtpQueue,
			uint32_t ssrc,
			float priority,     // priority in range ]0.0 .. 1.0], 1.0 is highest
			float minBitrate,   // Min target bitrate
			float startBitrate, // Starting bitrate
			float maxBitrate,   // Max target bitrate
			float rampUpSpeed = kRampUpSpeed,
			float rampUpScale = kRampUpScale,
			float maxRtpQueueDelay = kMaxRtpQueueDelay,
			float txQueueSizeFactor = kTxQueueSizeFactor,
			float queueDelayGuard = kQueueDelayGuard,
			float lossEventRateScale = kLossEventRateScale,
			float ecnCeEventRateScale = kEcnCeEventRateScale,
			bool isAdaptiveTargetRateScale = true,
		    float hysteresis = 0.0);

		/*
		 * Updates the min and max bitrates for an existing stream
		 */
		void updateBitrateStream(uint32_t ssrc,
			float minBitrate,
			float maxBitrate);

		/*
		 * Access the configured RtpQueue of an existing stream
		 */
		RtpQueueIface * getStreamQueue(uint32_t ssrc);

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
		float isOkToTransmit(uint32_t time_ntp, uint32_t &ssrc);

		/*
		* Add packet to list of transmitted packets
		* should be called when an RTP packet transmitted
		* Return time until isOkToTransmit can be called again
		*/
		float addTransmitted(uint32_t timestamp_ntp, // Wall clock ts when packet is transmitted
			uint32_t ssrc,
			int size,
			uint16_t seqNr,
			bool isMark);

		/* New incoming feedback, this function
		 * triggers a CWND update
		 * The SCReAM timestamp is in jiffies, where the frequency is controlled
		 * by the timestamp clock frequency(default 1000Hz)
		 * The ackVector indicates recption of the 64 RTP SN prior to highestSeqNr
		 *  Note : isOkToTransmit should be called after incomingFeedback
		 /*
		 /* Parse standardized feedback according to
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
		float getTargetBitrate(uint32_t ssrc);

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
		void getLog(float time, char *s, uint32_t ssrc, bool clear);

        /*
		* Get verbose log header
		*/
		void getLogHeader(char *s);

		/*
		* Get verbose log information
		*/
		void getShortLog(float time, char *s);

		/*
		* Get verbose log information
		*/
		void getVeryShortLog(float time, char *s);

		/*
		* Set file pointer for detailed per-ACK log
		*/
		void setDetailedLogFp(FILE *fp) {
			fp_log = fp;
		}

		void setTimeString(char *s) {
			strcpy(timeString, s);
		}

		/*
		* extra data to be appended to detailed log
		*/
		void setDetailedLogExtraData(char *s) {
			strcpy(detailedLogExtraData, s);
		}

		/*
		* Get the list of log items
		*/
		const char *getDetailedLogItemList() {
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
		 * Set the post congestion delay i.e how fast SCReAM should attempt
		 *  to rampup again after congestion, this complements the rampup speed
		 *  and does something simlar
		 */
		void setPostCongestionDelay(float a) {
			postCongestionDelay = a;
		}

		/*
		* The fastIncreaseFactor replaces rampUpSpeed and rampUpScale when 
		*  isNewCc = true
		* 1.0 is typicall OK but can be set lower if a more cautios ramp up 
		*  is desired.
		*/
		void setFastIncreaseFactor(float a) {
			fastIncreaseFactor = a;
		}

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



	private:
		/*
		* Struct for list of RTP packets in flight
		*/
		struct Transmitted {
			uint32_t timeTx_ntp;
			int size;
			uint16_t seqNr;
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
		 */
    class Stream {
		public:
			Stream(ScreamV1Tx *parent,
				RtpQueueIface *rtpQueue,
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
			  float hysteresis);

			float getMaxRate();

			float getTargetBitrate();

			void newMediaFrame(uint32_t time_ntp, int bytesRtp, bool isMarker);

			void updateRate(uint32_t time_ntp);

			void updateTargetBitrateI(float br);

			void updateTargetBitrate(uint32_t time_ntp);
			void updateTargetBitrateNew(uint32_t time_ntp);
			void updateTargetBitrateOld(uint32_t time_ntp);

			bool isRtpQueueDiscard();

			bool isMatch(uint32_t ssrc_) { return ssrc == ssrc_; };

			bool isLossEpoch();

			void setRateHysteresis(float aValue) {
				hysteresis = aValue;
			}


			ScreamV1Tx *parent;
			RtpQueueIface *rtpQueue;      // RTP Packet queue
			uint32_t ssrc;            // SSRC of stream
			float rampUpSpeed;
			float rampUpScale;
			float maxRtpQueueDelay;
			float txQueueSizeFactor;
			float queueDelayGuard;
			float lossEventRateScale;
			float ecnCeEventRateScale;
			bool isAdaptiveTargetRateScale;
			float hysteresis;

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
			float rateAcked;        // ACKed rate
			float rateLost;         // Lost packets (bit)rate
			float rateCe;           // Ce marked packets (bit)rate
			float rateTransmittedLog;  
			float rateAckedLog;        
			float rateLostLog;         
			float rateCeLog;           
			uint16_t hiSeqAck;      // Highest sequence number ACKed
			uint16_t hiSeqTx;       // Highest sequence number transmitted
			float minBitrate;       // Min bitrate
			float maxBitrate;       // Max bitrate
			float targetBitrate;    // Target bitrate
			float targetBitrateI;   // Target bitrate inflection point
			float targetBitrateH;   // Target bitrate (with hysteresis)
			bool wasFastStart;      // Was fast start
			bool lossEventFlag;     // Was loss event
			bool ecnCeEventFlag;    // Was ECN mark event
			float txSizeBitsAvg;    // Avergage nymber of bits in RTP queue
			uint32_t lastBitrateAdjustT_ntp; // Last time rate was updated for this stream
			uint32_t lastRateUpdateT_ntp;    // Last time rate estimate was updated
			uint32_t lastTargetBitrateIUpdateT_ntp;    // Last time rate estimate was updated

			uint32_t timeTxAck_ntp;  // timestamp when higest ACKed SN was transmitted
			uint32_t lastTransmitT_ntp;

			int bytesRtp;           // Number of RTP bytes from media coder
            uint64_t packetsRtp;
			float rateRtp;          // Media bitrate
			float rateRtpLog;
			float rateRtpHist[kRateUpDateSize];
			float rateAckedHist[kRateUpDateSize];
			float rateLostHist[kRateUpDateSize];
			float rateCeHist[kRateUpDateSize];
			float rateTransmittedHist[kRateUpDateSize];
			int rateUpdateHistPtr;
			float targetBitrateHist[kTargetBitrateHistSize];
			int targetBitrateHistPtr;
			uint32_t targetBitrateHistUpdateT_ntp;
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
			float frameSizeAvg;
			float adaptivePacingRateScale;
			float l4sOverShootScale;
			float framePeriod;

			float rtpQOverShoot;
			uint32_t lastTargetBitrateHUpdateT_ntp;

			float relFrameSizeHist[kRelFrameSizeHistBins];
			float relFrameSizeHigh;

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
			struct Transmitted *txPackets,
			uint16_t seqNr,
			uint32_t timestamp,
			Stream *stream,
			uint8_t ceBits,
			int &encCeMarkedBytes,
			bool isLast,
			bool &isMark);

		/*
		* Get total target bitrate for all streams
		*/
		float getTotalTargetBitrate();

		/*
		* Update CWND
		*/
		void updateCwnd(uint32_t time_ntp);

		/*
		* Detect lost RTP packets
		*/
		void detectLoss(uint32_t time_ntp, struct Transmitted *txPackets, uint16_t highestSeqNr, Stream *stream);

		/*
		* Call this function at regular intervals to determine active streams
		*/
		void determineActiveStreams(uint32_t time_ntp);

		/*
		* Compute 1st order prediction coefficient of queue delay multiplied by the queue delay fraction
		* A value [0.0..1.0] indicates if queue delay is increasing
		* This gives a rough estimate of how the queuing delay delay evolves
		*/
		void computeQueueDelayTrend();

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
		Stream* getStream(uint32_t ssrc, int &streamId);

		/*
		* Adjust stream bitrates to reflect priorities
		*/
		void adjustPriorities(uint32_t time_ntp);

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
		* return 1 if in fast start
		*/
		int isInFastStart() { return inFastStart ? 1 : 0; };

		/*
		* Get the fraction between queue delay and the queue delay target
		*/
		float getQueueDelayFraction();

		/*
		* Get the queuing delay trend
		*/
		float getQueueDelayTrend();


		/*
		* Variables for network congestion control
		*/

		/*
		* Related to computation of queue delay and target queuing delay
		*/
		float lossBeta;
		float ecnCeBeta;
		float queueDelayTargetMin;
		bool enableSbd;
		float gainUp;
		float gainDown;
		float packetPacingHeadroom;
		bool isNewCc;
		float fastIncreaseFactor;
		bool isEnablePacketPacing;

		uint32_t sRttSh_ntp;
		uint32_t sRtt_ntp;
		float sRtt;
		uint32_t ackedOwd;
		uint32_t baseOwd;

		uint32_t baseOwdHist[kBaseOwdHistSize];
		int baseOwdHistPtr;
		uint32_t baseOwdHistMin;
		uint32_t clockDriftCompensation;
		uint32_t clockDriftCompensationInc;

		float queueDelay;
		float queueDelayFractionAvg;
		float queueDelayFractionHist[kQueueDelayFractionHistSize];
		int queueDelayFractionHistPtr;
		float queueDelayTrend;
		float queueDelayTarget;
		float queueDelayNormHist[kQueueDelayNormHistSize];
		int queueDelayNormHistPtr;
		float queueDelaySbdVar;
		float queueDelaySbdMean;
		float queueDelaySbdSkew;
		float queueDelaySbdMeanSh;
		float queueDelayMax;

		/*
		* CWND management
		*/
		int bytesNewlyAcked;
		int mss; // Maximum Segment Size
		int cwnd; // congestion window
		int cwndI; // congestion window inflexion point
		int cwndMin;
		int cwndMinLow;
		bool openWindow;
		bool enableClockDriftCompensation;
		int bytesInFlight;
		int bytesInFlightLog;
		int bytesInFlightHistLo[kBytesInFlightHistSizeMax];
		int bytesInFlightHistHi[kBytesInFlightHistSizeMax];
		int bytesInFlightHistSize;
		int bytesInFlightHistPtr;
		int bytesInFlightMaxLo;
		int bytesInFlightHistLoMem;
		int bytesInFlightMaxHi;
		int bytesInFlightHistHiMem;
		float maxBytesInFlight;
		int accBytesInFlightMax;
		int nAccBytesInFlightMax;
		float rateTransmitted;
		float rateAcked;
		float rateRtp;
		float rateTransmittedLog;
		float rateAckedLog;
		float rateRtpLog;
		float queueDelayTrendMem;
		float maxRate;
		uint32_t lastCwndUpdateT_ntp;
		bool isL4s;
		float l4sAlpha;
		int bytesMarkedThisRtt;
		int bytesDeliveredThisRtt;
		uint32_t lastL4sAlphaUpdateT_ntp;
		float maxTotalBitrate;
		float postCongestionScale;
		float postCongestionDelay;
		float bytesInFlightRatio;
		uint32_t lastCwndIUpdateT_ntp;
		bool isAutoTuneMinCwnd;

		/*
		* Loss event
		*/
		bool lossEvent;
		bool wasLossEvent;
		float lossEventRate;

		/*
		* ECN-CE
		*/
		bool ecnCeEvent;
		bool isCeThisFeedback;
		bool isL4sActive;
		uint32_t lastCeEventT_ntp;
		float fractionMarked;
		float ceDensity;



		/*
		* Fast start
		*/
		bool inFastStart;

		/*
		* Transmission scheduling
		*/
		uint32_t paceInterval_ntp;
		float paceInterval;
		float rateTransmittedAvg;
		float maxAdaptivePacingRateScale;
		float adaptivePacingRateScale;

		/*
		* Update control variables
		*/
		bool isInitialized;
		uint32_t lastSRttUpdateT_ntp;
		uint32_t lastBaseOwdAddT_ntp;
		uint32_t baseOwdResetT_ntp;
		uint32_t lastAddToQueueDelayFractionHistT_ntp;
		uint32_t lastBytesInFlightT_ntp;
		uint32_t lastCongestionDetectedT_ntp;
		uint32_t lastLossEventT_ntp;
		uint32_t lastTransmitT_ntp;
		uint32_t nextTransmitT_ntp;
		uint32_t lastRateUpdateT_ntp;
		uint32_t lastAdjustPrioritiesT_ntp;
		uint32_t lastRttT_ntp;
		uint32_t lastBaseDelayRefreshT_ntp;
		uint32_t initTime_ntp;
		float queueDelayMin;
		float queueDelayMinAvg;
		bool enableRateUpdate;

		/*
		* SSRT histogram
		*/
		float sRttHist[kSrttHistBins];
		float sRttLow;
		float sRttHigh;

		float cwndRatio = 0.001f;
		float cwndRatioScale = 1.0f;

		/*
		* Variables for multiple steams handling
		*/
		Stream *streams[kMaxStreams];
		int nStreams;
		bool isNewFrame; // True if new frame received

		/*
		* Statistics
		*/
		char detailedLogExtraData[256];

		/*
		*
		*/
		FILE *fp_log;
		bool completeLogItem;
		char timeString[100];
		bool isUseExtraDetailedLog;
		int bytesNewlyAckedLog;
		int ecnCeMarkedBytesLog;
	};
	class ScreamV2Tx : public ScreamTx{
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
		* openWindow = true removes the congestion window limitation, this feature may be useful in combination with L4S
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
			bool openWindow = false,
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
		*  This can benefit video encoders that become confused by too frequent rate updates
		*/
		void registerNewStream(RtpQueueIface* rtpQueue,
			uint32_t ssrc,
			float priority,     // priority in range ]0.0 .. 1.0], 1.0 is highest
			float minBitrate,   // Min target bitrate
			float startBitrate, // Starting bitrate
			float maxBitrate,   // Max target bitrate
			float maxRtpQueueDelay = kMaxRtpQueueDelay,
			bool isAdaptiveTargetRateScale = true,
			float hysteresis = 0.0);

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
			bool isMark);

		/* New incoming feedback, this function
		 * triggers a CWND update
		 * The SCReAM timestamp is in jiffies, where the frequency is controlled
		 * by the timestamp clock frequency(default 1000Hz)
		 * The ackVector indicates recption of the 64 RTP SN prior to highestSeqNr
		 *  Note : isOkToTransmit should be called after incomingFeedback
		 /*
		 /* Parse standardized feedback according to
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
		float getTargetBitrate(uint32_t ssrc);

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
		 * Set the post congestion delay i.e how fast SCReAM should attempt
		 *  to rampup again after congestion, this complements the rampup speed
		 *  and does something simlar
		 */
		void setPostCongestionDelay(float a) {
			postCongestionDelay = a;
		}

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

	private:
		/*
		* Struct for list of RTP packets in flight
		*/
		struct Transmitted {
			uint32_t timeTx_ntp;
			int size;
			uint16_t seqNr;
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
				float hysteresis);

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


			ScreamV2Tx* parent;
			RtpQueueIface* rtpQueue;      // RTP Packet queue
			uint32_t ssrc;            // SSRC of stream
			bool isAdaptiveTargetRateScale;
			float hysteresis;
			float maxRtpQueueDelay;

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
			float rateAcked;        // ACKed rate
			float rateLost;         // Lost packets (bit)rate
			float rateCe;           // Ce marked packets (bit)rate
			float rateTransmittedLog;
			float rateAckedLog;
			float rateLostLog;
			float rateCeLog;
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
			uint32_t lastTransmitT_ntp;

			int bytesRtp;           // Number of RTP bytes from media coder
			uint64_t packetsRtp;
			float rateRtp;          // Media bitrate
			float rateRtpLog;
			float rateRtpHist[kRateUpDateSize];
			float rateAckedHist[kRateUpDateSize];
			float rateLostHist[kRateUpDateSize];
			float rateCeHist[kRateUpDateSize];
			float rateTransmittedHist[kRateUpDateSize];
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
			float frameSizeAvg;
			float adaptivePacingRateScale;
			float framePeriod;
			float relFrameSizeHist[kRelFrameSizeHistBins];
			float relFrameSizeHigh;
			uint32_t nFrames;

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
		* Compute 1st order prediction coefficient of queue delay multiplied by the queue delay fraction
		* A value [0.0..1.0] indicates if queue delay is increasing
		* This gives a rough estimate of how the queuing delay delay evolves
		*/
		void computeQueueDelayTrend();

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
		* Get the queuing delay trend
		*/
		float getQueueDelayTrend();

		/*
		* Variables for network congestion control
		*/

		/*
		* Related to computation of queue delay and target queuing delay
		*/
		float lossBeta;
		float ecnCeBeta;
		float queueDelayTargetMin;
		bool enableSbd;
		float packetPacingHeadroom;
		float bytesInFlightHeadRoom;
		bool isEnablePacketPacing;
		float multiplicativeIncreaseScalefactor;

		uint32_t sRttSh_ntp;
		uint32_t sRtt_ntp;
		float sRtt;
		uint32_t ackedOwd;
		uint32_t baseOwd;

		uint32_t baseOwdHist[kBaseOwdHistSize];
		int baseOwdHistPtr;
		uint32_t baseOwdHistMin;
		uint32_t clockDriftCompensation;
		uint32_t clockDriftCompensationInc;

		float queueDelay;
		float queueDelayFractionAvg;
		float queueDelayFractionHist[kQueueDelayFractionHistSize];
		int queueDelayFractionHistPtr;
		float queueDelayTrend;
		float queueDelayTarget;
		float queueDelayNormHist[kQueueDelayNormHistSize];
		int queueDelayNormHistPtr;
		float queueDelaySbdVar;
		float queueDelaySbdMean;
		float queueDelaySbdSkew;
		float queueDelaySbdMeanSh;
		float queueDelayMax;

		/*
		* CWND management
		*/
		int bytesNewlyAcked;
		int mss; // Maximum Segment Size
		int cwnd; // congestion window
		int cwndMin;
		int cwndMinLow;
		bool openWindow;
		bool enableClockDriftCompensation;
		int bytesInFlight;
		int bytesInFlightLog;
		float rateTransmitted;
		float rateAcked;
		float rateRtp;
		float rateTransmittedLog;
		float rateAckedLog;
		float rateRtpLog;
		float queueDelayTrendMem;
		float maxRate;
		uint32_t lastCwndUpdateT_ntp;
		bool isL4s;
		float l4sAlpha;
		int bytesMarkedThisRtt;
		int bytesDeliveredThisRtt;
		int packetsMarkedThisRtt;
		int packetsDeliveredThisRtt;
		uint32_t lastL4sAlphaUpdateT_ntp;
		float maxTotalBitrate;
		float postCongestionScale;
		float postCongestionDelay;
		float bytesInFlightRatio;
		bool isAutoTuneMinCwnd;
		int prevBytesInFlight;
		int maxBytesInFlight;
		int maxBytesInFlightPrev;

		int maxBytesInFlightHist[kMaxBytesInFlightHistSize];
		int maxBytesInFlightHistIx = 0;

		int cwndI; // congestion window inflexion point
		uint32_t lastCwndIUpdateT_ntp;

		/*
		* Loss event
		*/
		bool lossEvent;
		bool wasLossEvent;
		float lossEventRate;

		/*
		* ECN-CE
		*/
		bool ecnCeEvent;
		bool isCeThisFeedback;
		bool isL4sActive;
		uint32_t lastCeEventT_ntp;
		float fractionMarked;
		float ceDensity;
		int bytesNewlyAckedCe;

		/*
		* Transmission scheduling
		*/
		uint32_t paceInterval_ntp;
		float paceInterval;
		float rateTransmittedAvg;
		float maxAdaptivePacingRateScale;
		float adaptivePacingRateScale;

		/*
		* Update control variables
		*/
		bool isInitialized;
		uint32_t lastSRttUpdateT_ntp;
		uint32_t lastBaseOwdAddT_ntp;
		uint32_t baseOwdResetT_ntp;
		uint32_t lastAddToQueueDelayFractionHistT_ntp;
		uint32_t lastMssUpdateT_ntp;
		uint32_t lastCongestionDetectedT_ntp;
		uint32_t lastLossEventT_ntp;
		uint32_t lastTransmitT_ntp;
		uint32_t nextTransmitT_ntp;
		uint32_t lastRateUpdateT_ntp;
		uint32_t lastRttT_ntp;
		uint32_t lastBaseDelayRefreshT_ntp;
		uint32_t initTime_ntp;
		float queueDelayMin;
		float queueDelayMinAvg;
		bool enableRateUpdate;

		float cwndRatio;

		/*
		* Variables for multiple steams handling
		*/
		Stream* streams[kMaxStreams];
		int nStreams;
		bool isNewFrame; // True if new frame received

		/*
		* Statistics
		*/
		char detailedLogExtraData[256];

		/*
		*
		*/
		FILE* fp_log;
		bool completeLogItem;
		char timeString[100];
		bool isUseExtraDetailedLog;
		int bytesNewlyAckedLog;
		int ecnCeMarkedBytesLog;
	};
}
#endif
