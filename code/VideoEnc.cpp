#include "VideoEnc.h"
#include "RtpQueue.h"
#include <string.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

using namespace std;

static const int kMaxRtpSize = 1360;
static const int kRtpOverHead = 12;

uint32_t SSRC = 1;
VideoEnc::VideoEnc(RtpQueue* rtpQueue_, float frameRate_, char *fname, int ixOffset_, float sluggishness_) {
    rtpQueue = rtpQueue_;
    frameRate = frameRate_;
    ix = ixOffset_;
    nFrames = 0;
    seqNr = 0;
    nominalBitrate = 0.0;
    sluggishness = sluggishness_;
    FILE *fp = fopen(fname,"r");
    char s[100];
    float sum = 0.0;
    bytes = 0.0f;
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
	char rtpPacket[2000];
	float tmp = (int)(frameSize[ix] / nominalBitrate * targetBitrate);
    if (bytes > 0)
        bytes = bytes * (sluggishness)+(1.0 - sluggishness) * tmp;
    else
        bytes = tmp;
    int tmp2 = (int) bytes;
	//nominalBitrate = 0.95*nominalBitrate + 0.05*frameSize[ix] * frameRate * 8;
    ix++; if (ix == nFrames) ix = 0;
    while (tmp2 > 0) {
        int rtpSize = std::min(kMaxRtpSize, tmp2);
        bool isMarker = rtpSize < kMaxRtpSize;
        tmp2 -= rtpSize;
        rtpSize += kRtpOverHead;
        rtpBytes += rtpSize;
        rtpQueue->push(rtpPacket, rtpSize, SSRC, seqNr, isMarker, time);
        seqNr++;
    }
    rtpQueue->setSizeOfLastFrame(rtpBytes);
    return rtpBytes;
}

