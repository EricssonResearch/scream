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


void VideoEnc::encode(float time) {
    int t_ms = (int) (time*1000);
    if ((t_ms % 2000) ==0 && simIr) 
        isIr = true;
    float rnd = 1.0f+delta*2.0f*(((float)(rand()))/RAND_MAX-0.5f);
    float rtpPktPerSec = std::max(frameRate,targetBitrate/(1200*8));
    float rtpOverHead = 0*rtpPktPerSec*(kRtpOverHead*8);
    //cerr << rtpOverHead << endl;
    float tmp = std::max(5000.0f,targetBitrate-rtpOverHead);
    int bytes = (int)((tmp/frameRate/8.0)*rnd);
    if (isIr) {
        bytes *= 4;
        isIr = false;
    }
    while (bytes > 0) {
        int rtpSize = std::min(kMaxRtpSize,bytes);
        bytes -= rtpSize;
        rtpSize += kRtpOverHead; 
        rtpQueue->push(0,rtpSize,seqNr,time);
        seqNr++;
    }
}

