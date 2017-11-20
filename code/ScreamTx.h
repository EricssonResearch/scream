#ifndef SCREAM_TX
#define SCREAM_TX

#include <string.h>
#include <iostream>
#include <cstdint>
using namespace std;

/*
* This module implements the sender side of SCReAM,
*  see https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pdf
*  for details on how it is integrated in audio/video platforms
* A full implementation needs the additional code for
*  + RTCP feedback (e.g using RFC3611 XR elements)
*  + RTP queue(s), one queue per stream, see SCReAM description for interface description
*  + Other obvious stuff such as RTP payload packetizer, video+audio capture, coders....
*
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
// Max RTP queue delay, RTP queue is cleared if this value is exceeded
static const float kMaxRtpQueueDelay = 0.1;  // 0.1s
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



// Constants
/*
* Timestamp sampling rate for SCReAM feedback
*/
static const float kTimestampRate = 1000.0f;
/*
* Max number of RTP packets in flight
* With and MSS = 1200 byte and an RTT = 200ms
* this is enount to support media bitrates of ~50Mbps
* Note, 65536 % kMaxTxPackets must be zero
*/
static const int kMaxTxPackets = 2048;
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

class RtpQueueIface;
class ScreamTx {
public:
    /*
    * Constructor, see constant definitions above for an explanation of parameters
    * cwnd > 0 sets a different initial congestion window, for example it can be set to
    *  initialrate/8*rtt
    * cautiousPacing is set in the range [0.0..1.0]. A higher values restricts the transmission rate of large key frames
    *  which can be beneficial if it is evident that large key frames cause packet drops, for instance due to
    *  reduced buffer size in wireless modems.
    *  This is however at the potential cost of an overall increased transmission delay also when links are uncongested
    *  as the RTP packets are more likely to be buffered up on the sender side when cautiousPacing is set close to 1.0.
    * lossBeta == 1.0 means that packet losses are ignored by the congestion control
    * bytesInFlightHistSize can be set to a larger value than 5(s) for enhanced robustness to media coders that are idle
    *  for long periods
    * isL4s = true changes congestion window reaction to ECN marking to a scalable function, similar to DCTCP
    */
    ScreamTx(float lossBeta = kLossBeta,
        float ecnCeBeta = kEcnCeBeta,
        float queueDelayTargetMin = kQueueDelayTargetMin,
        bool enableSbd = kEnableSbd,
        float gainUp = kGainUp,
        float gainDown = kGainDown,
        int cwnd = 0,  // An initial cwnd larger than 2*mss
        float cautiousPacing = 0.0f,
        int bytesInFlightHistSize = 5,
        bool isL4s = false,
        bool openWindow = false);

    ~ScreamTx();

    /*
    * Register a new stream {SSRC,PT} tuple,
    *  with a priority value in the range ]0.0..1.0]
    *  where 1.0 denotes the highest priority.
    * It is recommended that at least one stream has prioritity 1.0.
    * Bitrates are specified in bps
    * See constant definitions above for an explanation of other default parameters
    */
    void registerNewStream(RtpQueueIface *rtpQueue,
        uint32_t ssrc,
        float priority,     // priority in range ]0.0 .. 1.0], 1.0 is highest
        float minBitrate,   // Min target bitrate
        float startBitrate, // Starting bitrate
        float maxBitrate,   // Max target bitrate
        float rampUpSpeed = kRampUpSpeed,
        float maxRtpQueueDelay = kMaxRtpQueueDelay,
        float txQueueSizeFactor = kTxQueueSizeFactor,
        float queueDelayGuard = kQueueDelayGuard,
        float lossEventRateScale = kLossEventRateScale,
        float ecnCeEventRateScale = kEcnCeEventRateScale);

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
    void newMediaFrame(uint64_t time_us, uint32_t ssrc, int bytesRtp);

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
    * ecnCeMarkedBytes indicates the cumulative number of bytes that are ECN-CE marked
    */
    void incomingFeedback(uint64_t time_us,
        uint32_t ssrc,         // SSRC of stream
        uint32_t timestamp,    // SCReAM FB timestamp [jiffy]
        uint16_t highestSeqNr, // Highest ACKed RTP sequence number
        uint64_t ackVector,   // ACK vector
        uint16_t ecnCeMarkedBytes = 0); // Number of ECN marked bytes

