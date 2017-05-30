#ifndef VIDEO_ENC
#define VIDEO_ENC

class RtpQueue;
#define TARGET_RATE 100
class VideoEnc {
public:
    VideoEnc(RtpQueue* rtpQueue, float frameRate, float delta = 0.0, bool simIr = false, bool simIdle = false, int delayIx_ = 0);

    int encode(float time);

    void setTargetBitrate(float targetBitrate_);
    void setFrameRate(float frameRate_) { frameRate = frameRate_; };

    RtpQueue* rtpQueue;
    float targetRate[TARGET_RATE];
    int targetRateI;
    float frameRate;
    float delta;
    unsigned int seqNr;
    bool simIr;
    bool simIdle;
    bool isIr;
    int ixIdle = 0;
    int delayIx = 0;
};


#endif