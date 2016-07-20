#ifndef SCREAM_TX
#define SCREAM_TX

#include <string.h>
#include <iostream>
#include <cstdint>
using namespace std;


// ==== Default parameters (if tuning necessary) ====
// Connection related default parameters 
// CWND scale factor upon loss event
static const float kLossBeta = 0.6f;
// Min and max queue delay target
static const float kQueueDelayTargetMin = 0.1f; //ms
// Enable shared botleneck detection and queue delay target adjustement
// good if SCReAM needs to compete with e.g FTP but 
// Can in some cases cause self-inflicted congestion
//  i.e the e2e delay can become large even though
//  there is no competing traffic present
static const bool kEnableSbd = true;

// Stream related default parameters 
// Max video rampup speed in bps/s (bits per second increase per second) 
static const float kRampUpSpeed = 200000.0f; // bps/s
// Max RTP queue delay, RTP queue is cleared if this value is exceeded
static const float kMaxRtpQueueDelay = 2.0;  // 2s
// Compensation factor for RTP queue size
// A higher value such as 0.2 gives less jitter esp. in wireless (LTE)
// but potentially also lower link utilization
static const float kTxQueueSizeFactor = 1.0f;
// Compensation factor for detected congestion in rate computation 
// A higher value such as 0.5 gives less jitter esp. in wireless (LTE)
// but potentially also lower link utilization
static const float kQueueDelayGuard = 0.3f;
// Video rate scaling due to loss events
static const float kLossEventRateScale = 0.8f;



// Constants
/*
* Timestamp sampling rate for SCReAM feedback
*/
static const float kTimestampRate = 1000.0f;
/* Max number of RTP packets in flight
* With and MSS = 1200 byte and an RTT = 200ms
* this is enount to support media bitrates of ~50Mbps
*/
static const int kMaxTxPackets = 1000;
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
static const int kBytesInFlightHistSize = 5;
static const int kRateRtpHistSize = 21;
static const int kRateUpDateSize = 4;

class RtpQueue;
class ScreamTx {
public:
	/*
	* Constructor, see constant definitions above for an explanation of parameters
	*/
	ScreamTx(float lossBeta = kLossBeta,
		float queueDelayTargetMin = kQueueDelayTargetMin,
		bool enableSbd = kEnableSbd);

	~ScreamTx();

	/*
	* Register a new stream {SSRC,PT} tuple,
	* with a priority value in the range [0.0..1.0]
	* where 1.0 denotes the highest priority
	* bitrates in bps
	* frame rate is the expected value for this stream
	* Constructor, see constant definitions above for an explanation of other default parameters
	*/
	void registerNewStream(RtpQueue *rtpQueue,
		uint32_t ssrc,
		float priority,
		float minBitrate,
		float maxBitrate,
		float rampUpSpeed = kRampUpSpeed,
		float maxRtpQueueDelay = kMaxRtpQueueDelay,
		float txQueueSizeFactor = kTxQueueSizeFactor,
		float queueDelayGuard = kQueueDelayGuard,
		float lossEventRateScale = kLossEventRateScale);

	/*
	* Call this function for each new video frame
	*  Note : isOkToTransmit should be called after newMediaFrame
	*/
	void newMediaFrame(uint64_t time_us, uint32_t ssrc, int bytesRtp);

	/*
	* Call this function at regular intervals to determine active streams
	*/
	void determineActiveStreams(uint64_t time_us);

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
	float isOkToTransmit(uint64_t time_us, uint32_t &ssrc);

	/*
	* Add packet to list of transmitted packets
	* should be called when an RTP packet transmitted
	* Return time until isOkToTransmit can be called again
	*/
	float addTransmitted(uint64_t timestamp_us, // Wall clock ts when packet is transmitted
		uint32_t ssrc,
		int size,
		uint16_t seqNr);

	/*
	* New incoming feedback, this function
	* triggers a CWND update
	* The SCReAM timestamp is in jiffies, where the frequency is controlled
	* by the timestamp clock frequency (default 1000Hz)
	* The ackVector indicates recption of the 64 RTP SN prior to highestSeqNr
	*  Note : isOkToTransmit should be called after incomingFeedback
	*/
	void incomingFeedback(uint64_t time_us,
		uint32_t ssrc,         // SSRC of stream
		uint32_t timestamp,    // SCReAM FB timestamp [jiffy]
		uint16_t highestSeqNr, // Highest ACKed RTP sequence number
		uint64_t  ackVector,   // ACK vector
		bool qBit);       // Source quench bit

