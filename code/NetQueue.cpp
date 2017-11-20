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
    tQueue = 0.0;
}

NetQueue::NetQueue(float delay_, float rate_, float jitter_, bool isL4s_) {
	for (int n=0; n < NetQueueSize; n++) {
		items[n] = new NetQueueItem();
	}
	head = -1;
	tail = 0;
	nItems = 0;
	rate = rate_;
	delay = delay_;
	jitter = jitter_;
    lastQueueLow = 0.0;
	nextTx = 0;
    isL4s = isL4s_;
    bytesTx = 0;
    lastRateUpdateT = 0;
    pDrop = 0.0f;
    prevRateFrac = 0.0f;
    l4sTh = std::max(0.005, 5.0 * 1200 * 8 / rate);
}

void NetQueue::insert(float time, 
	                  void *rtpPacket,  
					  unsigned int ssrc,
					  int size, 
					  unsigned short seqNr,
                      bool isCe) {
	head++; if (head == NetQueueSize) head = 0;
	items[head]->used = true;
	items[head]->packet = rtpPacket;
	items[head]->ssrc = ssrc;
	items[head]->size = size;
	items[head]->seqNr = seqNr;
	items[head]->tRelease = time+delay+jitter*(rand()/float(RAND_MAX));
    items[head]->tQueue = time;
    items[head]->isCe = isCe;
    if (rate > 0) {
        float delay = sizeOfQueue()*8.0f / rate;
        items[head]->tRelease += delay;
    }
	items[head]->tRelease = std::max(items[head]->tRelease, nextTx);
	nextTx = items[head]->tRelease;
}

bool NetQueue::extract(float time, 
	                   void *rtpPacket,   
					   unsigned int &ssrc,
					   int& size, 
                       unsigned short& seqNr,
                       bool& isCe) {
	if (items[tail]->used == false) {
        lastQueueLow = time;
		return false;
	} else {
        if (time >= items[tail]->tRelease) {
            rtpPacket = items[tail]->packet;
            seqNr = items[tail]->seqNr;
            ssrc = items[tail]->ssrc;
            size = items[tail]->size;
            isCe = items[tail]->isCe;
            items[tail]->used = false;
            /*
            * Implement a rudimentary CoDel-ish ECN marker (without the 1/sqrt(N) part)
            */
            float qDel = time - items[tail]->tQueue;
            if (!isL4s && time - items[tail]->tQueue <= 0.005 && rate > 0.0) {
                lastQueueLow = time;
            }
            if (!isL4s && time - lastQueueLow > 0.1 && rate > 0.0) {
                isCe = true;
                lastQueueLow = time;
            }
            if (isL4s && time - items[tail]->tQueue > l4sTh && rate > 0.0) {
              isCe = true;
            }
            if (isL4s && pDrop > 0.0f) {
              float rnd = float(rand()) / RAND_MAX;
              if (rnd < pDrop) {
                  isCe = true;
              }
          }
          bytesTx += size;
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

const float rateUpdateT = 0.05;
const float rateFracTarget1 = 0.9;
void NetQueue::updateRate(float time) {
    if (time - lastRateUpdateT >= rateUpdateT && rate > 0) {
        float dT = time - lastRateUpdateT;
        float rateT = bytesTx * 8 / dT;
        bytesTx = 0;
        lastRateUpdateT = time;
        float rateFrac = rateT / rate - 0.8;
        rateFrac /= 0.2;
        rateFrac = std::max(0.0f, std::min(1.0f, rateFrac));
        pDrop = 0.8*pDrop + 0.2*rateFrac;
        pDrop = std::min(1.0f, std::max(0.0f, pDrop));
        prevRateFrac = rateFrac;
        if (false && pDrop > 0)
          cerr << pDrop << endl;;
    }
}
