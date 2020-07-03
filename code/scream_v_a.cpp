// Simple test application for verification of SCReAM function
//

#include "stdafx.h"
#include "VideoEnc.h"
#include "RtpQueue.h"
#include "NetQueue.h"
#include "ScreamTx.h"
#include "ScreamRx.h"

using namespace std;

const float Tmax = 100;
const bool isChRate = false;
const bool printLog = true;
const bool ecnCapable = false;
const bool isL4s = false;
const float FR = 50.0f;
int swprio = -1;
#define TRACEFILE "../traces/trace_key.txt"
//#define TRACEFILE "../traces/trace_no_key.txt"
//#define TRACEFILE "../traces/trace_no_key_smooth.txt"
/*
* Mode determines how many streams should be run
* 1 = audio, 2 = video, 3 = 1+2, 4 = 
*/
const int mode = 0x01;
const double timeBase = 10000.0;

int main(int argc, char* argv[])
{
    int tick = (int)(timeBase / FR);
    ScreamTx *screamTx = new ScreamTx(0.8f, 0.9f, 0.06f, false, 1.0f, 5.0f, 0, 1.25f, 20, isL4s, false, false);

	screamTx->setCwndMinLow(5000);

    ScreamRx *screamRx = new ScreamRx(0,1,1);
    RtpQueue *rtpQueue[3] = { new RtpQueue(), new RtpQueue(), new RtpQueue() };
    VideoEnc *videoEnc[3] = { 0, 0, 0};
    NetQueue *netQueueDelay = new NetQueue(0.04f, 0.0f, 0.0f);
    NetQueue *netQueueRate = new NetQueue(0.0f, 20e6, 0.0f, isL4s);
    videoEnc[0] = new VideoEnc(rtpQueue[0], FR, (char*)TRACEFILE, 0);
	//videoEnc[0] = new VideoEnc(rtpQueue[0], FR, (char*)TRACEFILE);
	videoEnc[1] = new VideoEnc(rtpQueue[1], FR, (char*)TRACEFILE, 50);
    videoEnc[2] = new VideoEnc(rtpQueue[2], FR, (char*)TRACEFILE, 100);
    if (mode & 0x01)
		screamTx->registerNewStream(rtpQueue[0], 10, 0.7f, 1e6f, 1e6f, 20e6f, 10e6f, 0.2f, 0.2f, 0.2f, 0.1f);//, 0.9f, 0.95f, false);//, 0.7f, 0.8f, false);
        //screamTx->registerNewStream(rtpQueue[0], 10, 1.0f, 20e3f, 200e3f, 200e3f, 1e6f, 0.5f, 0.2f, 0.1f, 0.1f);
    if (mode & 0x02)
		//screamTx->registerNewStream(rtpQueue[1], 11, 0.2f, 2056e3f, 10024e3f, 80192e3f, 10e6f, 0.5f, 0.2f, 0.1f, 0.1f);
		screamTx->registerNewStream(rtpQueue[1], 11, 0.6f, 1e6f, 1e6f, 15e6f, 10e6f, 0.5f, 0.2f, 0.2f, 0.1f);
	    //screamTx->registerNewStream(rtpQueue[1], 11, 0.2f, 256e3f, 1024e3f, 8192e3f, 1e6f, 0.5f, 0.2f, 0.1f, 0.1f);
    if (mode & 0x04)
        screamTx->registerNewStream(rtpQueue[2], 12, 0.2f, 256e3f, 1024e3f, 8192e3f, 1e6f, 0.5f, 0.2f, 0.1f, 0.1f);


    float time = 0.0f;
    uint32_t time_ntp = 0;
    uint32_t time_ntp_rx = 0;
    int n = 0;
    int nPkt = 0;
    uint32_t ssrc;
    char rtpPacket[2000];
    int size;
    uint16_t seqNr;
    int nextCallN = -1;
    bool isFeedback = false;
    double lastLogT = -1.0;
    while (time <= Tmax) {
        float retVal = -1.0;
        time = n / timeBase;
        time_ntp = n*(65536 / timeBase) + 100;
        time = time_ntp / 65536.0f;
        time_ntp_rx = n * (65536.0f / timeBase);
        netQueueRate->updateRate(time);
        bool isEvent = false;

        if (n % tick == 0) {
            // "Encode" audio + video frame
            if (mode & 0x01) {
                videoEnc[0]->setTargetBitrate(screamTx->getTargetBitrate(10));
                int bytes = videoEnc[0]->encode(time);
                screamTx->newMediaFrame(time_ntp, 10, bytes);
            }
            if (mode & 0x02) {
                videoEnc[1]->setTargetBitrate(screamTx->getTargetBitrate(11));
                int bytes = videoEnc[1]->encode(time);
                screamTx->newMediaFrame(time_ntp, 11, bytes);
            }
            if (mode & 0x04) {
                videoEnc[2]->setTargetBitrate(screamTx->getTargetBitrate(12));
                int bytes = videoEnc[2]->encode(time);
                screamTx->newMediaFrame(time_ntp, 12, bytes);
            }
            /*
            * New RTP packets added, try if OK to transmit
            */
            retVal = screamTx->isOkToTransmit(time_ntp, ssrc);
            isEvent = true;
        }

        bool isCe = false;
        if (netQueueDelay->extract(time, rtpPacket, ssrc, size, seqNr, isCe)) {
            netQueueRate->insert(time, rtpPacket, ssrc, size, seqNr, isCe);
        }

        if (true || time < 30.0 || time > 30.1) {
            if (netQueueRate->extract(time, rtpPacket, ssrc, size, seqNr, isCe)) {
                if (seqNr % 200 == 19 && false) {
                    cerr << "lost " << seqNr << endl;
                }
                else {
                    uint8_t ceBits = 0x00;
                    if (ecnCapable) {
                        if (isL4s)
                            ceBits = 0x01;
                        else
                            ceBits = 0x02;
                        if (isCe) ceBits = 0x03;
                    }
					//if (isCe)
					//cerr << time << " " << isCe << endl;

					screamRx->receive(time_ntp_rx, 0, ssrc, size, seqNr, ceBits);
                }
                nPkt++;
            }
        }

        unsigned char buf[2000];
        int fb_size = -1;
        if (true || time < 30.0 || time > 30.5) {
            bool isFeedback = screamRx->isFeedback(time_ntp_rx) &&
                (time_ntp_rx - screamRx->getLastFeedbackT() > screamRx->getRtcpFbInterval() || screamRx->checkIfFlushAck());

            if (isFeedback && screamRx->createStandardizedFeedback(time_ntp_rx, false, buf, fb_size)) {
                screamTx->incomingStandardizedFeedback(time_ntp, buf, fb_size);
                retVal = screamTx->isOkToTransmit(time_ntp, ssrc);
                isEvent = true;
            }
        }

        if (n == nextCallN && retVal != 0.0f) {
            retVal = screamTx->isOkToTransmit(time_ntp, ssrc);
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
            bool isMark = false; //XXX TODO: figure out correct value
            retVal = screamTx->addTransmitted(time_ntp, ssrc, size, seqNr, isMark);
            nextCallN = n + max(1, (int)(1000.0*retVal));
            isEvent = true;
        }

        if (true && printLog && time - lastLogT > 0.05) {
            cout << time <<  " " ;
            char s[500];
            screamTx->getShortLog(time, s);
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
            if ((time > 20.0 && time < 30) && isChRate) {
                netQueueRate->rate = 20000e3;
            }
            else {
                netQueueRate->rate = 50000e3;
            }
        }
        
        if (time > 30 && swprio == 0) {
            swprio = 1;
            screamTx->setTargetPriority(10, 0.2);
            screamTx->setTargetPriority(11, 1.0);
        }
        if (time > 35 && swprio == 1) {
            swprio = 2;
            screamTx->setTargetPriority(10, 1.0);
            screamTx->setTargetPriority(11, 0.2);
        }

        if (false && time > 50)
            netQueueRate->rate = 8e6;
           
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

