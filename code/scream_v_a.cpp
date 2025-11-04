// Simple test application for verification of SCReAM function
//

#include "stdafx.h"
//#define NO_LOG_TAG


#include "VideoEnc.h"
#include "RtpQueue.h"
#include "NetQueue.h"
#include "OooQueue.h"
#include "ScreamRx.h"

using namespace std;


#ifdef NO_LOG_TAG
const char* log_tag = "scream_lib";
#else
const char* log_tag = "";
#endif

const float Tmax = 5;
const bool isChRate = false;
const bool printLog = false;
const bool ecnCapable = false;
const bool isL4s = true && ecnCapable;
const float FR = 50.0f; // Frame rate for stream 0
const int FR_DIV = 1;   // Divisor for framerate for streams 1...N
const bool isNewCc = true;
const bool enablePacing = true;

int swprio = -1;
//#define TRACEFILE "../traces/trace_key.txt"
//#define TRACEFILE "../traces/trace_no_key.txt"
#define TRACEFILE "../traces/trace_flat.txt"
/*
* Mode determines how many streams should be run
* 0x1 = stream 0, 0x2 = stream 1, 0x3 = 1+2
*/
const int mode = 0x1;// 0x0F;

const float RTT = 0.025f;

//int mssList[5] = { 300, 500, 800, 1000, 1300 };
//int nMssListItems = 5;
int mssList[5] = { 1000 };
int nMssListItems = 1;