    /*
    * Parse feedback according to the format below. It is up to the
    * wrapper application this RTCP from a compound RTCP if needed
    * BT = 255, means that this is experimental use
    *
    * 0                   1                   2                   3
    * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    * |V=2|P|reserved |   PT=XR=207   |           length=6            |
    * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    * |                              SSRC                             |
    * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    * |     BT=255    |    reserved   |         block length=4        |
    * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    * |                        SSRC of source                         |
    * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    * | Highest recv. seq. nr. (16b)  |         ECN_CE_bytes          |
    * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    * |                     Ack vector (b0-31)                        |
    * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    * |                     Ack vector (b32-63)                       |
    * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    * |                    Timestamp (32bits)                         |
    * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    */
    void incomingFeedback(uint64_t time_us,
        unsigned char* buf,
        int size);

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
    * Get verbose log information
    */
    void getLog(float time, char *s);

    /*
    * Get verbose log information
    */
    void getShortLog(float time, char *s);

    /*
    * Get overall simplified statistics
    */
    void getStatistics(float time, char *s);


private:
    /*
    * Struct for list of RTP packets in flight
    */
    struct Transmitted {
        uint64_t timeTx_us;
        uint32_t timestamp;
        int size;
        uint16_t seqNr;
        bool isUsed;
        bool isAcked;
        bool isAfterReceivedEdge;

    };

    /*
      * Statistics for the network congestion control and the
      *  stream[0]
      */
    class Statistics {
    public:
        Statistics();
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

    };


    /*
    * One instance is created for each {SSRC,PT} tuple
    */
    class Stream {
    public:
        Stream(ScreamTx *parent,
            RtpQueueIface *rtpQueue,
            uint32_t ssrc,
            float priority,
            float minBitrate,
            float startBitrate,
            float maxBitrate,
            float rampUpSpeed,
            float maxRtpQueueDelay,
            float txQueueSizeFactor,
            float queueDelayGuard,
            float lossEventRateScale,
            float ecnCeEventRateScale);

        float getMaxRate();

        float getTargetBitrate();

        void updateRate(uint64_t time_us);

        void updateTargetBitrateI(float br);

        void updateTargetBitrate(uint64_t time_us);

        bool isRtpQueueDiscard();

        bool isMatch(uint32_t ssrc_) { return ssrc == ssrc_; };
        ScreamTx *parent;
        RtpQueueIface *rtpQueue;      // RTP Packet queue
        uint32_t ssrc;            // SSRC of stream
        float rampUpSpeed;
        float maxRtpQueueDelay;
        float txQueueSizeFactor;
        float queueDelayGuard;
        float lossEventRateScale;
        float ecnCeEventRateScale;

        int credit;             // Credit that is received if another stream gets
        //  priority to transmit
        int creditLost;         // Amount of lost (unused) credit, input to
        //  adjustPriorities function
        float targetPriority;   // Stream target priority
        float targetPriorityInv;// Stream target priority inverted
        int bytesTransmitted;   // Number of bytes transmitted
        int bytesAcked;         // Number of ACKed bytes
        int bytesLost;          // Number of lost bytes
        float rateTransmitted;  // Transmitted rate
        float rateAcked;        // ACKed rate
        float rateLost;         // Lost packets (bit)rate
        uint16_t hiSeqAck;      // Highest sequence number ACKed
        uint16_t hiSeqTx;       // Highest sequence number transmitted
        float minBitrate;       // Min bitrate
        float maxBitrate;       // Max bitrate
        float targetBitrate;    // Target bitrate
        float targetBitrateI;   // Target bitrate inflection point
        bool wasFastStart;      // Was fast start
        bool lossEventFlag;     // Was loss event
        bool ecnCeEventFlag;    // Was ECN mark event
        float txSizeBitsAvg;    // Avergage nymber of bits in RTP queue
        uint64_t lastBitrateAdjustT_us; // Last time rate was updated for this stream
        uint64_t lastRateUpdateT_us;    // Last time rate estimate was updated
        uint64_t lastTargetBitrateIUpdateT_us;    // Last time rate estimate was updated

