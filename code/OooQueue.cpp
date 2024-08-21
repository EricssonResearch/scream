#include "OooQueue.h"
#include <iostream>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

using namespace std;
/*
 * Implements a simple RTP packet queue
 */

OooQueueItem::OooQueueItem() {
	packet = 0;
	used = false;
	size = 0;
	seqNr = 0;
	tRelease = 0.0;
    tQueue = 0.0;
}

OooQueue::OooQueue(float maxDelay_) {
	for (int n=0; n < OooQueueSize; n++) {
		items[n] = new OooQueueItem();
	}
	item = new OooQueueItem();
	nItems = 0;
	maxDelay = maxDelay_;
	prevTRelease = 0;
	nDelayed;
}

bool OooQueue::insert(float time,
	void* rtpPacket,
	unsigned int ssrc,
	int size,
	unsigned short seqNr,
	bool isCe,
	bool isMark, 
	unsigned int timeStamp) {

	float delay = maxDelay; //rand()* maxDelay / RAND_MAX;
	//if (delay > 0.0001f) {
	if (maxDelay > 0.0f && (seqNr % 2048 < 20) && seqNr > 100 && seqNr % 1 == 0) {//}&& seqNr < 3000) {
/*
		* put in queue to apply OOO delay
		*/
		int ix = -1;
		for (int n = 0; n < OooQueueSize; n++) {
			if (!items[n]->used) {
				ix = n;
				continue;
			}
		}
		if (ix == -1) {
			cerr << "Too many OOO packets, increase OooQueueSize" << endl;
			exit(-1);
		}
		OooQueueItem* tmp = item;
		tmp->tRelease = time;
		tmp = items[ix];
		//cerr << time << " " << seqNr << endl;
		if (false || nDelayed == 0) {
			tmp->tRelease = time + delay;
		}
		else {
			//tmp->tRelease = time + maxDelay;
			tmp->tRelease = prevTRelease - 0.0001;
		}
		prevTRelease = tmp->tRelease;
		tmp->used = true;
		tmp->packet = rtpPacket;
		tmp->ssrc = ssrc;
		tmp->size = size;
		tmp->seqNr = seqNr;
		tmp->timeStamp = timeStamp;
		tmp->isCe = isCe;
		tmp->isMark = isMark;
		tmp->tQueue = time;
		nDelayed++;
		return true;
	}
	nDelayed = 0;

	return false;
}

bool OooQueue::extract(float time,
	void* rtpPacket,
	unsigned int& ssrc,
	int& size,
	unsigned short& seqNr,
	bool& isCe,
	bool& isMark,
	unsigned int& timeStamp) {

	OooQueueItem* tmp = item;
	for (int n = 0; n < OooQueueSize; n++) {
		if (items[n]->used && items[n]->tRelease < time) {
			tmp = items[n];
		}
	}

	if (tmp->used) {
		rtpPacket = tmp->packet;
		seqNr = tmp->seqNr;
		timeStamp = tmp->timeStamp;
		ssrc = tmp->ssrc;
		size = tmp->size;
		isCe = tmp->isCe;
		isMark = tmp->isMark;
		tmp->used = false;

		return true;
	} else {
		return false;
	}

}
