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

const float l4sThLo = 0.004f;
const float l4sThHi = 0.006f;
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
//	nextTx = 0;
	sendTime = 0;
    isL4s = isL4s_;
    bytesTx = 0;
    lastRateUpdateT = 0;
    pDrop = 0.0f;
    prevRateFrac = 0.0f;
	tQueueAvg = 0.0;

}

float pMarkList[] = {0.0,0.5000,1.0000,1.5000,2.0000,5.0000,10.0000,20.0000,30.0000,40.0000,50.0000 };
float markCarry = 0.0f;
void NetQueue::insert(float time,
	void* rtpPacket,
	unsigned int ssrc,
	int size,
	unsigned short seqNr,
	bool isCe,
	bool isMark) {
	int prevHead = head;
	if (false && nItems > 1000)
		return;
	nItems++;
	head++; if (head == NetQueueSize) head = 0;
	items[head]->used = true;
	items[head]->packet = rtpPacket;
	items[head]->ssrc = ssrc;
	items[head]->size = size;
	items[head]->seqNr = seqNr;
	float tmp = 0;
	if (rate > 0)
		tmp += size * 8 / rate;
	sendTime = std::max(sendTime, time) + tmp;	
	items[head]->tRelease = sendTime + delay + jitter * (rand() / float(RAND_MAX));;
	items[head]->tQueue = time;
	if (rate > 0) {
		float qDelay = items[tail]->tRelease - items[tail]->tQueue;
		if (isL4s) {
			int ix = int(time / 60.0f);
			//float pMark = pMarkList[ix] / 100.0;
			float pMark = std::max(0.0f, std::min(1.0f, (qDelay - l4sThLo) / (l4sThHi - l4sThLo)));
			markCarry += pMark;
			//if (markCarry >= 1.0f) {
			if ((rand() % 1000) / 1000.0 < pMark) {
				markCarry -= 1.0f;
				isCe = true;
			}
		} else {
			if (qDelay > 0.03) {
				isCe = true;
			}
		}
	}


	items[head]->isCe = isCe;
	items[head]->isMark = isMark;
}

bool NetQueue::extract(float time,
	void* rtpPacket,
	unsigned int& ssrc,
	int& size,
	unsigned short& seqNr,
	bool& isCe,
	bool& isMark) {
	if (items[tail]->used == false) {
		lastQueueLow = time;
		return false;
	}
	else {
		if (time >= items[tail]->tRelease) {
		//	items[tail]->tReleaseExt = time;
			rtpPacket = items[tail]->packet;
			seqNr = items[tail]->seqNr;
			ssrc = items[tail]->ssrc;
			size = items[tail]->size;
			isCe = items[tail]->isCe;
			isMark = items[tail]->isMark;

			items[tail]->used = false;

			bytesTx += size;
			tail++; if (tail == NetQueueSize) tail = 0;
			nItems = std::max(0, nItems - 1);

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

void NetQueue::updateRate(float time) {
    if (time - lastRateUpdateT >= rateUpdateT && rate > 0) {
        float dT = time - lastRateUpdateT;
        float rateT = bytesTx * 8 / dT;
        bytesTx = 0;
        lastRateUpdateT = time;
        float rateFrac = rateT / rate - 0.9;
        rateFrac /= 0.2;
        rateFrac = std::max(0.0f, std::min(1.0f, rateFrac));
        pDrop = 0.9*pDrop + 0.1*rateFrac;
        pDrop = std::min(1.0f, std::max(0.0f, pDrop));
        prevRateFrac = rateFrac;
        if (false && pDrop > 0)
          cerr << pDrop << endl;;
    }
}
