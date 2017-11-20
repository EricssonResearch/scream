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
    float tQueue;
    bool isCe;
    bool used;
};
const int NetQueueSize = 10000;
class NetQueue {
public:

    NetQueue(float delay, float rate=0.0f, float jitter=0.0f, bool isL4s = false);

    void insert(float time,
        void *rtpPacket,
        unsigned int ssrc,
        int size,
        unsigned short seqNr,
        bool isCe = false);
    bool extract(float time, 
        void *rtpPacket, 
        unsigned int &ssrc,
        int& size, 
        unsigned short &seqNr,
        bool &isCe);
    int sizeOfQueue();

    void updateRate(float time);

    NetQueueItem *items[NetQueueSize];
    int head; // Pointer to last inserted item
    int tail; // Pointer to the oldest item
    int nItems;
    float delay;
    float rate;
    float jitter;
    float nextTx;
    float lastQueueLow;
    bool isL4s;
    int bytesTx;
    float lastRateUpdateT;
    float pDrop;
    float prevRateFrac;
    float l4sTh;
};

#endif