#ifndef VIDEO_ENC
#define VIDEO_ENC

class RtpQueue;
class VideoEnc {
public:
    VideoEnc(RtpQueue* rtpQueue, float frameRate, float delta=0.0, bool simIr=false, bool simIdle=false);

    int encode(float time);

    void setTargetBitrate(float targetBitrate_); 
    void setFrameRate(float frameRate_) {frameRate = frameRate_;};

    RtpQueue* rtpQueue;
    float targetBitrate;
    float frameRate;
    float delta;
    unsigned int seqNr;
    bool simIr;
	bool simIdle;
    bool isIr; 
	int ixIdle = 0;
};


#endif