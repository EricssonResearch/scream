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
  sumRateCe = 0.0f;
  avgRateTx = 0.0f;
  avgRtt = 0.0f;
  avgQueueDelay = 0.0f;
  lossRate = 0.0;
  lossRateLong = 0.0;
  ceRate = 0.0;
  ceRateLong = 0.0;
  rateLostAcc = 0.0f;
  rateCeAcc = 0.0f;
  rateLostN = 0;
  for (int n = 0; n < kLossRateHistSize; n++) {
    lossRateHist[n] = 0.0f;
    ceRateHist[n] = 0.0f;
  }
  lossRateHistPtr = 0;
}

void ScreamTx::Statistics::add(float rateTx, float rateLost, float rateCe, float rtt, float queueDelay) {
  const float alpha = 0.98f;
  sumRateTx += rateTx;
  sumRateLost += rateLost;
  sumRateCe += rateCe;
  if (avgRateTx == 0.0f) {
    avgRateTx = rateTx;
    avgRtt = rtt;
    avgQueueDelay = queueDelay;
  }
  else {
    avgRateTx = alpha * avgRateTx + (1.0f - alpha)*rateTx;
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
    avgRtt = alpha * avgRtt + (1.0f - alpha)*rtt;
    avgQueueDelay = alpha * avgQueueDelay + (1.0f - alpha)*queueDelay;
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

void ScreamTx::Statistics::getSummary(float time, char s[]) {
  sprintf(s, "%s summary %5.1f  Transmit rate = %5.0fkbps, PLR = %5.2f%%(%5.2f%%), CE = %5.2f%%(%5.2f%%), RTT = %5.3fs, Queue delay = %5.3fs",
  parent->logTag,
  time,
  avgRateTx / 1000.0f,
  lossRate,
  lossRateLong,
  ceRate,
  ceRateLong,
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
