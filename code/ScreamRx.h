#ifndef SCREAM_RX
#define SCREAM_RX
#include <cstdint>
#include <list>
const int kAckVectorBits = 64;

class ScreamRx {
public:
    ScreamRx();
    /*
    * One instance is created for each SSRC tuple
    */ 
    class Stream {
    public:
        Stream(uint32_t ssrc); 

        bool isMatch(uint32_t ssrc_) {return ssrc==ssrc_;};

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
    };

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

    /*
    * Get SCReAM RTCP feedback elements
    * return FALSE if no pending feedback available
    */
	bool getFeedback(uint64_t time_us,
		uint32_t &ssrc,
		uint32_t &receiveTimestamp,
		uint16_t &highestSeqNr,
		uint64_t &ackVector);

    uint64_t getLastFeedbackT() {return lastFeedbackT_us;};

    uint64_t lastFeedbackT_us;
    /*
    * Variables for multiple steams handling
    */
	std::list<Stream*> streams;
};

#endif

