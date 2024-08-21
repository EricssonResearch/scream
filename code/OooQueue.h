#ifndef OOO_QUEUE
#define OOO_QUEUE


class OooQueueItem {
public:
    OooQueueItem();
    void* packet;
    unsigned int ssrc;
    int size;
    unsigned short seqNr;
    unsigned long timeStamp;
    float tRelease;
    float tQueue;
    bool isCe;
    bool isMark;
    bool used;
};
const int OooQueueSize = 512;
class OooQueue {
public:

    OooQueue(float maxDelay);

    /*
    * Return true of packet is delayed, otherwise false
    */
    bool insert(float time,
        void *rtpPacket,
        unsigned int ssrc,
        int size,
        unsigned short seqNr,
        bool isCe,
        bool isMark,
        unsigned int timeStamp);
    bool extract(float time,
        void* rtpPacket,
        unsigned int& ssrc,
        int& size,
        unsigned short& seqNr,
        bool& isCe,
        bool& isMark,
        unsigned int& timeStamp);

    OooQueueItem *items[OooQueueSize];
    OooQueueItem *item;
    int nItems;
    float maxDelay;
    int nDelayed;
    float prevTRelease;
};

#endif