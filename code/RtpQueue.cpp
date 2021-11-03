#include "RtpQueue.h"
#include <iostream>
#include <string.h>
using namespace std;
/*
* Implements a simple RTP packet queue
*/

RtpQueueItem::RtpQueueItem() {
    used = false;
    size = 0;
    seqNr = 0;
}


RtpQueue::RtpQueue() {
    for (int n=0; n < kRtpQueueSize; n++) {
        items[n] = new RtpQueueItem();
    }
    head = -1;
    tail = 0;
    nItems = 0;
    sizeOfLastFrame = 0;
    bytesInQueue_ = 0;
    sizeOfQueue_ = 0;
    sizeOfNextRtp_ = -1;
}

bool RtpQueue::push(void *rtpPacket, int size, unsigned short seqNr,  bool isMark, float ts) {
    int ix = head+1;
    if (ix == kRtpQueueSize) ix = 0;
    if (items[ix]->used) {
      /*
      * RTP queue is full, do a drop tail i.e ignore new RTP packets
      */
      return (false);
    }
    head = ix;
    items[head]->seqNr = seqNr;
    items[head]->size = size;
    items[head]->ts = ts;
    items[head]->isMark = isMark;
    items[head]->used = true;
    bytesInQueue_ += size;
    sizeOfQueue_ += 1;
#ifndef IGNORE_PACKET
    items[head]->packet = rtpPacket;
#endif
    computeSizeOfNextRtp();
    return (true);
}
bool RtpQueue::pop(void **rtpPacket, int& size, unsigned short& seqNr, bool &isMark)
{
    if (items[tail]->used == false) {
        *rtpPacket = NULL;
        return false;
        sizeOfNextRtp_ = -1;
    } else {
        size = items[tail]->size;

#ifndef IGNORE_PACKET
		*rtpPacket = items[tail]->packet;
#endif
		seqNr = items[tail]->seqNr;
        isMark = items[tail]->isMark;
        items[tail]->used = false;
        tail++; if (tail == kRtpQueueSize) tail = 0;
        bytesInQueue_ -= size;
        sizeOfQueue_ -= 1;
        computeSizeOfNextRtp();
        return true;
    }
}

void RtpQueue::computeSizeOfNextRtp() {
    if (!items[tail]->used) {
        sizeOfNextRtp_ = - 1;
    } else {
        sizeOfNextRtp_ = items[tail]->size;
    }
}

int RtpQueue::sizeOfNextRtp() {
    return sizeOfNextRtp_;
}

int RtpQueue::seqNrOfNextRtp() {
    if (!items[tail]->used) {
        return -1;
    } else {
        return items[tail]->seqNr;
    }
}

int RtpQueue::seqNrOfLastRtp() {
    if (!items[head]->used) {
        return -1;
    } else {
        return items[head]->seqNr;
    }
}

int RtpQueue::bytesInQueue() {
    return bytesInQueue_;
}

int RtpQueue::sizeOfQueue() {
    return sizeOfQueue_;
}

float RtpQueue::getDelay(float currTs) {
    if (items[tail]->used == false) {
        return 0;
    } else {
        return currTs-items[tail]->ts;
    }
}

bool RtpQueue::sendPacket(void **rtpPacket, int& size, unsigned short& seqNr) {
    if (sizeOfQueue() > 0) {
        bool isMark;
        pop(rtpPacket, size, seqNr, isMark);
        return true;
    }
    return false;
}

#ifndef IGNORE_PACKET
extern void packet_free(void *buf);
#endif
int RtpQueue::clear() {
    uint16_t seqNr;
    int freed = 0;
    int size;
    void *buf;
    while (sizeOfQueue() > 0) {
        bool isMark;
        pop(&buf, size, seqNr, isMark);
        if (buf != NULL) {
            freed++;
        }
#ifndef IGNORE_PACKET
		packet_free(buf);
#endif
	}
    return (freed);
}
