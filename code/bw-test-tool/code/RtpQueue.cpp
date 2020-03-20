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

void RtpQueue::push(void *rtpPacket, int size, unsigned short seqNr, float ts) {
    int ix = head+1;
    if (ix == kRtpQueueSize) ix = 0;
    if (items[ix]->used) {
      /*
      * RTP queue is full, do a drop tail i.e ignore new RTP packets
      */
      return;
    }
    head = ix;
    items[head]->seqNr = seqNr;
    items[head]->size = size;
    items[head]->ts = ts;
    items[head]->used = true;
    bytesInQueue_ += size;
    sizeOfQueue_ += 1;
    memcpy(items[head]->packet, rtpPacket, size);
    computeSizeOfNextRtp();
}

bool RtpQueue::pop(void *rtpPacket, int& size, unsigned short& seqNr) {
    if (items[tail]->used == false) {
        return false;
        sizeOfNextRtp_ = -1;
    } else {
        size = items[tail]->size;
        memcpy(rtpPacket,items[tail]->packet,size);
        seqNr = items[tail]->seqNr;
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

bool RtpQueue::sendPacket(void *rtpPacket, int& size, unsigned short& seqNr) {
    if (sizeOfQueue() > 0) {
        pop(rtpPacket, size, seqNr);
        return true;
    }
    return false;
}

void RtpQueue::clear() {
    for (int n=0; n < kRtpQueueSize; n++) {
        items[n]->used = false;
    }
    head = -1;
    tail = 0;
    nItems = 0;
    bytesInQueue_ = 0;
    sizeOfQueue_ = 0;
    sizeOfNextRtp_ = -1;
}
