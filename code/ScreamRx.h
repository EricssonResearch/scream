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
* See https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pdf
*  for details on how it is integrated in audio/video platforms.
* A full implementation needs the additional code for
*  + RTCP feedback (e.g using RFC3611 XR elements)
*  + Other obvious stuff such as RTP payload depacketizer, video+audio deoders, rendering, dejitterbuffers
* It is recommended that RTCP feedback for multiple streams are bundled in one RTCP packet.
*  However as low bitrate media (e.g audio) requires a lower feedback rate than high bitrate media (e.g video)
*  it is possible to include RTCP feedback for the audio stream more seldom. The code for this is T.B.D
*/
class ScreamRx {
public:
    ScreamRx();
    /*
    * One instance is created for each SSRC tuple
    */
    class Stream {
    public:
        Stream(uint32_t ssrc);

        bool isMatch(uint32_t ssrc_) { return ssrc == ssrc_; };

        bool checkIfFlushAck(int seqNr);
        /*
        * Receive RTP packet
        */
        void receive(uint64_t time_us,
            void *rtpPacket,
            int size,
            uint16_t seqNr);

        uint32_t ssrc;                // SSRC of stream
        uint16_t highestSeqNr;        // Highest received sequence number
        uint32_t receiveTimestamp;    // Wall clock time
        uint64_t ackVector;           // List of received packets

        uint64_t lastFeedbackT_us;    // Last time feedback transmitted for
        //  this SSRC
        int nRtpSinceLastRtcp;       // Number of RTP packets since last transmitted RTCP

        bool firstReceived;
    };

    /*
    * Check to ensure that ACK vector can cover also large holes in
    *  in the received sequence number space. These cases can frequently occur when
    *  SCReAM is used in frame discard mode i.e. when real video rate control is
    *  not possible
    */
    bool checkIfFlushAck(
        uint32_t ssrc,
        uint16_t seqNr);

    /*
    * Function is called each time an RTP packet is received
    */
    void receive(uint64_t time_us,
        void* rtpPacket,
        uint32_t ssrc,
        int size,
        uint16_t seqNr);

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
        uint64_t &ackVector);

    uint64_t getLastFeedbackT() { return lastFeedbackT_us; };

    uint64_t lastFeedbackT_us;
    int bytesReceived;
    uint64_t lastRateComputeT_us;
    float averageReceivedRate;


    /*
    * Variables for multiple steams handling
    */
    std::list<Stream*> streams;
};

#endif

