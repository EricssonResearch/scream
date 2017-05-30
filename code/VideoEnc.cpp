#include "VideoEnc.h"
#include "RtpQueue.h"
#include <string.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

using namespace std;

static const int kMaxRtpSize = 1200;
static const int kRtpOverHead = 20; // 12 byte RTP header + PL format framing
static const float scale[] = { 1.0f, 1.0f, 1.0f, 0.1f, 1.0f, 1.0f, 1.0f, 1.0f, 0.1f, 1.0f };

VideoEnc::VideoEnc(RtpQueue* rtpQueue_, float frameRate_, float delta_, bool simIr_, bool simIdle_, int delayIx_) {
    rtpQueue = rtpQueue_;
    frameRate = frameRate_;
    delta = delta_;
    simIr = simIr_;
    simIdle = simIdle_;
    for (int n = 0; n < TARGET_RATE; n++) {
        targetRate[n] = 0.0f;
    }
    targetRateI = 0;
    seqNr = 0;
    if (simIr)
        isIr = true;
    else
        isIr = false;
    delayIx = delayIx_;
    ixIdle = 0;// (rand() % 10);
}

void VideoEnc::setTargetBitrate(float targetBitrate_) {
    targetRate[targetRateI] = targetBitrate_;
    targetRateI = (targetRateI + 1) % TARGET_RATE;
}

float rnd2 = 0.0f;
int VideoEnc::encode(float time) {
    int rtpBytes = 0;
    int t_ms = (int)(time * 1000);
    if ((t_ms % 5000) < 1 && simIr)
        isIr = true;
    //if ((t_ms == 1000) && simIr && false)
    //		isIr = true;
    rnd2 = 0.8*rnd2 + delta*2.0f*(((float)(rand())) / RAND_MAX - 0.5f);
    float rnd = 1.0f + rnd2;
    if (isIr) {
        // Adjust slighly to compensate for IRs
        //
        rnd *= 1.0f - 1.0f / (frameRate*2.0f);
    }
    //rnd *= 0.8;
    int ix = targetRateI - 1 - delayIx;
    //cerr << delayIx << endl;
    if (ix < 0) ix += TARGET_RATE;
    float rtpPktPerSec = std::max(frameRate, targetRate[ix] / (1200 * 8));


    float rtpOverHead = 1 * rtpPktPerSec*(kRtpOverHead * 8);
    //cerr << rtpOverHead << " " << rtpPktPerSec << endl;

    float tbr = targetRate[ix];
    //if (time > 20 && time < 25)
    //   tbr = 100000;
    float tmp = std::max(1000.0f, tbr - rtpOverHead);
    int bytes = (int)((tmp / frameRate / 8.0)*rnd);
    if (isIr) {
        bytes *= 4;
        isIr = false;
    }
    if (simIdle) {
        if ((t_ms % 10000) < 25) {
            ixIdle = (ixIdle + 1) % 10;
        }
        bytes = (int)(bytes*scale[ixIdle]);
    }

    int sizeOfFrame = 0;
    while (bytes > 0) {
        int rtpSize = std::min(kMaxRtpSize, bytes);
        bytes -= rtpSize;
        rtpSize += kRtpOverHead;
        rtpBytes += rtpSize;
        rtpQueue->push(0, rtpSize, seqNr, time);
        sizeOfFrame += rtpSize;
        seqNr++;
    }
    rtpQueue->setSizeOfLastFrame(sizeOfFrame);
    return rtpBytes;
}

