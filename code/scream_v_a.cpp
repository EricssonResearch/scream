// scream_01.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "VideoEnc.h"
#include "RtpQueue.h"
#include "NetQueue.h"
#include "ScreamTx.h"
#include "ScreamRx.h"

using namespace std;

const float Tmax = 100.0f;
const bool isChRate = false;

/*
* Mode determines how many streams should be run
* 1 = audio, 2 = video, 3 = 1+2
*/
const int mode = 0x02;

int main(int argc, char* argv[])
{
    int tick = (int)(10000.0 / 25.0);
    ScreamTx *screamTx = new ScreamTx(0.6f, 0.1f, false);
    ScreamRx *screamRx = new ScreamRx();
    RtpQueue *rtpQueue[2] = { new RtpQueue(), new RtpQueue() };
    VideoEnc *videoEnc[2] = { 0, 0 };
    NetQueue *netQueueDelay = new NetQueue(0.05f, 0.0f, 0.03f);
    NetQueue *netQueueRate = new NetQueue(0.0f, 5000e03, 0.0f);
    videoEnc[0] = new VideoEnc(rtpQueue[0], 25.0, 0.1f, false, false, 0);
    videoEnc[1] = new VideoEnc(rtpQueue[1], 25.0, 0.2f, false, false, 0);
    if (mode & 0x01)
        screamTx->registerNewStream(rtpQueue[0], 10, 1.0f, 6000.0f, 6000.0f, 100e3f, 5000.0f, 2.0f, 1.0f, 0.1f);
    //screamTx->registerNewStream(rtpQueue[0], 10, 1.0f, 64000.0f, 10e6f, 1000000.0f);
    //screamTx->registerNewStream(rtpQueue[0], 10, 1.0f, 64000.0f, 10000000.0f, 50000.0f, 2.0f, 1.0f, 0.3f);
    //screamTx->registerNewStream(rtpQueue[0], 10, 1.0f, 5000.0f, 100000.0f, 5000.0f, 2.0f, 1.0f, 0.3f);
    if (mode & 0x02)
        screamTx->registerNewStream(rtpQueue[1], 11, 1.0f, 64e3f, 64e3f, 10e6f, 1000000.0f);
    //screamTx->registerNewStream(rtpQueue[1], 11, 1.0f, 64000.0f, 10000000.0f, 50000.0f, 2.0f, 1.0f, 0.3f);


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
        time = n / 10000.0f;
        time_us = n * 100;
        time_us_tx = time_us + 000000;
        time_us_rx = time_us + 000000;

        bool isEvent = false;

        if (n % tick == 0) {
            // "Encode" audio + video frame
            if (mode & 0x01) {
                videoEnc[0]->setTargetBitrate(screamTx->getTargetBitrate(10));
                int bytes = videoEnc[0]->encode(time);
                screamTx->newMediaFrame(time_us_tx, 10, bytes);
            }
            if (mode & 0x02) {
                videoEnc[1]->setTargetBitrate(screamTx->getTargetBitrate(11));
                int bytes = videoEnc[1]->encode(time);
                screamTx->newMediaFrame(time_us_tx, 11, bytes);
            }
            /*
            * New RTP packets added, try if OK to transmit
            */
            retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
            isEvent = true;
        }

        if (netQueueRate->extract(time, rtpPacket, ssrc, size, seqNr)) {
            netQueueDelay->insert(time, rtpPacket, ssrc, size, seqNr);
        }
        if (netQueueDelay->extract(time, rtpPacket, ssrc, size, seqNr)) {
            if ((nPkt % 1000 == 19 || nPkt % 1000 == 21) && false) {
                cerr << "lost " << seqNr << endl;
            }
            else {
                screamRx->receive(time_us_rx, 0, ssrc, size, seqNr);
            }
            nPkt++;
        }
        isFeedback |= screamRx->isFeedback(time_us);
        uint32_t rxTimestamp;
        uint16_t aseqNr;
        uint64_t aackVector;
        uint64_t rtcpFbInterval_us = screamRx->getRtcpFbInterval();
        if (isFeedback && (time_us_rx - screamRx->getLastFeedbackT() > rtcpFbInterval_us)) {
            if (screamRx->getFeedback(time_us_rx, ssrc, rxTimestamp, aseqNr, aackVector)) {
                screamTx->incomingFeedback(time_us_tx, ssrc, rxTimestamp, aseqNr, aackVector, false);
                retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
                isFeedback = false;
                isEvent = true;
            }
            /*
            * Fetch both at the same time if there is any,  more efficient
            */
            if (screamRx->getFeedback(time_us_rx, ssrc, rxTimestamp, aseqNr, aackVector)) {
                screamTx->incomingFeedback(time_us_tx, ssrc, rxTimestamp, aseqNr, aackVector, false);
                retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
                isFeedback = false;
                isEvent = true;
            }
        }

        if (n == nextCallN && retVal != 0.0f) {
            retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
            if (retVal > 0) {
                nextCallN = n + max(1, (int)(1000.0*retVal));
                isEvent = true;
            }
        }
        if (retVal == 0) {
            /*
            * RTP packet can be transmitted
            */
            if (ssrc == 10)
                rtpQueue[0]->sendPacket(rtpPacket, size, seqNr);
            else
                rtpQueue[1]->sendPacket(rtpPacket, size, seqNr);
            netQueueRate->insert(time, rtpPacket, ssrc, size, seqNr);
            retVal = screamTx->addTransmitted(time_us_tx, ssrc, size, seqNr);
            nextCallN = n + max(1, (int)(1000.0*retVal));
            isEvent = true;
        }

        if (true && isEvent) {
            cout << time << " ";
            char s[500];
            screamTx->printLog(time, s);
            //cout << endl;
            cout << " " << s << endl;
        }

        /*
        * Test the set traget priority feature
        if (time > 30 && time < 100)
        screamTx->setTargetPriority(11, 0.1);
        else if (time > 50)
        screamTx->setTargetPriority(11, 0.5);
        */
        if (isChRate) {
            if ((time >= 30) && (time < 60) && isChRate) {
                netQueueRate->rate = 1000e3;
            }
            else {
                netQueueRate->rate = 5000e3;
            }
        }
        /*

            if ((time >= 60) && (time < 80) && isChRate)
            netQueueRate->rate = 600e3;

            if ((time >= 80) && (time < 100) && isChRate)
            netQueueRate->rate = 1000e3;

            if ((time >= 100) && isChRate)
            netQueueRate->rate = 2000e3;
            */
        n++;
#ifdef _WIN32
        Sleep(0);
#else
        sleep(0);
#endif
    }

    return 0;
}

