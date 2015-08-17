// scream_01.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "VideoEnc.h"
#include "RtpQueue.h"
#include "NetQueue.h"
#include "ScreamTx.h"
#include "ScreamRx.h"
#include <iostream>
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
using namespace std;

const gfloat Tmax = 120.0f;
const gboolean testLowBitrate = FALSE;
const gboolean isChRate = TRUE; 

int _tmain(int argc, _TCHAR* argv[])
{

    guint64 rtcpFbInterval_us;
    gfloat kFrameRate = 25.0f;
    gint videoTick = (gint)(2000.0/kFrameRate);
    if (testLowBitrate) {
        kFrameRate = 50.0f;
        videoTick = (gint)(2000.0/kFrameRate);
        rtcpFbInterval_us = 100000;
    } else {
        kFrameRate = 25.0f;
        videoTick = (gint)(2000.0/kFrameRate);
        rtcpFbInterval_us = 30000;
    }
    ScreamTx *screamTx = new ScreamTx();
    ScreamRx *screamRx = new ScreamRx();
    RtpQueue *rtpQueue = new RtpQueue();
    VideoEnc *videoEnc = 0;
    NetQueue *netQueueDelay = new NetQueue(0.01f,0.0f,0.03f);
    NetQueue *netQueueRate = 0;
    if (testLowBitrate) {
        netQueueRate = new NetQueue(0.0f,50000,0.0f);
        videoEnc = new VideoEnc(rtpQueue, kFrameRate, 0.3f); 
        screamTx->registerNewStream(rtpQueue, 10, 1.0f, 5000.0f, 50000.0f,kFrameRate);
    } else {
        netQueueRate = new NetQueue(0.0f,1000e3,0.0f);
        videoEnc = new VideoEnc(rtpQueue, kFrameRate, 0.3f,false);
        screamTx->registerNewStream(rtpQueue, 10, 1.0f, 100000.0f, 5000000.0f,kFrameRate);
    }


    gfloat time = 0.0f;
    guint64 time_us = 0;
    guint64 time_us_tx = 0;
    guint64 time_us_rx = 0;
    gint n = 0;
    gint nPkt = 0;
    guint32 ssrc;
    guint8 pt;
    void *rtpPacket = 0;
    gint size;
    guint16 seqNr;
    gint nextCallN = -1;
    gboolean isFeedback = FALSE;

    while (time <= Tmax) {
        gfloat retVal = -1.0;
        time = n/2000.0f;
        time_us = n*500;
        time_us_tx = time_us+000000;
        time_us_rx = time_us+000000;

        gboolean isEvent = FALSE;

        if (n % videoTick == 0) {
            // "Encode" video frame
            videoEnc->setTargetBitrate(screamTx->getTargetBitrate(10));
            int bytes = videoEnc->encode(time);
            screamTx->newMediaFrame(time_us_tx, 10, bytes);
            /*
            * New RTP packets added, try if OK to transmit
            */
            retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
            isEvent = TRUE;
        }

        if (netQueueRate->extract(time, rtpPacket, ssrc, size, seqNr)) {
            netQueueDelay->insert(time, rtpPacket, ssrc, size, seqNr);
        }
        if (netQueueDelay->extract(time,rtpPacket, ssrc, size, seqNr)) {
            if ((nPkt % 2000 == 19) && false) {
            } else {
                screamRx->receive(time_us_rx, 0, ssrc, size, seqNr);
                isFeedback |= screamRx->isFeedback();
            }
            nPkt++;

        }
        guint32 rxTimestamp;
        guint16 aseqNr;
        guint32 aackVector;
        guint8 anLoss;
        if (isFeedback && (time_us_rx - screamRx->getLastFeedbackT() > rtcpFbInterval_us)) {
            if (screamRx->getFeedback(time_us_rx, ssrc, rxTimestamp, aseqNr, anLoss)) {
                screamTx->incomingFeedback(time_us_tx, ssrc, rxTimestamp, aseqNr,  anLoss, FALSE);
                retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
                isFeedback = FALSE;
                isEvent = TRUE;
            }
        }

        if (n==nextCallN && retVal != 0.0f) {
            retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
            if (retVal > 0) {
                nextCallN = n + max(1,(int)(1000.0*retVal));
                isEvent = TRUE;
            }
        }
        if (retVal == 0) {
            /*
            * RTP packet can be transmitted
            */ 
            rtpQueue->sendPacket(rtpPacket, size, seqNr);
            netQueueRate->insert(time,rtpPacket, ssrc, size, seqNr);
            retVal = screamTx->addTransmitted(time_us_tx, ssrc, size, seqNr);
            nextCallN = n + max(1,(int)(1000.0*retVal));
            isEvent = TRUE;
        }

        if (true && isEvent) {
            cout << time << " ";
            screamTx->printLog(time);
            cout << endl;
        }

            if ((time >= 40) && (time < 60) && isChRate)
                netQueueRate->rate = 2000e3;

            if ((time >= 60) && (time < 80) && isChRate)
                netQueueRate->rate = 600e3;

			if ((time >= 80) && (time < 100) && isChRate)
                netQueueRate->rate = 1000e3;

            if ((time >= 100) && isChRate)
                netQueueRate->rate = 2000e3;

        n++;
        Sleep(0) ;
    }

    return 0;
}

