#ifndef RTP_QUEUE
#define RTP_QUEUE

/*
* Implements a simple RTP packet queue, one RTP queue 
* per stream {SSRC,PT}
*/

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
class RtpQueue {
public:
    RtpQueue();

    void push(void *rtpPacket, int size, unsigned short seqNr, float ts);
    bool pop(void *rtpPacket, int &size, unsigned short &seqNr);
    int sizeOfNextRtp();
    int seqNrOfNextRtp();
    int sizeOfQueue();
    float getDelay(float currTs);
    bool sendPacket(void *rtpPacket, int &size, unsigned short &seqNr);
    void clear();

    RtpQueueItem *items[RtpQueueSize];
    int head; // Pointer to last inserted item
    int tail; // Pointer to the oldest item
    int nItems;
};

#endif