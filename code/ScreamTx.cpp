#include "ScreamTx.h"
#include <cstdio>

ScreamTx::ScreamTx() {
	statistics = new Statistics(this);
}

ScreamTx::~ScreamTx() {
	delete statistics;
}

ScreamTx::Statistics::Statistics(ScreamTx *parent_) {
	parent = parent_;
	sumRateTx = 0.0f;
	sumRateLost = 0.0f;
	avgRateTx = 0.0f;
	avgRtt = 0.0f;
	avgQueueDelay = 0.0f;
	rateLostAcc = 0.0f;
	rateLostN = 0;
	for (int n = 0; n < kLossRateHistSize; n++) {
		lossRateHist[n] = 0.0f;
	}
	lossRateHistPtr = 0;
}

void ScreamTx::Statistics::add(float rateTx, float rateLost, float rtt, float queueDelay) {
	const float alpha = 0.98f;
	sumRateTx += rateTx;
	sumRateLost += rateLost;
	if (avgRateTx == 0.0f) {
		avgRateTx = rateTx;
		avgRtt = rtt;
		avgQueueDelay = queueDelay;
	}
	else {
		avgRateTx = alpha * avgRateTx + (1.0f - alpha)*rateTx;
		rateLostAcc += rateLost;
		rateLostN++;
		if (rateLostN == 10) {
			rateLostAcc /= 10;
			rateLostN = 0;
			float lossRate = 0.0f;
			if (rateTx > 0)
				lossRate = rateLostAcc / rateTx * 100.0f;
			lossRateHist[lossRateHistPtr] = lossRate;
			lossRateHistPtr = (lossRateHistPtr + 1) % kLossRateHistSize;
		}
		avgRtt = alpha * avgRtt + (1.0f - alpha)*rtt;
		avgQueueDelay = alpha * avgQueueDelay + (1.0f - alpha)*queueDelay;
	}
}

void ScreamTx::Statistics::getSummary(float time, char s[]) {
	float lossRate = 0.0f;
	for (int n = 0; n < kLossRateHistSize; n++)
		lossRate += lossRateHist[n];
	lossRate /= kLossRateHistSize;
	float lossRateLong = 0.0f;
	if (sumRateTx > 100000.0f) {
		lossRateLong = sumRateLost / sumRateTx * 100.0f;
	}
	sprintf(s, "%s summary %5.1f  Transmit rate = %5.0fkbps, PLR = %5.2f%%(%5.2f%%), RTT = %5.3fs, Queue delay = %5.3fs",
        parent->logTag,
		time,
		avgRateTx / 1000.0f,
		lossRate,
		lossRateLong,
		avgRtt,
		avgQueueDelay);
}

void ScreamTx::getStatistics(float time, char* s) {
	statistics->getSummary(time, s);
}


