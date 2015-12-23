#include "ScreamRx.h"
#include "ScreamTx.h"
#include <climits>
#include <string.h>
#include <iostream>
using namespace std;

ScreamRx::Stream::Stream(uint32_t ssrc_) {
    ssrc = ssrc_;
    ackVector = 0xffffffff;
    nLoss = 0;
    receiveTimestamp = 0x0;
    highestSeqNr = 0x0;
    lastFeedbackT_us =  0;
    pendingFeedback = false;
}

void ScreamRx::Stream::receive(uint64_t time_us, 
    void* rtpPacket, 
    int size, 
    uint16_t seqNr) {
        /*
        * Make things wrap-around safe
        */
        uint32_t seqNrExt = seqNr;
        uint32_t highestSeqNrExt = highestSeqNr;
        if (seqNrExt < highestSeqNrExt && highestSeqNrExt-seqNrExt > 20000)
            seqNrExt += 65536;
        else if (seqNrExt > highestSeqNrExt && seqNrExt - highestSeqNrExt > 20000)
            highestSeqNrExt += 65536;

        if (seqNrExt >= highestSeqNrExt) {
            /*
            * Normal in-order reception
            */
            uint16_t diff = seqNrExt - highestSeqNrExt;
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
            uint16_t diff = highestSeqNrExt - seqNrExt;
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

        receiveTimestamp = (uint32_t)(time_us/1000);
        pendingFeedback = true;
}

ScreamRx::ScreamRx() {
    lastFeedbackT_us = 0;
    // streams = NULL;
}

void ScreamRx::receive(uint64_t time_us, 
    void* rtpPacket, 
    uint32_t ssrc,
    int size, 
    uint16_t seqNr) {
        if (!streams.empty()) {
         for (auto it = streams.begin(); it != streams.end(); ++it)
	 {
           if ((*it)->isMatch(ssrc)) {
             /*
              * Packets for this SSRC received earlier
              * stream is thus already in list
              */
              (*it)->receive(time_us,rtpPacket,size,seqNr);
              return;
            }

         }
        }
#if 0
        // if (streams != NULL) {
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
#endif
        /*
        * New {SSRC,PT} 
        */
        Stream *stream = new Stream(ssrc);
        stream->receive(time_us,rtpPacket,size,seqNr);
        //streams = g_slist_append(streams, stream);
        streams.push_back(stream);
}


bool ScreamRx::isFeedback() {
        if (!streams.empty()) {
         for (auto it = streams.begin(); it != streams.end(); ++it)
	 {
           if ((*it)->pendingFeedback) {
              return true;
            }

         }
        }
#if 0
    GSList *l = streams;
    while (l != NULL) {
        Stream *stream = (Stream*) (l->data);
        if (stream->pendingFeedback) {
            return true;
        }
        l = g_slist_next(l);
    }
#endif
    return false;
}	

bool ScreamRx::getFeedback(uint64_t time_us,
    uint32_t &ssrc,
    uint32_t &receiveTimestamp,
    uint16_t &highestSeqNr,
    uint8_t &nLoss) {
        Stream *stream = NULL;
        uint64_t minT_us = ULONG_MAX;
         for (auto it = streams.begin(); it != streams.end(); ++it)
	 {
           if ((*it)->pendingFeedback && (*it)->lastFeedbackT_us < minT_us) {
                stream = *it;
                minT_us = (*it)->lastFeedbackT_us;
            }

         }
#if 0
        Stream *stream = NULL;
        GSList *l = streams;
        uint64_t minT_us = ULONG_MAX;
        while (l != NULL) {
            Stream *tmp = (Stream*) (l->data);
            if (tmp->pendingFeedback && tmp->lastFeedbackT_us < minT_us) {
                stream = tmp;
                minT_us = tmp->lastFeedbackT_us;
            }
            l = g_slist_next(l);
        }
#endif
        if (stream == NULL)
            return false;

        receiveTimestamp = stream->receiveTimestamp;
        highestSeqNr = stream->highestSeqNr;
        ssrc = stream->ssrc;
        nLoss = stream->nLoss;

        stream->lastFeedbackT_us = time_us;
        stream->pendingFeedback = false;
        lastFeedbackT_us = time_us;

        return true;
}	
