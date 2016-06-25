#include "NetQueue.h"
#include <iostream>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

using namespace std;
/*
 * Implements a simple RTP packet queue
 */

NetQueueItem::NetQueueItem() {
	packet = 0;
	used = false;
	size = 0;
	seqNr = 0;
	tRelease = 0.0;
}


NetQueue::NetQueue(float delay_, float rate_, float jitter_) {
	for (int n=0; n < NetQueueSize; n++) {
		items[n] = new NetQueueItem();
	}
	head = -1;
	tail = 0;
	nItems = 0;
	rate = rate_;
	delay = delay_;
	jitter = jitter_;
	nextTx = 0;
}

void NetQueue::insert(float time, 
	                  void *rtpPacket,  
					  unsigned int ssrc,
					  int size, 
					  unsigned short seqNr) {
	head++; if (head == NetQueueSize) head = 0;
	items[head]->used = true;
	items[head]->packet = rtpPacket;
	items[head]->ssrc = ssrc;
	items[head]->size = size;
	items[head]->seqNr = seqNr;
	items[head]->tRelease = time+delay+jitter*(rand()/float(RAND_MAX));
	if (false && jitter > 0)
		cerr << items[head]->tRelease << endl;
	if (rate > 0)
		items[head]->tRelease += sizeOfQueue()*8.0f/rate;
	items[head]->tRelease = std::max(items[head]->tRelease, nextTx);
	nextTx = items[head]->tRelease;
}

bool NetQueue::extract(float time, 
	                   void *rtpPacket,   
					   unsigned int &ssrc,
					   int& size, 
					   unsigned short& seqNr) {
	if (items[tail]->used == false) {
		return false;
	} else {
		if (time >= items[tail]->tRelease) {
	      rtpPacket = items[tail]->packet;
		  seqNr = items[tail]->seqNr;
		  ssrc = items[tail]->ssrc;
		  size = items[tail]->size;
		  items[tail]->used = false;
		  tail++; if (tail == NetQueueSize) tail = 0;
		  return true;
		}
		return false;
	}
}

int NetQueue::sizeOfQueue() {
	int size = 0;
	for (int n=0; n < NetQueueSize; n++) {
		if (items[n]->used)  
			size += items[n]->size;
	}
	return size;
}
