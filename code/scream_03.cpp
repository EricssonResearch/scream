// scream_03.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "VideoEnc.h"
#include "RtpQueue.h"
#include "NetQueue.h"
#include "ScreamTx.h"
#include "ScreamRx.h"
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
using namespace std;

const int screamTxTick = (int)(1e-3*1000);
const float Tmax = 150.0f;
const bool isChRate = false; 
const uint64_t rtcpFbInterval_us = 30000;
const float kFrameRate = 25.0f;

int main(int argc, char* argv[])
{
    ScreamTx *screamTx = new ScreamTx();
    ScreamRx *screamRx = new ScreamRx();
    RtpQueue *rtpQueue[3] = {new RtpQueue(), new RtpQueue(),new RtpQueue() };
    VideoEnc *videoEnc[3] = {0,0,0};
    NetQueue *netQueueDelay = new NetQueue(0.05f,0.0f,0.0f);
    NetQueue *netQueueRate = 0;
    int ssrcL[3] = {10,11,12};
	int tick;
	tick = (int)(1000.0 / kFrameRate);
	netQueueRate = new NetQueue(0.0f,5e6,0.0f);
    videoEnc[0] = new VideoEnc(rtpQueue[0], kFrameRate, 0.3f, false, false); 
    videoEnc[1] = new VideoEnc(rtpQueue[1], kFrameRate, 0.3f, false, false);
	videoEnc[2] = new VideoEnc(rtpQueue[2], kFrameRate, 0.3f, false, false);
	screamTx->registerNewStream(rtpQueue[0], ssrcL[0], 0.5f, 200000.0f, 5000000.0f, kFrameRate);
    screamTx->registerNewStream(rtpQueue[1], ssrcL[1], 1.0f, 200000.0f, 5000000.0f, kFrameRate);
	screamTx->registerNewStream(rtpQueue[2], ssrcL[2], 0.5f, 200000.0f, 5000000.0f, kFrameRate);

    float time = 0.0f;
    uint64_t time_us = 0;
    uint64_t time_us_tx = 0;
    uint64_t time_us_rx = 0;
    int n = 0;
    int nPkt = 0;
    uint32_t ssrc;
    void *rtpPacket = 0;
    int size;
    uint16_t seqNr;
    int nextCallN = -1;
    bool isFeedback = false;

    while (time <= Tmax) {
        float retVal = -1.0;
        time = n/1000.0f;
        time_us = n*1000;
        time_us_tx = time_us+000000;
        time_us_rx = time_us+000000;
        bool isEvent = false;		
        bool isEncoded = false;

		screamTx->determineActiveStreams(time_us_tx);
			//if (n % kVideoTick == 0) {
		if ((n + tick / 2) % tick == 0) {
			// "Encode" audio frame
			if ((time > 31 && time < 91)) {
				videoEnc[1]->setTargetBitrate(screamTx->getTargetBitrate(ssrcL[1]));
				int bytes = videoEnc[1]->encode(time);
				screamTx->newMediaFrame(time_us_tx, ssrcL[1], bytes);
				isEncoded = true;
			}
        }
        if (n % tick == 0) {
            // "Encode" video frame
            videoEnc[0]->setTargetBitrate(screamTx->getTargetBitrate(ssrcL[0]));
            int bytes = videoEnc[0]->encode(time);
            screamTx->newMediaFrame(time_us_tx, ssrcL[0], bytes);
            isEncoded = true;
        }
		if ((n + tick / 3) % tick == 0) {
			// "Encode" audio frame
				videoEnc[2]->setTargetBitrate(screamTx->getTargetBitrate(ssrcL[2]));
				int bytes = videoEnc[2]->encode(time);
				screamTx->newMediaFrame(time_us_tx, ssrcL[2], bytes);
				isEncoded = true;
		}
		if (isEncoded) {
            retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
            isEvent = true;
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
        uint32_t rxTimestamp;
        uint16_t aseqNr;
        uint8_t anLoss;
        if (isFeedback && (time_us_rx - screamRx->getLastFeedbackT() > rtcpFbInterval_us)) {
            if (screamRx->getFeedback(time_us_rx, ssrc, rxTimestamp, aseqNr, anLoss)) {
                screamTx->incomingFeedback(time_us_tx, ssrc, rxTimestamp, aseqNr, anLoss, false);
                retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
                isEvent = true;
            }
            isFeedback = false;
        }

        if (n==nextCallN && retVal != 0.0f) {
            retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
            if (retVal > 0) {
                nextCallN = n + max(1,(int)(1000.0*retVal));
                isEvent = true;
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
			else if (ssrc == ssrcL[2])
				ix = 2;
			if (rtpQueue[ix]->sendPacket(rtpPacket, size, seqNr)) {
                netQueueRate->insert(time,rtpPacket, ssrc, size, seqNr);
                retVal = screamTx->addTransmitted(time_us_tx, ssrc, size, seqNr);
                nextCallN = n + max(1,(int)(1000.0*retVal));
                isEvent = true;
            } else {
                nextCallN = n+1;
            }
        }

        if (isEvent) {
            cout << time << " ";
            screamTx->printLog(time);
            cout << endl;
        }

        n++;
#ifdef _WIN32
        Sleep(0) ;
#else
        sleep(0);
#endif
    }

    return 0;
}

