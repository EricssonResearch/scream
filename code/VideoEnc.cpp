#include "VideoEnc.h"
#include "RtpQueue.h"
#include <string.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

using namespace std;

static const int kMaxRtpSize = 1200;
static const int kRtpOverHead = 12;

VideoEnc::VideoEnc(RtpQueue* rtpQueue_, float frameRate_, char *fname, int ixOffset_) {
    rtpQueue = rtpQueue_;
    frameRate = frameRate_;
    ix = ixOffset_;
    nFrames = 0;
    seqNr = 0;
    nominalBitrate = 0.0;
    FILE *fp = fopen(fname,"r");
    char s[100];
    float sum = 0.0;
    while (fgets(s,99,fp)) {
        if (nFrames < MAX_FRAMES - 1) {
            float x = atof(s);
            frameSize[nFrames] = x;
            nFrames++;
            sum += x;
        }
    }
    float t = nFrames / frameRate;
    nominalBitrate = sum * 8 / t;
    fclose(fp);
}

void VideoEnc::setTargetBitrate(float targetBitrate_) {
    targetBitrate = targetBitrate_;
}

int VideoEnc::encode(float time) {
    int rtpBytes = 0;
    int bytes = (int) (frameSize[ix]/nominalBitrate*targetBitrate);
    nominalBitrate = 0.95*nominalBitrate + 0.05*frameSize[ix] * frameRate * 8;
    ix++; if (ix == nFrames) ix = 0;
    while (bytes > 0) {
        int rtpSize = std::min(kMaxRtpSize, bytes);
        bytes -= rtpSize;
        rtpSize += kRtpOverHead;
        rtpBytes += rtpSize;
        rtpQueue->push(0, rtpSize, seqNr, time);
        seqNr++;
    }
    rtpQueue->setSizeOfLastFrame(rtpBytes);
    return rtpBytes;
}

