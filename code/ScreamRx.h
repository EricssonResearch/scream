#ifndef SCREAM_RX
#define SCREAM_RX
#include <cstdint>
#include <list>
const int kAckVectorBits = 64;

/*
* This module implements the receiver side of SCReAM.
* As SCReAM is a sender based congestion control, the receiver side is
*  actually dumber than dumb. In essense the only thing that it does is to
*  + Record receive time stamps and RTP sequence numbers for incoming RTP packets
*  + Generate RTCP feedback elements
*  + Calculate an appropriate RTCP feedback interval
* See https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx
*  for details on how it is integrated in audio/video platforms.
* A full implementation needs the additional code for
*  + Other obvious stuff such as RTP payload depacketizer, video+audio deoders, rendering, dejitterbuffers
* It is recommended that RTCP feedback for multiple streams are bundled in one RTCP packet.
*  However as low bitrate media (e.g audio) requires a lower feedback rate than high bitrate media (e.g video)
*  it is possible to include RTCP feedback for the audio stream more seldom. The code for this is T.B.D
*/
class ScreamRx {
public:
    ScreamRx(uint32_t ssrc); // SSRC of this RTCP session
    ~ScreamRx();
    
    /*
    * One instance is created for each source SSRC
    */
    class Stream {
    public:
        Stream(uint32_t ssrc);

        bool isMatch(uint32_t ssrc_) { return ssrc == ssrc_; };

        bool checkIfFlushAck();
        /*
        * Receive RTP packet
        */
        void receive(uint64_t time_us,
            void *rtpPacket,
            int size,
            uint16_t seqNr,
            bool isEcnCe);

        uint32_t ssrc;                // SSRC of stream (source SSRC)
        uint16_t highestSeqNr;        // Highest received sequence number
        uint16_t highestSeqNrTx;      // Highest fed back sequence number
        uint32_t receiveTimestamp;    // Wall clock time
        uint64_t ackVector;           // List of received packets
        uint16_t ecnCeMarkedBytes;    // Number of ECN-CE marked bytes
                                      //  (i.e size of RTP packets with CE set in IP header)

        uint64_t lastFeedbackT_us;    // Last time feedback transmitted for
                                      //  this SSRC
        int nRtpSinceLastRtcp;        // Number of RTP packets since last transmitted RTCP

        bool firstReceived;

        float timeStampConversionFactor;

        int ix;
    };

    /*
    * Check to ensure that ACK vector can cover also large holes in
    *  in the received sequence number space. These cases can frequently occur when
    *  SCReAM is used in frame discard mode i.e. when real video rate control is
    *  not possible 
    */
    bool checkIfFlushAck();

    /*
    * Function is called each time an RTP packet is received
    */
    void receive(uint64_t time_us,
        void* rtpPacket,
        uint32_t ssrc,
        int size,
        uint16_t seqNr,
        bool isEcnCe = false);

    /*
    * Return TRUE if an RTP packet has been received and there is
    * pending feedback
    */
    bool isFeedback(uint64_t time_us);

    uint64_t getRtcpFbInterval();

    /*
    * Get SCReAM RTCP feedback elements
    * return FALSE if no pending feedback available
    */
    bool getFeedback(uint64_t time_us,
        uint32_t &ssrc,
        uint32_t &receiveTimestamp,
        uint16_t &highestSeqNr,
        uint64_t &ackVector,
        uint16_t &ecnCeMarkedBytes);

    /*
    * Create feedback according to the format below. It is up to the
    * wrapper application to prepend this RTCP with SR or RR when needed
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
    bool createFeedback(uint64_t time_us, unsigned char *buf, int &size);

    uint64_t getLastFeedbackT() { return lastFeedbackT_us; };

    uint64_t lastFeedbackT_us;
    int bytesReceived;
    uint64_t lastRateComputeT_us;
    float averageReceivedRate;
    uint64_t rtcpFbInterval_us;
    uint32_t ssrc;

    int getIx(uint32_t ssrc);
    int ix;

    /*
    * Variables for multiple steams handling
    */
    std::list<Stream*> streams;
};

#endif