	/*
	* Get the target bitrate for stream with SSRC
	* NOTE!, Because SCReAM operates on RTP packets, the target bitrate will
	*  also include the RTP overhead. This means that a subsequent call to set the
	*  media coder target bitrate must subtract an estimate of the RTP + framing
	*  overhead. This is not critical for Video bitrates but can be important
	*  when SCReAM is used to congestion control e.g low bitrate audio streams
	*/
	float getTargetBitrate(uint32_t ssrc);

	/*
	* Print logs
	*/
	void printLog(float time);

private:
	/*
	* Struct for list of RTP packets in flight
	*/
	struct Transmitted {
		uint64_t timeTx_us;
		uint32_t timestamp;
		uint32_t ssrc;
		int size;
		uint16_t seqNr;
		bool isUsed;
		bool isAcked;
		bool isAfterReceivedEdge;

	};

	/*
	* One instance is created for each {SSRC,PT} tuple
	*/
	class Stream {
	public:
		Stream(ScreamTx *parent,
			RtpQueue *rtpQueue,
			uint32_t ssrc,
			float priority,
			float minBitrate,
			float maxBitrate,
			float rampUpSpeed,
			float maxRtpQueueDelay,
			float txQueueSizeFactor,
			float queueDelayGuard,
			float lossEventRateScale);

		float getMaxRate();

		void updateRate(uint64_t time_us);

		void updateTargetBitrate(uint64_t time_us);

		bool isMatch(uint32_t ssrc_) { return ssrc == ssrc_; };
		ScreamTx *parent;
		RtpQueue *rtpQueue;      // RTP Packet queue
		uint32_t ssrc;            // SSRC of stream
		float rampUpSpeed;
		float maxRtpQueueDelay;
		float txQueueSizeFactor;
		float queueDelayGuard;
		float lossEventRateScale;

		float credit;           // Credit that is received if another stream gets priority 
		                        //  to transmit
		float targetPriority;   // Stream target priority
		int bytesTransmitted;   // Number of bytes transmitted
		int bytesAcked;         // Number of ACKed bytes
		float rateTransmitted;  // Transmitted rate
		float rateAcked;        // ACKed rate
		float minBitrate;       // Min bitrate
		float maxBitrate;       // Max bitrate
		float targetBitrate;    // Target bitrate
		float targetBitrateI;   // Target bitrate inflection point
		bool wasFastStart;      // Was fast start
		bool lossEventFlag;     // Was loss event
		float txSizeBitsAvg;    // Avergage nymber of bits in RTP queue
		uint64_t lastBitrateAdjustT_us; // Last time rate was updated for this stream
		uint64_t lastRateUpdateT_us;    // Last time rate estimate was updated
		uint64_t lastTargetBitrateIUpdateT_us;    // Last time rate estimate was updated

		uint64_t timeTxAck_us;      // timestamp when higest ACKed SN was transmitted

		int bytesRtp;           // Number of RTP bytes from media coder
		float rateRtp;          // Media bitrate
		float rateRtpSum;       // Temp accumulated rtpRate
		int rateRtpSumN;        //
		float rateRtpHist[kRateRtpHistSize]; // History of media coder bitrates
		int rateRtpHistPtr;     // Ptr to above
		float rateRtpMedian;    // Median media bitrate
		float rateRtpHistSh[kRateUpDateSize];
		float rateAckedHist[kRateUpDateSize];
		float rateTransmittedHist[kRateUpDateSize];
		int rateUpdateHistPtr;

		bool isActive;
		uint64_t lastFrameT_us;
		uint64_t initTime_us;

	};

	/*
	* Initialize values
	*
	*/
	void initialize(uint64_t time_us);

	/*
	* Compute 1st order prediction coefficient of queue delay multiplied by the queue delay fraction
	* A value [0.0..1.0] indicates if queue delay is increasing
	* This gives a rough estimate of how the queuing delay delay evolves
	*/
	void computeQueueDelayTrend();