        uint64_t timeTxAck_us;  // timestamp when higest ACKed SN was transmitted

        int bytesRtp;           // Number of RTP bytes from media coder
        float rateRtp;          // Media bitrate
        float rateRtpHist[kRateUpDateSize];
        float rateAckedHist[kRateUpDateSize];
        float rateLostHist[kRateUpDateSize];
        float rateTransmittedHist[kRateUpDateSize];
        int rateUpdateHistPtr;
        float targetBitrateHist[kTargetBitrateHistSize];
        int targetBitrateHistPtr;
        uint64_t targetBitrateHistUpdateT_us;
        float targetRateScale;

        bool isActive;
        uint64_t lastFrameT_us;
        uint64_t initTime_us;
        bool rtpQueueDiscard;
        uint64_t lastRtpQueueDiscardT_us;
        bool wasRepairLoss;
        bool repairLoss;
        int lastLossDetectIx;
        uint16_t ecnCeMarkedBytes;


        Transmitted txPackets[kMaxTxPackets];
        int txPacketsPtr;

    };

    /*
    * Initialize values
    */
    void initialize(uint64_t time_us);

    /*
    * Mark ACKed RTP packets
    */
    void markAcked(uint64_t time_us, struct Transmitted *txPackets, uint16_t highestSeqNr, uint64_t ackVector, uint32_t timestamp, Stream *stream);

    /*
    * Update CWND
    */
    void updateCwnd(uint64_t time_us);

    /*
    * Detect lost RTP packets
    */
    void detectLoss(uint64_t time_us, struct Transmitted *txPackets, uint16_t highestSeqNr, Stream *stream);

    /*
    * Call this function at regular intervals to determine active streams
    */
    void determineActiveStreams(uint64_t time_us);

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
    * True if competing (TCP)flows detected
    */
    bool isCompetingFlows();

    /*
    * Get stream with corresponding SSRC
    */
    Stream* getStream(uint32_t ssrc);

    /*
    * Get matching stream index for this SSRC tuple,
    *  return -1 if no match
    */
    int getStreamIndex(uint32_t ssrc);

    /*
    * Adjust stream bitrates to reflect priorities
    */
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
    float cautiousPacing;

    uint64_t sRttSh_us;
    uint64_t sRtt_us;
    float sRtt;
    uint32_t ackedOwd;
    uint32_t baseOwd;

    uint32_t baseOwdHist[kBaseOwdHistSize];
    int baseOwdHistPtr;
    uint32_t baseOwdHistMin;
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
    bool openWindow;
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
    float queueDelayTrendMem;
    float maxRate;
    uint64_t lastCwndUpdateT_us;
    bool isL4s;
    float l4sAlpha;
    int bytesMarkedThisRtt;
    int bytesDeliveredThisRtt;
    uint64_t lastL4sAlphaUpdateT_us;
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

    /*
    * Fast start
    */
    bool inFastStart;

    /*
    * Transmission scheduling
    */
    uint64_t paceInterval_us;
    float paceInterval;
    float rateTransmittedAvg;

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
    uint64_t lastBaseDelayRefreshT_us;
    uint64_t initTime_us;
    float queueDelayMin;
    float queueDelayMinAvg;

    /*
    * Variables for multiple steams handling
    */
    Stream *streams[kMaxStreams];
    int nStreams;

    /*
      * Statistics
      */
    Statistics *statistics;

};
#endif
