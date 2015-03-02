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

const gint screamTxTick = (int)(1e-3*1000);
const gfloat Tmax = 200.0f;
const gboolean isChRate = false; 
const guint64 rtcpFbInterval_us = 30000;
const gfloat kVideoFrameRate = 25.0f;
const gfloat kAudioFrameRate = 50.0f;
const gint kVideoTick = (int)(1000.0/kVideoFrameRate);
const gint kAudioTick = (int)(1000.0/kAudioFrameRate);

int _tmain(int argc, _TCHAR* argv[])
{
    ScreamTx *screamTx = new ScreamTx();
    ScreamRx *screamRx = new ScreamRx();
    RtpQueue *rtpQueue[2] = {new RtpQueue(), new RtpQueue()};
    VideoEnc *videoEnc[2] = {0,0};
    NetQueue *netQueueDelay = new NetQueue(0.05f,0.0f,0.0f);
    NetQueue *netQueueRate = 0;
    int ssrcL[2] = {10,11};
    netQueueRate = new NetQueue(0.0f,1.5e6,0.0f);
    videoEnc[0] = new VideoEnc(rtpQueue[0], kVideoFrameRate, 0.3f); 
    videoEnc[1] = new VideoEnc(rtpQueue[1], kAudioFrameRate, 0.0f);
    screamTx->registerNewStream(rtpQueue[0], ssrcL[0], 1.0f, 150000.0f, 1500000.0f, kVideoFrameRate);
    screamTx->registerNewStream(rtpQueue[1], ssrcL[1], 1.0f,   5000.0f,   50000.0f, kAudioFrameRate);

    gfloat time = 0.0f;
    guint64 time_us = 0;
    guint64 time_us_tx = 0;
    guint64 time_us_rx = 0;
    gint n = 0;
    gint nPkt = 0;
    guint ssrc;
    void *rtpPacket = 0;
    gint size;
    guint16 seqNr;
    gint nextCallN = -1;
    gboolean isFeedback = FALSE;

    while (time <= Tmax) {
        float retVal = -1.0;
        time = n/1000.0f;
        time_us = n*1000;
        time_us_tx = time_us+000000;
        time_us_rx = time_us+000000;
        gboolean isEvent = FALSE;		
        gboolean isEncoded = FALSE;
        //if (n % kVideoTick == 0) {
        if ((n+kAudioTick/2) % kAudioTick == 0) {
            // "Encode" audio frame
            videoEnc[1]->setTargetBitrate(screamTx->getTargetBitrate(ssrcL[1]));
            videoEnc[1]->encode(time);
            screamTx->newMediaFrame(time_us_tx, ssrcL[1]);
            isEncoded = true;
        }
        if (n % kVideoTick == 0) {
            // "Encode" video frame
            videoEnc[0]->setTargetBitrate(screamTx->getTargetBitrate(ssrcL[0]));
            videoEnc[0]->encode(time);
            screamTx->newMediaFrame(time_us_tx, ssrcL[0]);
            isEncoded = TRUE;
        }
        if (isEncoded) {
            retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
            isEvent = TRUE;
        }

        if (netQueueRate->extract(time, rtpPacket, ssrc, size, seqNr)) {
            netQueueDelay->insert(time, rtpPacket, ssrc, size, seqNr);
        }
        if (netQueueDelay->extract(time,rtpPacket, ssrc, size, seqNr)) {
            if ((nPkt % 2000 == 199) && false) {
            } else {
                screamRx->receive(time_us_rx, 0, ssrc, size, seqNr);
                isFeedback |= screamRx->isFeedback();
            }
            nPkt++;

        }
        guint32 rxTimestamp;
        guint16 aseqNr;
        guint8 anLoss;
        if (isFeedback && (time_us_rx - screamRx->getLastFeedbackT() > rtcpFbInterval_us)) {
            if (screamRx->getFeedback(time_us_rx, ssrc, rxTimestamp, aseqNr, anLoss)) {
                screamTx->incomingFeedback(time_us_tx, ssrc, rxTimestamp, aseqNr, anLoss, false);
                retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
                isEvent = TRUE;
            }
            isFeedback = FALSE;
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
            int ix = 0;
            if (ssrc == ssrcL[0])
                ix = 0;
            else if (ssrc == ssrcL[1])
                ix = 1;
            if (rtpQueue[ix]->sendPacket(rtpPacket, size, seqNr)) {
                netQueueRate->insert(time,rtpPacket, ssrc, size, seqNr);
                retVal = screamTx->addTransmitted(time_us_tx, ssrc, size, seqNr);
                nextCallN = n + max(1,(int)(1000.0*retVal));
                isEvent = TRUE;
            } else {
                nextCallN = n+1;
            }
        }

        if (isEvent) {
            cout << time << " ";
            screamTx->printLog(time);
            cout << endl;
        }

        if ((time == 20 || time == 60) && isChRate)
            netQueueRate->rate = 3e5;

        if ((time == 30 || time == 70) && isChRate)
            netQueueRate->rate = 3e6;

        n++;
        Sleep(0) ;
    }

    return 0;
}