	/*
	* Update CWND
	*/
	void updateCwnd(uint64_t time_us);

	/*
	* Estimate one way delay [jiffy] and updated base delay
	* Base delay is not subtracted
	*/
	uint32_t estimateOwd(uint64_t time_us);

	/*
	* return base delay [jiffy]
	*/
	uint32_t getBaseOwd();

	/*
	* Compute indicators of shared bottleneck
	*/
	void computeSbd();

	/*
	* TRUE if competing (TCP)flows detected
	*/
	bool isCompetingFlows();

	Stream* getStream(uint32_t ssrc);

	/*
	* Get matching stream index for this SSRC tuple,
	*  return -1 if no match
	*/
	int getStreamIndex(uint32_t ssrc);

	void adjustPriorities(uint64_t time_us);

	/*
	* Get the prioritized stream
	*  Return NULL if no stream with
	*  with RTP packets
	*/
	Stream* getPrioritizedStream(uint64_t time_us);

	/*
	* Add credit to unserved streams
	*/
	void addCredit(uint64_t time_us,
		Stream* servedStream,
		int transmittedBytes);

	/*
	* Subtract used credit
	*/
	void subtractCredit(uint64_t time_us,
		Stream* servedStream,
		int transmittedBytes);

	/*
	* Variables for multiple steams handling
	*/
	Stream *streams[kMaxStreams];
	int nStreams;

	/*
	* Variables for network congestion control
	*/
	/*
	* Related to computation of queue delay and target queuing delay
	*/

	float lossBeta;
	float queueDelayTargetMin;
	bool enableSbd;

	Transmitted txPackets[kMaxTxPackets];
	uint64_t sRttSh_us;
	uint64_t sRtt_us;
	uint32_t ackedOwd;
	uint32_t baseOwd;

	uint32_t baseOwdHist[kBaseOwdHistSize];
	int baseOwdHistPtr;
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

	/*
	* CWND management
	*/
	int bytesNewlyAcked;
	int mss; // Maximum Segment Size
	int cwnd; // congestion window
	int cwndMin;
	int bytesInFlightHistLo[kBytesInFlightHistSize];
	int bytesInFlightHistHi[kBytesInFlightHistSize];
	int bytesInFlightHistPtr;
	int bytesInFlightMaxLo;
	int bytesInFlightMaxHi;
	int accBytesInFlightMax;
	int nAccBytesInFlightMax;
	float rateTransmitted;
	float queueDelayTrendMem;


	/*
	* Loss event
	*/
	bool lossEvent;
	bool wasLossEvent;
	float lossEventRate;

	/*
	* Fast start
	*/
	bool inFastStart;

	/*
	* Transmission scheduling
	*/
	float pacingBitrate;

	/*
	* Update control variables
	*/
	bool isInitialized;
	uint64_t lastSRttUpdateT_us;
	uint64_t lastBaseOwdAddT_us;
	uint64_t baseOwdResetT_us;
	uint64_t lastAddToQueueDelayFractionHistT_us;
	uint64_t lastBytesInFlightT_us;
	uint64_t lastCongestionDetectedT_us;
	uint64_t lastLossEventT_us;
	uint64_t lastTransmitT_us;
	uint64_t nextTransmitT_us;
	uint64_t lastRateUpdateT_us;
	uint64_t lastAdjustPrioritiesT_us;
	uint64_t lastRttT_us;

	/*
	* Methods
	*/
	/*
	* return 1 if in fast start
	*/
	int isInFastStart() { return inFastStart ? 1 : 0; };

	/*
	* Get smoothed RTT
	*/
	float getSRtt() { return sRtt_us / 1e6f; };

	/*
	* Returns the maximum bytes in flight value of the last
	*  kBytesInFlightHistSize values.
	* Used for congestion window validation.
	*/
	int getMaxBytesInFlightHi();
	int getMaxBytesInFlightLo();

	/*
	* Returns the number of bytes currently in flight.
	*/
	int bytesInFlight();

	/*
	* Get pacing bitrate
	*/
	float getPacingBitrate() { return pacingBitrate; };

	/*
	* Get the fraction between queue delay and the queue delay target
	*/
	float getQueueDelayFraction();

	/*
	* Get the queuing delay trend
	*/
	float getQueueDelayTrend();
};
#endif