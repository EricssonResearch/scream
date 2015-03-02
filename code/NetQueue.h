#ifndef NET_QUEUE
#define NET_QUEUE


class NetQueueItem {
public:
    NetQueueItem();
    void* packet;
    unsigned int ssrc;
    int size;
    unsigned short seqNr;
    float tRelease;
    bool used;
};

const int NetQueueSize = 1000;
class NetQueue {
public:

    NetQueue(float delay, float rate=0.0f, float jitter=0.0f);

    void insert(float time, 
        void *rtpPacket, 
        unsigned int ssrc,
        int size, 
        unsigned short seqNr);
    bool extract(float time, 
        void *rtpPacket, 
        unsigned int &ssrc,
        int& size, 
        unsigned short &seqNr);
    int sizeOfQueue();

    NetQueueItem *items[NetQueueSize];
    int head; // Pointer to last inserted item
    int tail; // Pointer to the oldest item
    int nItems;
    float delay;
    float rate;
    float jitter;
    float nextTx;
};

#endif