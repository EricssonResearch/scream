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
const float l4sThHi = 0.010f;
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
	if (isL4s && rate > 0) {
		float qDelay = items[tail]->tRelease - items[tail]->tQueue;
		//cerr << time << " " << items[head]->tRelease << " " << qDelay << " " << (size * 8 / rate) << " " << rate << endl;
		float pMark = std::max(0.0f, std::min(1.0f, (qDelay - l4sThLo) / (l4sThHi - l4sThLo)));
		markCarry += pMark;
		//if (markCarry >= 1.0) {
		if ((rand() % 1000) / 1000.0 < pMark) {
			markCarry -= 1.0f;
			isCe = true;
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


#ifdef hmm
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
	items[head]->tRelease = time + delay + jitter * (rand() / float(RAND_MAX));
	items[head]->tReleaseExt = items[head]->tRelease;
	items[head]->tQueue = time;
	items[head]->isCe = isCe;
	items[head]->isMark = isMark;
}


float markCarry = 0.0f;
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
		//cerr << time << " " << nextTx << " " << items[tail]->tRelease << " " << delay << endl;
		if (time >= std::max(items[tail]->tRelease, nextTx)) {
			items[tail]->tReleaseExt = time;
			nextTx = time;
			if (rate > 0) {
				nextTx += size * 8 / rate;
				//cerr << size << " " << rate << endl;
			}
			rtpPacket = items[tail]->packet;
			seqNr = items[tail]->seqNr;
			ssrc = items[tail]->ssrc;
			size = items[tail]->size;
			isCe = items[tail]->isCe;
			isMark = items[tail]->isMark;

			items[tail]->used = false;
			float qDelay = items[tail]->tReleaseExt - items[tail]->tRelease;
			if (isL4s && rate > 0) {
				float pMark = std::max(0.0f, std::min(1.0f, (qDelay - l4sThLo) / (l4sThHi - l4sThLo)));
				markCarry += pMark;
				//if (markCarry >= 1.0) {
				if ((rand() % 1000) / 1000.0 < pMark) {
					markCarry -= 1.0f;
					isCe = true;
				}
			}


			bytesTx += size;
			tail++; if (tail == NetQueueSize) tail = 0;
			nItems = std::max(0, nItems - 1);

			return true;
		}
		return false;
	}
}
#endif

/*
void NetQueue::insert(float time, 
	                  void *rtpPacket,  
					  unsigned int ssrc,
					  int size, 
					  unsigned short seqNr,
                      bool isCe) {
	int prevHead = head;
	if (nItems > 1000)
		return;
	nItems++;
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
		float delay = size*8.0f / rate;		
		if (prevHead != -1 && items[prevHead]->used) {
			items[head]->tRelease = items[prevHead]->tRelease + delay;
		}
		else {
			items[head]->tRelease = time + delay;
		}
	}
	items[head]->tRelease = std::max(items[head]->tRelease, nextTx);
	if (isL4s && rate > 0) {
		float pMark = std::max(0.0f, std::min(1.0f, (items[head]->tRelease - time - l4sThLo) / (l4sThHi-l4sThLo)));
		if ((rand() % 1000) / 1000 < pMark)
			items[head]->isCe = true;
	}
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
            
            //Implement a rudimentary CoDel-ish ECN marker (without the 1/sqrt(N) part)
            float qDel = time - items[tail]->tQueue;
            if (!isL4s && time - items[tail]->tQueue <= 0.005 && rate > 0.0) {
                lastQueueLow = time;
            }
            if (!isL4s && time - lastQueueLow > 0.1 && rate > 0.0) {
                isCe = true;
                lastQueueLow = time;
            }
			//tQueueAvg = (time-items[tail]->tQueue);
			//if (isL4s && time - items[tail]->tQueue > l4sTh && rate > 0.0) {
			//if (isL4s && tQueueAvg > l4sTh && rate > 0.0) {
              //isCe = true;
            //}
            bytesTx += size;
            tail++; if (tail == NetQueueSize) tail = 0;
			nItems = std::max(0, nItems-1);

		  return true;
		}
		return false;
	}
}
*/

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
