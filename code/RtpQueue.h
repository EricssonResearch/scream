#ifndef RTP_QUEUE
#define RTP_QUEUE

/*
* Implements a simple RTP packet queue, one RTP queue
* per stream {SSRC,PT}
*/

class RtpQueueIface {
public:
    virtual int clear() = 0;
    virtual int sizeOfNextRtp() = 0;
    virtual int seqNrOfNextRtp() = 0;
    virtual int seqNrOfLastRtp() = 0;
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
    bool isMark;
    bool used;
};

const int kRtpQueueSize = 1024;
class RtpQueue : public RtpQueueIface {
public:
    RtpQueue();

    bool push(void *rtpPacket, int size, unsigned short seqNr, bool isMark, float ts);
    bool pop(void **rtpPacket, int &size, unsigned short &seqNr, bool &isMark);
    int sizeOfNextRtp();
    int seqNrOfNextRtp();
    int seqNrOfLastRtp();
    int bytesInQueue(); // Number of bytes in queue
    int sizeOfQueue();  // Number of items in queue
    float getDelay(float currTs);
    bool sendPacket(void **rtpPacket, int &size, unsigned short &seqNr);
    int clear();
    int getSizeOfLastFrame() {return sizeOfLastFrame;};
    void setSizeOfLastFrame(int sz) {sizeOfLastFrame=sz;};
    void computeSizeOfNextRtp();

    RtpQueueItem *items[kRtpQueueSize];
    int head; // Pointer to last inserted item
    int tail; // Pointer to the oldest item
    int nItems;
    int sizeOfLastFrame;

    int bytesInQueue_;
    int sizeOfQueue_;
    int sizeOfNextRtp_;
};

#endif
