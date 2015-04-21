#include "VideoEnc.h"
#include "RtpQueue.h"
#include <string.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
using namespace std;

static const int kMaxRtpSize = 1200;
static const int kRtpOverHead = 20; // 12 byte RTP header + PL format framing

VideoEnc::VideoEnc(RtpQueue* rtpQueue_, float frameRate_, float delta_, bool simIr_) {
    targetBitrate = 1000000;
    rtpQueue = rtpQueue_;
    frameRate = frameRate_;
    delta = delta_;
    simIr = simIr_;
    targetBitrate = 150000.0f;
    seqNr = 0;
    if (simIr)
        isIr = true;
    else
        isIr = false;
}

void VideoEnc::setTargetBitrate(float targetBitrate_) {
    targetBitrate = targetBitrate_;
};


int VideoEnc::encode(float time) {
    int rtpBytes = 0;
    int t_ms = (int) (time*1000);
    if ((t_ms % 5000) < 1 && simIr) 
        isIr = true;
    float rnd = 1.0f+delta*2.0f*(((float)(rand()))/RAND_MAX-0.5f);
    if (isIr) {
        // Adjust slighly to compensate for IRs
        //
        rnd *= 1.0-1.0/(frameRate*2.0);
    }
    float rtpPktPerSec = std::max(frameRate,targetBitrate/(1200*8));
    float rtpOverHead = 0*rtpPktPerSec*(kRtpOverHead*8);
    //cerr << rtpOverHead << endl;

    float tbr = targetBitrate;
    //if (time > 20 && time < 25)
    //   tbr = 100000;
    float tmp = std::max(5000.0f,tbr-rtpOverHead);
    int bytes = (int)((tmp/frameRate/8.0)*rnd);
    if (isIr) {
        bytes *= 5;
        isIr = false;
    }
    while (bytes > 0) {
        int rtpSize = std::min(kMaxRtpSize,bytes);
        bytes -= rtpSize;
        rtpSize += kRtpOverHead; 
        rtpBytes += rtpSize;
        rtpQueue->push(0,rtpSize,seqNr,time);
        seqNr++;
    }
    return rtpBytes;
}

