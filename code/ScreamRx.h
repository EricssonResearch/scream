#ifndef SCREAM_RX
#define SCREAM_RX
#include <glib-object.h>

class ScreamRx {
public:
    ScreamRx();
    /*
    * One instance is created for each SSRC tuple
    */ 
    class Stream {
    public:
        Stream(guint32 ssrc); 

        gboolean isMatch(guint32 ssrc_) {return ssrc==ssrc_;};

        /*
        * Receive RTP packet 
        * return TRUE if loss detected
        */
        void receive(guint64 time_us, 
            void *rtpPacket, 
            gint size, 
            guint16 seqNr);

        guint32 ssrc;                // SSRC of stream
        guint16 highestSeqNr;        // Highest received sequence number
        guint32 receiveTimestamp;    // Wall clock time
        guint16 ackVector;           // List of received packets
        guint8  nLoss;               // Ackumulated loss

        guint64 lastFeedbackT_us;    // Last time feedback transmitted for
                                     //  this SSRC
        gboolean pendingFeedback;    // TRUE if new feedback pending
    };

    /*
    * Function is called each time an RTP packet is received
    */
    void receive(guint64 time_us, 
        gpointer rtpPacket, 
        guint32 ssrc,
        gint size, 
        guint16 seqNr);

    /*
    * Return TRUE if an RTP packet has been received and there is
    * pending feedback
    */
    gboolean isFeedback();

    /*
    * Get SCReAM RTCP feedback elements
    * return FALSE if no pending feedback available
    */
    gboolean getFeedback(guint64 time_us,
        guint32 &ssrc,
        guint32 &receiveTimestamp,
        guint16 &highestSeqNr,
        guint8 &nLoss);

    guint64 getLastFeedbackT() {return lastFeedbackT_us;};

    guint64 lastFeedbackT_us;
    /*
    * Variables for multiple steams handling
    */
    GSList *streams;
};

#endif

