#include "ScreamTx.h"
#include <cstdio>

static const uint32_t kMinTimeForMinMaxStatistics_ntp = 5*65536;

ScreamTx::ScreamTx() {
	statistics = new Statistics(this);
}

ScreamTx::~ScreamTx() {
	delete statistics;
}

ScreamTx::Statistics::Statistics(ScreamTx* parent_) {
	parent = parent_;
	sumRateTx = 0.0f;
	sumRateLost = 0.0f;
	sumRateCe = 0.0f;
	avgRateTx = 0.0f;
	minRate = 1e9f;
	maxRate = 0.0f;
	avgRtt = 0.0f;
	sumRtt = 0.0f;
	minRtt = 10.0f;
	maxRtt = 0.0f;
	avgQueueDelay = 0.0f;
	maxQueueDelay = 0.0f;
	lossRate = 0.0f;
	lossRateLong = 0.0f;
	ceRate = 0.0f;
	ceRateLong = 0.0f;
	rateLostAcc = 0.0f;
	rateCeAcc = 0.0f;
	rateLostN = 0;
	nStatisticsItems++;
	n00 = 0;
	n10 = 0;
	n01 = 0;
	n11 = 0;
	nEcn = 0;
	for (int n = 0; n < kLossRateHistSize; n++) {
		lossRateHist[n] = 0.0f;
		ceRateHist[n] = 0.0f;
	}
	lossRateHistPtr = 0;
}

void ScreamTx::Statistics::add(uint32_t time_ntp, float rateTx, float rateLost, float rateCe, float rtt, float queueDelay) {
	nStatisticsItems++;
	const float alpha = 0.98f;
	sumRateTx += rateTx;
	sumRateLost += rateLost;
	sumRateCe += rateCe;
	sumRtt += rtt;
	minRtt = std::min(minRtt,rtt);
	maxRtt = std::max(maxRtt,rtt);
	sumQueueDelay += queueDelay;
	maxQueueDelay = std::max(maxQueueDelay, queueDelay);
	if (time_ntp > kMinTimeForMinMaxStatistics_ntp) {
		minRate = std::min(minRate, rateTx);
		maxRate = std::max(maxRate, rateTx);
	}
	if (avgRateTx == 0.0f) {
		avgRateTx = rateTx;
		avgRtt = rtt;
		avgQueueDelay = queueDelay;
	}
	else {
		avgRateTx = alpha * avgRateTx + (1.0f - alpha) * rateTx;
		rateLostAcc += rateLost;
		rateCeAcc += rateCe;
		rateLostN++;
		if (rateLostN == 10) {
			rateLostAcc /= 10;
			rateCeAcc /= 10;
			rateLostN = 0;
			float lossRate = 0.0f;
			float ceRate = 0.0f;
			if (rateTx > 0) {
				lossRate = rateLostAcc / rateTx * 100.0f;
				ceRate = rateCeAcc / rateTx * 100.0f;
			}
			lossRateHist[lossRateHistPtr] = lossRate;
			ceRateHist[lossRateHistPtr] = ceRate;
			lossRateHistPtr = (lossRateHistPtr + 1) % kLossRateHistSize;
		}
		avgRtt = alpha * avgRtt + (1.0f - alpha) * rtt;
		avgQueueDelay = alpha * avgQueueDelay + (1.0f - alpha) * queueDelay;
	}
	lossRate = 0.0f;
	ceRate = 0.0f;
	for (int n = 0; n < kLossRateHistSize; n++) {
		lossRate += lossRateHist[n];
		ceRate += ceRateHist[n];
	}
	lossRate /= kLossRateHistSize;
	ceRate /= kLossRateHistSize;
	lossRateLong = 0.0f;
	ceRateLong = 0.0f;
	if (sumRateTx > 100000.0f) {
		lossRateLong = sumRateLost / sumRateTx * 100.0f;
		ceRateLong = sumRateCe / sumRateTx * 100.0f;
	}
}

void ScreamTx::Statistics::addEcn(uint8_t ecn) {
	nEcn++;
	switch (ecn) {
	case 0x00:
		n00++;
		break;
	case 0x02:
		n10++;
		break;
	case 0x01:
		n01++;
		break;
	case 0x03:
		n11++;
		break;
	default:
		break;
	}
}

void ScreamTx::Statistics::printFinalSummary() {
	printf("\n======================== Summary ==========================\n");
	printf(" Bitrate min/max/avg [kbps]          : %5.0f/%5.0f/%5.0f\n",minRate/1000.0f,maxRate/1000.0f,
		sumRateTx/nStatisticsItems/1000.0f);
	printf(" RTT     min/max/avg [s]             : %2.3f/%2.3f/%2.3f\n",minRtt,maxRtt,sumRtt/nStatisticsItems);
	printf(" Queue delay max/avg [s]             : %2.3f/%2.3f\n",maxQueueDelay,sumQueueDelay/nStatisticsItems);
	printf(" Packet loss avg [%%]                 : %4.3f\n", lossRateLong);
	int tmp = std::max(1, nEcn);
	printf(" ECN/L4S NotECT/ECT(0)/ECT(1)/CE [%%] : %4.1f/%4.1f/%4.1f/%4.1f\n",
		100.0f * float(n00) / tmp,
		100.0f * float(n10) / tmp,
		100.0f * float(n01) / tmp,
		100.0f * float(n11) / tmp);
	printf("===========================================================\n");
}

void ScreamTx::Statistics::getSummary(float time, char s[]) {
	int tmp = std::max(1, nEcn);
	sprintf(s, "%s summary %5.1f  Transmit rate = %5.0fkbps, PLR = %5.2f%%(%5.2f%%), CE = %5.2f%%(%5.2f%%)[%4.1f%%, %4.1f%%, %4.1f%%, %4.1f%%], RTT = %5.3fs, Queue delay = %5.3fs",
		parent->logTag,
		time,
		avgRateTx / 1000.0f,
		lossRate,
		lossRateLong,
		ceRate,
		ceRateLong,
		100.0f * float(n00) / tmp,
		100.0f * float(n10) / tmp,
		100.0f * float(n01) / tmp,
		100.0f * float(n11) / tmp,
		avgRtt,
		avgQueueDelay);
}

float ScreamTx::Statistics::getStatisticsItem(StatisticsItem item) {
	switch (item) {
	case AVG_RATE:
		return avgRateTx;
		break;
	case LOSS_RATE:
		return lossRate;
		break;
	case LOSS_RATE_LONG:
		return lossRateLong;
		break;
	case CE_RATE:
		return ceRate;
		break;
	case CE_RATE_LONG:
		return ceRateLong;
		break;
	case AVG_RTT:
		return avgRtt;
		break;
	case AVG_QUEUE_DELAY:
		return avgQueueDelay;
		break;
	}
	return 0.0f;
}

void ScreamTx::getStatistics(float time, char* s) {
	statistics->getSummary(time, s);
}

float ScreamTx::getStatisticsItem(StatisticsItem item) {
	return statistics->getStatisticsItem(item);
}

void ScreamTx::printFinalSummary() {
	statistics->printFinalSummary();
}

