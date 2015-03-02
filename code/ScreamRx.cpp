#include "ScreamRx.h"
#include "ScreamTx.h"
#include <string.h>
#include <iostream>
using namespace std;

ScreamRx::Stream::Stream(guint32 ssrc_) {
    ssrc = ssrc_;
    ackVector = 0xffffffff;
    nLoss = 0;
    receiveTimestamp = 0x0;
    highestSeqNr = 0x0;
    lastFeedbackT_us =  0;
    pendingFeedback = FALSE;
}

void ScreamRx::Stream::receive(guint64 time_us, 
    gpointer rtpPacket, 
    gint size, 
    guint16 seqNr) {
        /*
        * Make things wrap-around safe
        */
        guint32 seqNrExt = seqNr;
        guint32 highestSeqNrExt = highestSeqNr;
        if (seqNrExt < highestSeqNrExt && highestSeqNrExt-seqNrExt > 20000)
            seqNrExt += 65536;
        else if (seqNrExt > highestSeqNrExt && seqNrExt - highestSeqNrExt > 20000)
            highestSeqNrExt += 65536;

        if (seqNrExt >= highestSeqNrExt) {
            /*
            * Normal in-order reception
            */
            guint16 diff = seqNrExt - highestSeqNrExt;
            if (diff) {
                if (diff >= 16) {
                    ackVector = 0x0000;
                }
                else {
                    // Fill with potential zeros
                    ackVector = ackVector >> diff;
                    // Add previous highest seq nr to ack vector
                    ackVector = ackVector | (1 << (16 - diff));
                }
            }
            highestSeqNr = seqNr;
        } else {
            /*
            * Out-of-order reception
            */
            guint16 diff = highestSeqNrExt - seqNrExt;
            if (diff < 16) {
                ackVector = ackVector | (1 << (16 - diff));
            }
        }
        if ((ackVector & (1 << (16-5))) == 0) {
            /*
            * Check for losses, give a grace time of 5 RTP packets
            * to handle reordering
            */
            nLoss++;
        }

        receiveTimestamp = (guint)(time_us/1000);
        pendingFeedback = TRUE;
}

ScreamRx::ScreamRx() {
    lastFeedbackT_us = 0;
    streams = NULL;
}

void ScreamRx::receive(guint64 time_us, 
    gpointer rtpPacket, 
    guint32 ssrc,
    gint size, 
    guint16 seqNr) {
        if (streams != NULL) {
            GSList *l = streams;
            while (l != NULL) {
                Stream *stream = (Stream*) (l->data);
                if (stream->isMatch(ssrc)) {
                    /*
                    * Packets for this SSRC received earlier
                    * stream is thus already in list
                    */
                    stream->receive(time_us,rtpPacket,size,seqNr);
                    return;
                }
                l = g_slist_next(l);
            }
        }
        /*
        * New {SSRC,PT} 
        */
        Stream *stream = new Stream(ssrc);
        stream->receive(time_us,rtpPacket,size,seqNr);
        streams = g_slist_append(streams, stream);
}


gboolean ScreamRx::isFeedback() {
    GSList *l = streams;
    while (l != NULL) {
        Stream *stream = (Stream*) (l->data);
        if (stream->pendingFeedback) {
            return TRUE;
        }
        l = g_slist_next(l);
    }
    return FALSE;
}	

gboolean ScreamRx::getFeedback(guint64 time_us,
    guint32 &ssrc,
    guint32 &receiveTimestamp,
    guint16 &highestSeqNr,
    guint8 &nLoss) {
        Stream *stream = NULL;
        GSList *l = streams;
        guint64 minT_us = ULONG_MAX;
        while (l != NULL) {
            Stream *tmp = (Stream*) (l->data);
            if (tmp->pendingFeedback && tmp->lastFeedbackT_us < minT_us) {
                stream = tmp;
                minT_us = tmp->lastFeedbackT_us;
            }
            l = g_slist_next(l);
        }

        if (stream == NULL)
            return FALSE;

        receiveTimestamp = stream->receiveTimestamp;
        highestSeqNr = stream->highestSeqNr;
        ssrc = stream->ssrc;
        nLoss = stream->nLoss;

        stream->lastFeedbackT_us = time_us;
        stream->pendingFeedback = FALSE;
        lastFeedbackT_us = time_us;

        return TRUE;
}	
