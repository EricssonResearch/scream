#include "RtpQueue.h"
#include <iostream>
#include <string.h>
using namespace std;
/*
* Implements a simple RTP packet queue
*/

const int RtpSize = 1500;
RtpQueueItem::RtpQueueItem() {
    packet = 0;
    used = false;
    size = 0;
    seqNr = 0;
}


RtpQueue::RtpQueue() {
    for (int n=0; n < RtpQueueSize; n++) {
        items[n] = new RtpQueueItem();
    }
    head = -1;
    tail = 0;
    nItems = 0;
	sizeOfLastFrame = 0;
}

void RtpQueue::push(void *rtpPacket, int size, unsigned short seqNr, float ts) {
    head++; if (head == RtpQueueSize) head = 0;
    items[head]->packet = rtpPacket;
    items[head]->seqNr = seqNr;
    items[head]->size = size;
    items[head]->ts = ts;
    items[head]->used = true;
}

bool RtpQueue::pop(void *rtpPacket, int& size, unsigned short& seqNr) {
    if (items[tail]->used == false) {
        return false;
    } else {
        rtpPacket = items[tail]->packet;
        size = items[tail]->size;
        seqNr = items[tail]->seqNr;
        items[tail]->used = false;
        tail++; if (tail == RtpQueueSize) tail = 0;
        return true;
    }
}

int RtpQueue::sizeOfNextRtp() {
    if (!items[tail]->used) {
        return -1;
    } else {
        return items[tail]->size;
    }
}

int RtpQueue::seqNrOfNextRtp() {
    if (!items[tail]->used) {
        return -1;
    } else {
        return items[tail]->seqNr;
    }
}

int RtpQueue::bytesInQueue() {
	int size = 0;
	for (int n = 0; n < RtpQueueSize; n++) {
		if (items[n]->used)
			size += items[n]->size;
	}
	return size;
}

int RtpQueue::sizeOfQueue() {
	int size = 0;
	for (int n = 0; n < RtpQueueSize; n++) {
		if (items[n]->used)
			size += 1;
	}
	return size;
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
    for (int n=0; n < RtpQueueSize; n++) {
        items[n]->used = false;  
    }
    head = -1;
    tail = 0;
    nItems = 0;
}