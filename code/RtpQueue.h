#ifndef RTP_QUEUE
#define RTP_QUEUE

/*
* Implements a simple RTP packet queue, one RTP queue 
* per stream {SSRC,PT}
*/

class RtpQueueIface {
public:
    virtual void clear() = 0;
    virtual int sizeOfNextRtp() = 0;
    virtual int seqNrOfNextRtp() = 0;
    virtual int bytesInQueue() = 0; // Number of bytes in queue
    virtual int sizeOfQueue() = 0;  // Number of items in queue
    virtual float getDelay(float currTs) = 0;
    virtual int getSizeOfLastFrame() = 0;
};

class RtpQueueItem {
public:
    RtpQueueItem();
    void* packet;
    int size;
    unsigned short seqNr;
    float ts;
    bool used;
};

const int RtpQueueSize = 20000;
class RtpQueue : public RtpQueueIface {
public:
    RtpQueue();

    void push(void *rtpPacket, int size, unsigned short seqNr, float ts);
    bool pop(void *rtpPacket, int &size, unsigned short &seqNr);
    int sizeOfNextRtp();
    int seqNrOfNextRtp();
	int bytesInQueue(); // Number of bytes in queue
	int sizeOfQueue();  // Number of items in queue
	float getDelay(float currTs);
    bool sendPacket(void *rtpPacket, int &size, unsigned short &seqNr);
    void clear();
	void setSizeOfLastFrame(int aSize) { sizeOfLastFrame = aSize;};
	int getSizeOfLastFrame() {return sizeOfLastFrame;};

    RtpQueueItem *items[RtpQueueSize];
    int head; // Pointer to last inserted item
    int tail; // Pointer to the oldest item
    int nItems;
	int sizeOfLastFrame; // Size of last frame in bytes
};

#endif