#include "ScreamTx.h"
int main(int argc, char* argv[])
{

	int tick = (int)(65536.0f / FR);
	ScreamV2Tx* screamTx = new ScreamV2Tx(0.7f, 0.8f, 0.06f, 10000, 1.5f, 1.5f, 2.0f, 0.05f, isL4s, 5.0f, false, false);

	screamTx->setCwndMinLow(2000);
	screamTx->enablePacketPacing(enablePacing);
	screamTx->enableRelaxedPacing(true);
	screamTx->setMssListMinPacketsInFlight(mssList, nMssListItems, 5);
	//screamTx->autoTuneMinCwnd(true);
	//screamTx->setMaxTotalBitrate(40e6);
	screamTx->setLogTag((char*)log_tag);
	screamTx->isEnableAdaptiveWindowHeadroom(true);

	FILE* fp = fopen("log.txt", "w");
	screamTx->setDetailedLogFp(fp);


	ScreamRx* screamRx = new ScreamRx(0, -1);
	RtpQueue* rtpQueue[4] = { new RtpQueue(), new RtpQueue(), new RtpQueue() , new RtpQueue() };
	VideoEnc* videoEnc[4] = { 0, 0, 0, 0 };
	NetQueue* netQueueDelay = new NetQueue(RTT, 0.0f, 0.0f);
	NetQueue* netQueueRate = new NetQueue(0.0f, 20e6, 0.0f, true && isL4s);
	OooQueue* oooQueue = new OooQueue(0.0f);
	videoEnc[0] = new VideoEnc(rtpQueue[0], FR, (char*)TRACEFILE, 0, 0.0);
	videoEnc[1] = new VideoEnc(rtpQueue[1], FR / FR_DIV, (char*)TRACEFILE, 50);
	videoEnc[2] = new VideoEnc(rtpQueue[2], FR / FR_DIV, (char*)TRACEFILE, 100);
	videoEnc[3] = new VideoEnc(rtpQueue[3], FR / FR_DIV, (char*)TRACEFILE, 150);
	if (mode & 0x01)
		//screamTx->registerNewStream(rtpQueue[0], 10, 1.0f, 1e6f, 1e6f, 10e6f, 0.1f, false, 0.05f);
		screamTx->registerNewStream(rtpQueue[0], 10, 1.0f, 0.1e6f, 0.5e6f, 2e6f, 0.1f, false, 0.0f, false);
	if (mode & 0x02)
		screamTx->registerNewStream(rtpQueue[1], 11, 0.1f, 1.0e6f, 5e6f, 50e6f, 0.1f, false, 0.1f);
	if (mode & 0x04)
		screamTx->registerNewStream(rtpQueue[2], 12, 0.3f, 1.0e6f, 5e6f, 50e6f, 0.1f, false, 0.1f);
	if (mode & 0x08)
		screamTx->registerNewStream(rtpQueue[3], 13, 0.2f, 0.8e6f, 5e6f, 30e6f, 0.1f, false, 0.1f);

	float time = 0.0f;
	uint32_t time_ntp = 0;
	uint32_t time_ntp_rx = 0;
	uint32_t time_ntp_rx_plus = 0;
	int n = 0;
	uint32_t ssrc;
	char rtpPacket[2000];
	int size;
	uint16_t seqNr;
	uint32_t timeStamp;
	int nextCallN = -1;
	bool isFeedback = false;
	double lastLogT = -1.0;
	time = 0;
	cerr << "Start!" << endl;
	while (time <= Tmax) {
		char s[100];
		sprintf(s, "%3.4f", time);
		screamTx->setTimeString(s);

		time_ntp = n + 0;
		time_ntp_rx = n + time_ntp_rx_plus + 0*n/16384;
		float retVal = -1.0;
		time = n / 65536.0f;
		if (n % 65536 == 0) {
		//	cerr << time << endl;
		}
		if (time > 20.0) {
			//time_ntp_rx_plus = 0.03 * 65536;
		}

		netQueueRate->updateRate(time);
		bool isEvent = false;

		bool isFrame = false;
		int recommendedMss = screamTx->getRecommendedMss(time_ntp);
		if (n % tick == 0) {
			//cerr << time << " " << screamTx->getStatisticsItem(LOSS_RATE) << " " << screamTx->getStatisticsItem(LOSS_RATE_LONG) << " " << screamTx->getStatisticsItem(CE_RATE) << " " << screamTx->getStatisticsItem(CE_RATE_LONG) << " " << screamTx->getStatisticsItem(AVG_RTT) << " " << screamTx->getStatisticsItem(AVG_QUEUE_DELAY) << endl;
			// "Encode" audio + video frame
			if (mode & 0x01) {
				//cerr << recommendedMss << endl;
				float br = screamTx->getTargetBitrate(time_ntp, 10);
				//if (time > 15 && time < 20)
				//	br /= 4;
				videoEnc[0]->setMss(recommendedMss);
				videoEnc[0]->setTargetBitrate(br);
				int bytes = videoEnc[0]->encode(time);
				screamTx->newMediaFrame(time_ntp, 10, bytes, true);
				isFrame = true;
			}
		}
		if (n % (tick * FR_DIV) == 0) {
			if (mode & 0x02) {
				videoEnc[1]->setMss(recommendedMss);
				videoEnc[1]->setTargetBitrate(screamTx->getTargetBitrate(time_ntp, 11));
				int bytes = videoEnc[1]->encode(time);
				screamTx->newMediaFrame(time_ntp, 11, bytes, true);
				isFrame = true;
			}
			if (mode & 0x04) {
				videoEnc[2]->setMss(recommendedMss);
				videoEnc[2]->setTargetBitrate(screamTx->getTargetBitrate(time_ntp, 12));
				int bytes = videoEnc[2]->encode(time);
				screamTx->newMediaFrame(time_ntp, 12, bytes, true);
				isFrame = true;
			}
			if (mode & 0x08) {
				videoEnc[3]->setMss(recommendedMss);
				videoEnc[3]->setTargetBitrate(screamTx->getTargetBitrate(time_ntp, 13));
				int bytes = videoEnc[3]->encode(time);
				screamTx->newMediaFrame(time_ntp, 13, bytes, true);
				isFrame = true;
			}
		}
		if (isFrame) {
			/*
			* New RTP packets added, try if OK to transmit
			*/
			retVal = screamTx->isOkToTransmit(time_ntp, ssrc);
			if (retVal > 0) {
				nextCallN = n + max(1, (int)(65536.0f * retVal));
				isEvent = true;
			}
		}


		bool isCe = false;
		bool isMark = false;
		int seqNrTx = 0, seqNrRx = 0;
		if (netQueueDelay->extract(time, rtpPacket, ssrc, size, seqNr, isCe, isMark, timeStamp)) {
			netQueueRate->insert(time, rtpPacket, ssrc, size, seqNr, isCe, isMark, timeStamp);
			seqNrTx = seqNr;
		}

		if (true) {
			netQueueRate->addBytes(time);
			while (netQueueRate->canExtract()) {
				if (netQueueRate->extract(time, rtpPacket, ssrc, size, seqNr, isCe, isMark, timeStamp)) {


					if (!oooQueue->insert(time, rtpPacket, ssrc, size, seqNr, isCe, isMark, timeStamp)) {
						if (seqNr == 22821 || seqNr == 22822) {
							//cerr << " INS 2  " << seqNr << endl;
						}
						uint8_t ceBits = 0x00;
						if (ecnCapable) {
							if (isL4s)
								ceBits = 0x01;
							else
								ceBits = 0x02;
							if (isCe || (rand() % 1000) < 0) ceBits = 0x03;
						}
						screamRx->receive(time_ntp_rx, 0, ssrc, size, seqNr, ceBits, isMark, timeStamp);
						seqNrRx = seqNr;
					}
				}
		    }
			while (oooQueue->extract(time, rtpPacket, ssrc, size, seqNr, isCe, isMark, timeStamp)) {
				if (seqNr == 22821 || seqNr == 22822) {
					//cerr << " EXT 2  " << seqNr << endl;
				}
				uint8_t ceBits = 0x00;
				if (ecnCapable) {
					if (isL4s)
						ceBits = 0x01;
					else
						ceBits = 0x02;
					if (isCe || (rand() % 1000) < 0) ceBits = 0x03;
				}
				screamRx->receive(time_ntp_rx, 0, ssrc, size, seqNr, ceBits, isMark, timeStamp);
				seqNrRx = seqNr;
			}
		}

		unsigned char buf[2000];
		int fb_size = -1;
		if (true) {
			bool isFeedback = screamRx->isFeedback(time_ntp_rx) &&
				(time_ntp_rx - screamRx->getLastFeedbackT() > screamRx->getRtcpFbInterval() || screamRx->checkIfFlushAck());

			if (isFeedback && screamRx->createStandardizedFeedback(time_ntp_rx, false, buf, fb_size)) {
				screamTx->incomingStandardizedFeedback(time_ntp, buf, fb_size);
				while (screamRx->isOooDetected()) {
					/*
					* OOO RTP detected, additional "transmission" of RTCP
					*/
					screamRx->createStandardizedFeedbackOoo(time_ntp_rx, false, buf, fb_size);
					screamTx->incomingStandardizedFeedback(time_ntp, buf, fb_size);
				}
				retVal = screamTx->isOkToTransmit(time_ntp, ssrc);
				isEvent = true;
			}
		}

		if (n == nextCallN && retVal != 0.0f) {
			retVal = screamTx->isOkToTransmit(time_ntp, ssrc);
			if (retVal > 0) {
				nextCallN = n + max(1, (int)(65536.0f * retVal));
				isEvent = true;
			}
		}

		if (retVal == 0) {
			/*
			* RTP packet can be transmitted
			*/
			void** rtpPacket = 0;
			uint32_t ssrc_tmp;
			float rtpQueueDelay = 0.0f;
			switch (ssrc) {
			case 10:
				rtpQueueDelay = rtpQueue[0]->getDelay(time);
				rtpQueue[0]->sendPacket(rtpPacket, size, ssrc_tmp, seqNr, isMark, timeStamp);
				break;
			case 11:
				rtpQueueDelay = rtpQueue[1]->getDelay(time);
				rtpQueue[1]->sendPacket(rtpPacket, size, ssrc_tmp, seqNr, isMark, timeStamp);
				break;
			case 12:
				rtpQueueDelay = rtpQueue[2]->getDelay(time);
				rtpQueue[2]->sendPacket(rtpPacket, size, ssrc_tmp, seqNr, isMark, timeStamp);
				break;
			case 13:
				rtpQueueDelay = rtpQueue[3]->getDelay(time);
				rtpQueue[3]->sendPacket(rtpPacket, size, ssrc_tmp, seqNr, isMark, timeStamp);
				break;
			}
			netQueueDelay->insert(time, rtpPacket, ssrc, size, seqNr, false, isMark, timeStamp);
			retVal = screamTx->addTransmitted(time_ntp, ssrc, size, seqNr, isMark, rtpQueueDelay, timeStamp);
			nextCallN = n + max(1, (int)(65536.0f * retVal));
			isEvent = true;
		}

		if (true && printLog && time - lastLogT > 0.05) {
			cout << time << " ";
			char s[500];
			screamTx->getShortLog(time, s);
			//cout << endl;
			cout << " " << s << endl;
			lastLogT = time;
		}





		if (isChRate) {
			if ((time > 10.0 && time < 20) && isChRate) {
				netQueueRate->rate = 600e3;
			}
			else {
				netQueueRate->rate = 1000e3;
			}
		}





		if (time > 20 && swprio == 0) {
			swprio = 1;
			screamTx->setTargetPriority(10, 0.2);
			screamTx->setTargetPriority(11, 1.0);
		}
		if (time > 25 && swprio == 1) {
			swprio = 2;
			screamTx->setTargetPriority(10, 1.0);
			screamTx->setTargetPriority(11, 0.2);
		}


		n++;
#ifdef _WIN32
		Sleep(0);
#else
		sleep(0);
#endif
	}

    fclose(fp);
	return 0;
}

