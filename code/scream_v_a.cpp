// Simple test application for verification of SCReAM function
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
const bool printLog = true;
const bool ecnCapable = false;
const bool isL4s = false;
const float FR = 25.0f;
#define TRACEFILE "../traces/trace_no_key_smooth.txt"
//#define TRACEFILE "../traces/trace_key.txt"
/*
* Mode determines how many streams should be run
* 1 = audio, 2 = video, 3 = 1+2, 4 = 
*/
const int mode = 0x01;
const double timeBase = 50000.0;

int main(int argc, char* argv[])
{
    int tick = (int)(timeBase / FR);
    ScreamTx *screamTx = new ScreamTx(0.8f, 0.9f, 0.1f, false, 1.0f, 2.0f, 0, 0.0f, 20, isL4s);
    ScreamRx *screamRx = new ScreamRx(0);
    RtpQueue *rtpQueue[3] = { new RtpQueue(), new RtpQueue(), new RtpQueue() };
    VideoEnc *videoEnc[3] = { 0, 0, 0};
    NetQueue *netQueueDelay = new NetQueue(0.05f, 0.0f, 0.002f);
    NetQueue *netQueueRate = new NetQueue(0.0f, 5e6, 0.0f, isL4s);
    videoEnc[0] = new VideoEnc(rtpQueue[0], FR, (char*)TRACEFILE);
    videoEnc[1] = new VideoEnc(rtpQueue[1], FR, (char*)TRACEFILE, 50);
    videoEnc[2] = new VideoEnc(rtpQueue[2], FR, (char*)TRACEFILE, 100);
    if (mode & 0x01)
       //screamTx->registerNewStream(rtpQueue[0], 10, 1.0f, 256e3f, 1024e3f, 100e6f, 4e6f, 0.2f, 0.1f, 0.1f);
        screamTx->registerNewStream(rtpQueue[0], 10, 1.0f, 256e3f, 1024e3f, 8192e3f, 1e6f, 0.2f, 0.1f, 0.1f);
    if (mode & 0x02)
        screamTx->registerNewStream(rtpQueue[1], 11, 0.2f, 256e3f, 1024e3f, 8192e3f, 1e6f, 0.2f, 0.1f, 0.1f);
    if (mode & 0x04)
        screamTx->registerNewStream(rtpQueue[2], 12, 0.2f, 256e3f, 1024e3f, 8192e3f, 1e6f, 0.2f, 0.1f, 0.1f);


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
    double lastLogT = -1.0;
    bool swprio = false;
    while (time <= Tmax) {
        float retVal = -1.0;
        time = n / timeBase;
        time_us = n*(1e6 / timeBase);
        time_us_tx = time_us + 000000;
        time_us_rx = time_us + 000000;

        netQueueRate->updateRate(time);
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
            if (mode & 0x04) {
                videoEnc[2]->setTargetBitrate(screamTx->getTargetBitrate(12));
                int bytes = videoEnc[2]->encode(time);
                screamTx->newMediaFrame(time_us_tx, 12, bytes);
            }
            /*
            * New RTP packets added, try if OK to transmit
            */
            retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
            isEvent = true;
        }

        bool isCe = false;
        if (netQueueDelay->extract(time, rtpPacket, ssrc, size, seqNr, isCe)) {
            netQueueRate->insert(time, rtpPacket, ssrc, size, seqNr, isCe);
        }
        if (netQueueRate->extract(time, rtpPacket, ssrc, size, seqNr, isCe)) {
            if ((nPkt % 1000 == 19 || nPkt % 1000 == 21) && false) {
                cerr << "lost " << seqNr << endl;
            }
            else {
                if (!ecnCapable) isCe = false;
                screamRx->receive(time_us_rx, 0, ssrc, size, seqNr, isCe);
            }
            nPkt++;
        }
        unsigned char buf[100];
        int fb_size = -1;
        if (screamRx->createFeedback(time_us_rx, buf, fb_size)) {
            screamTx->incomingFeedback(time_us_tx, buf, fb_size);
            retVal = screamTx->isOkToTransmit(time_us_tx, ssrc);
            isEvent = true;
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
            switch (ssrc) {
            case 10:
                rtpQueue[0]->sendPacket(rtpPacket, size, seqNr);
                break;
            case 11:
                rtpQueue[1]->sendPacket(rtpPacket, size, seqNr);
                break;
            case 12:
                rtpQueue[2]->sendPacket(rtpPacket, size, seqNr);
                break;
            }
            netQueueDelay->insert(time, rtpPacket, ssrc, size, seqNr);
            retVal = screamTx->addTransmitted(time_us_tx, ssrc, size, seqNr);
            nextCallN = n + max(1, (int)(1000.0*retVal));
            isEvent = true;
        }

        if (printLog && time - lastLogT > 0.01) {
            cout << time << " ";
            char s[500];
            screamTx->getLog(time, s);
            //cout << endl;
            cout << " " << s << endl;
            lastLogT = time;
        }
        /*
        * Test the set traget priority feature
        if (time > 30 && time < 100)
        screamTx->setTargetPriority(11, 0.1);
        else if (time > 50)
        screamTx->setTargetPriority(11, 0.5);
        */
        if (isChRate) {
            if ((time > 30 && time < 60) && isChRate) {
                netQueueRate->rate = 1000e3;
            }
            else {
                netQueueRate->rate = 4000e3;
            }
        }
        if (false && time > 50 && !swprio) {
            swprio = true;
            //screamTx->setTargetPriority(10, 0.2);
            screamTx->setTargetPriority(11, 1.0);
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

