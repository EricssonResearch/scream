#ifndef SCREAM_TX
#define SCREAM_TX

#include <string.h>
#include <iostream>
#include <glib-object.h>
using namespace std;

// Constants
/*
* Timestamp sampling rate for SCReAM feedback
*/
static const gfloat kTimestampRate = 1000.0f; 
/* Max number of RTP packets in flight
* With and MSS = 1200 byte and an RTT = 200ms
* this is enount to support media bitrates of ~50Mbps
*/
static const gint kMaxTxPackets = 1000;
/*
* Max number of streams
*/
static const gint kMaxStreams = 10;
/*
* History vectors
*/ 
static const gint kBaseOwdHistSize = 50;
static const gint kOwdNormHistSize = 100;
static const gint kOwdFractionHistSize = 20;
static const gint kBytesInFlightHistSize = 5; 
static const gint kRateRtpHistSize = 21;
static const gint kRateUpDateSize = 4;



class RtpQueue;
class ScreamTx {
public:
    ScreamTx();

    ~ScreamTx();

    /*
    * Register a new stream {SSRC,PT} tuple, 
    * with a priority value in the range [0.0..1.0]
    * where 1.0 denotes the highest priority
    * bitrates in bps
    */
    void registerNewStream(RtpQueue *rtpQueue,
        guint32 ssrc, 
        gfloat priority,
        gfloat minBitrate,
        gfloat maxBitrate,
        gfloat frameRate);

    /*
    * Call this function for each new video frame
    *  Note : isOkToTransmit should be called after newMediaFrame
    */ 
    void newMediaFrame(guint64 time_us, guint32 ssrc, gint bytesRtp);

	/*
	* Call this function at regular intervals to determine active streams
	*/
	void determineActiveStreams(guint64 time_us);

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
    gfloat isOkToTransmit(guint64 time_us, guint32 &ssrc);

    /*
    * Add packet to list of transmitted packets
    * should be called when an RTP packet transmitted
    * Return time until isOkToTransmit can be called again
    */
    gfloat addTransmitted(guint64 timestamp_us, // Wall clock ts when packet is transmitted
        guint32 ssrc,
        gint size,
        guint16 seqNr);

    /*
    * New incoming feedback, this function 
    * triggers a CWND update
    * The SCReAM timestamp is in jiffies, where the frequency is controlled
    * by the timestamp clock frequency (default 1000Hz)
    * The ackVector indicates recption of the 32 RTP SN prior to highestSeqNr
    *  Note : isOkToTransmit should be called after incomingFeedback
    */
    void incomingFeedback(guint64 time_us,
        guint32 ssrc,         // SSRC of stream
        guint32 timestamp,    // SCReAM FB timestamp [jiffy]
        guint16 highestSeqNr, // Highest ACKed RTP sequence number
        guint8  numLoss,      // Ackumulated number of losses
        gboolean qBit);       // Source quench bit

    /*
    * Get the target bitrate for stream with SSRC
    * NOTE!, Because SCReAM operates on RTP packets, the target bitrate will
    *  also include the RTP overhead. This means that a subsequent call to set the 
    *  media coder target bitrate must subtract an estimate of the RTP + framing 
    *  overhead. This is not critical for Video bitrates but can be important 
    *  when SCReAM is used to congestion control e.g low bitrate audio streams
    */
    gfloat getTargetBitrate(guint32 ssrc);

    /*
    * Print logs
    */
    void printLog(double time);

private:
    /*
    * Struct for list of RTP packets in flight
    */ 
    struct Transmitted {
        guint64 timeTx_us;
        guint32 timestamp;
        guint32 ssrc;
        gint size;
        guint16 seqNr;
        gboolean isUsed;
    };

    /*
    * One instance is created for each {SSRC,PT} tuple
    */ 
    class Stream {
    public:
        Stream(ScreamTx *parent,
            RtpQueue *rtpQueue,
            guint32 ssrc, 
            gfloat priority,
            gfloat minBitrate,
            gfloat maxBitrate,
            gfloat frameRate); 

        gfloat getMaxRate();

        void updateRate(guint64 time_us);

        void updateTargetBitrate(guint64 time_us);

        gboolean isMatch(guint32 ssrc_) {return ssrc==ssrc_;};
        ScreamTx *parent;
        RtpQueue *rtpQueue;      // RTP Packet queue
        guint32 ssrc;            // SSRC of stream
        gfloat credit;           // Credit that is received if another stream gets priority 
                                 //  to transmit
        gfloat targetPriority;   // Stream target priority
        gint bytesTransmitted;   // Number of bytes transmitted
        gint bytesAcked;         // Number of ACKed bytes
        gfloat rateTransmitted;  // Transmitted rate
        gfloat rateAcked;        // ACKed rate
        gfloat minBitrate;       // Min bitrate
        gfloat maxBitrate;       // Max bitrate
        gfloat targetBitrate;    // Target bitrate
        gfloat frameRate;        // Frame rate
        gfloat targetBitrateI;   // Target bitrate inflection point
        gboolean wasFastStart;   // Was fast start
        gboolean lossEventFlag;  // Was loss event
        gfloat txSizeBitsAvg;    // Avergage nymber of bits in RTP queue
        guint64 lastBitrateAdjustT_us; // Last time rate was updated for this stream
        guint64 lastRateUpdateT_us;    // Last time rate estimate was updated
		guint64 lastTargetBitrateIUpdateT_us;    // Last time rate estimate was updated
		guint8 nLoss;            // Ackumulated loss 

