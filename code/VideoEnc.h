#ifndef VIDEO_ENC
#define VIDEO_ENC

class RtpQueue;
#define MAX_FRAMES 10000
class VideoEnc {
public:
    VideoEnc(RtpQueue* rtpQueue, float frameRate, char *fname, int ixOffset=0);

    int encode(float time);

    void setTargetBitrate(float targetBitrate);

    RtpQueue* rtpQueue;
    float frameSize[MAX_FRAMES];
    int nFrames;
    float targetBitrate;
    float frameRate;
    float nominalBitrate;
    unsigned int seqNr;
    int ix;
};


#endif