        gint bytesRtp;           // Number of RTP bytes from media coder
        gfloat rateRtp;          // Media bitrate
        gfloat rateRtpSum;       // Temp accumulated rtpRate
        gint rateRtpSumN;        //
        gfloat rateRtpHist[kRateRtpHistSize]; // History of media coder bitrates
        gint rateRtpHistPtr;     // Ptr to above
        gfloat rateRtpMedian;    // Median media bitrate
		gfloat rateRtpHistSh[kRateUpDateSize];
		gfloat rateAckedHist[kRateUpDateSize];
		gfloat rateTransmittedHist[kRateUpDateSize];
		gint rateUpdateHistPtr;

		gboolean isActive;
		guint64 lastFrameT_us;
		guint64 initTime_us;

    };

    /*
    * Initialize values
    * 
    */
    void initialize(guint64 time_us);

    /*
    * Compute 1st order prediction coefficient of OWD multiplied by the owd fraction
    * A value [0.0..1.0] indicates if OWD is increasing
    * This gives a rough estimate of how the one way delay evolves
    */
    void computeOwdTrend();

    /*
    * Update CWND
    */ 
    void updateCwnd(guint64 time_us);

    /*
    * Estimate one way delay [jiffy] and updated base delay
    * Base delay is not subtracted
    */
    guint32 estimateOwd(guint64 time_us);

    /*
    * return base delay [jiffy]
    */
    guint32 getBaseOwd(); 

    /*
    * Compute indicators of shared bottleneck
    */
    void computeSbd();

    /*
    * TRUE if competing (TCP)flows detected
    */
    gboolean isCompetingFlows();

    Stream* getStream(guint32 ssrc);

    /*
    * Get matching stream index for this SSRC tuple,
    *  return -1 if no match
    */ 
    gint getStreamIndex(guint32 ssrc);

    void adjustPriorities(guint64 time_us);

    /*
    * Get the prioritized stream
    *  Return NULL if no stream with 
    *  with RTP packets
    */ 
    Stream* getPrioritizedStream(guint64 time_us);

    /*
    * Add credit to unserved streams
    */
    void addCredit(guint64 time_us,
        Stream* servedStream, 
        gint transmittedBytes);

    /*
    * Subtract used credit 
    */
    void subtractCredit(guint64 time_us,
        Stream* servedStream, 
        gint transmittedBytes);

    /*
    * Variables for multiple steams handling
    */
    Stream *streams[kMaxStreams];
    gint nStreams;

    /*
    * Variables for network congestion control
    */
    /*
    * Related to computation of OWD and target OWD
    */

    Transmitted txPackets[kMaxTxPackets];
    guint64 sRttSh_us;
    guint64 sRtt_us;
    guint32 ackedOwd;                      
    guint32 baseOwd;                       
                                           
    guint32 baseOwdHist[kBaseOwdHistSize]; 
    gint baseOwdHistPtr;
    gfloat owd;
    gfloat owdFractionAvg;
    gfloat owdFractionHist[kOwdFractionHistSize];
    gint owdFractionHistPtr;
    gfloat owdTrend;
    gfloat owdTarget;
    gfloat owdNormHist[kOwdNormHistSize];
    gint owdNormHistPtr;
    gfloat owdSbdVar;
    gfloat owdSbdMean;
    gfloat owdSbdSkew;
    gfloat owdSbdMeanSh;

    /*
    * CWND management
    */
    gint bytesNewlyAcked;
    gint mss; // Maximum Segment Size
    gint cwnd; // congestion window
    gint cwndMin;
    gint cwndI;
    gboolean wasCwndIncrease;
    gint bytesInFlightHistLo[kBytesInFlightHistSize];
    gint bytesInFlightHistHi[kBytesInFlightHistSize];
    gint bytesInFlightHistPtr;
    gint bytesInFlightMaxLo;
    gint bytesInFlightMaxHi;
    gint accBytesInFlightMax;
    gint nAccBytesInFlightMax;
    gfloat rateTransmitted;  
    gfloat owdTrendMem;


    /*
    * Loss event 
    */
    gboolean lossEvent;

    /*
    * Fast start
    */
    gboolean inFastStart;  
    gint nFastStart;

    /*
    * Transmission scheduling
    */
    gfloat pacingBitrate;

    /*
    * Update control variables
    */
    gboolean isInitialized;
    guint64 lastSRttUpdateT_us;
    guint64 lastBaseOwdAddT_us;
    guint64 baseOwdResetT_us;
    guint64 lastAddToOwdFractionHistT_us;
    guint64 lastBytesInFlightT_us;
    guint64 lastCongestionDetectedT_us;
    guint64 lastLossEventT_us;
    guint64 lastTransmitT_us;
    guint64 nextTransmitT_us;
    guint64 lastRateUpdateT_us;
    guint64 lastAdjustPrioritiesT_us;

    /*
    * Methods
    */
    /*
    * return 1 if in fast start
    */
    gint isInFastStart() {return inFastStart ? 1 :  0; };

    /*
    * Get smoothed RTT
    */
    gfloat getSRtt() {return sRtt_us/1e6f;};

    /*
    * Returns the maximum bytes in flight value of the last 
    *  kBytesInFlightHistSize values.
    * Used for congestion window validation.
    */
    gint getMaxBytesInFlightHi();
    gint getMaxBytesInFlightLo();

    /*
    * Returns the number of bytes currently in flight.
    */
    gint bytesInFlight();

    /*
    * Get pacing bitrate
    */
    gfloat getPacingBitrate() {return pacingBitrate;};

    /*
    * Get the fraction between OWD and the OWD target
    */
	gfloat getOwdFraction();

	/*
	* Get the OWD trend
	*/
	gfloat getOwdTrend();
};
#endif