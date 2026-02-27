#include "ScreamRx.h"
#ifdef _MSC_VER
#define NOMINMAX
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#endif
#include <string.h>
#include <climits>
#include <algorithm>
#include <iostream>
using namespace std;

static const int kMaxRtcpSize = 900;
static const float kMaxRtcpInterval = 0.04f;

#define OOO
// Time stamp scale
static const int kTimeStampAtoScale = 1024;
static const float ntp2SecScaleFactor = 1.0 / 65536;

ScreamRx::Stream::Stream(uint32_t ssrc_) {
	ssrc = ssrc_;
	receiveTimestamp = 0x0;
	highestSeqNr = 0x0;
	highestSeqNrTx = 0x0;
	oooLowSeqNr = 0x0;
	numOooDetected = 0;

	lastFeedbackT_ntp = 0;
	nRtpSinceLastRtcp = 0;
	firstReceived = false;
	doFlush = false;
	//ix = 0;

	for (int n = 0; n < kRxHistorySize; n++) {
		ceBitsHist[n] = 0x00;
		rxTimeHist[n] = 0;
		seqNrHist[n] = 0x0000;
		isOooHist[n] = false;
	}

}

bool ScreamRx::Stream::checkIfFlushAck(uint32_t ackDiff) {
	uint32_t diff = highestSeqNr - highestSeqNrTx;
	return (diff >= ackDiff || doFlush);
}

void ScreamRx::Stream::receive(uint32_t time_ntp,
	void* rtpPacket,
	int size,
	uint16_t seqNr,
	bool isEcnCe,
	uint8_t ceBits_,
	bool isMarker,
	uint32_t timeStamp) {

	/*
	* Count received RTP packets since last RTCP transmitted for this SSRC
	*/
	nRtpSinceLastRtcp++;

	doFlush |= isMarker;

	/*
	* Initialize on first received packet
	*/
	if (firstReceived == false) {
		highestSeqNr = seqNr;
		highestSeqNr--;
		for (int n = 0; n < kRxHistorySize; n++) {
			// Initialize seqNr list properly
			seqNrHist[n] = seqNr + 1;
		}
		firstReceived = true;
	}
	/*
	* Update CE bits and RX time vectors
	*/
	uint16_t ix = seqNr % kRxHistorySize;
	ceBitsHist[ix] = ceBits_;
	rxTimeHist[ix] = time_ntp;
	seqNrHist[ix] = seqNr;


#ifdef OOO	
	uint16_t diff = highestSeqNr - seqNr;
	/*
	* We tag a packet as OOO as soon at it is behind the highest ACKed packet
	*/
	if (diff > 0 && diff < (kRxHistorySize-kReportedRtpPackets)) {
		/*
		* Large OOO RTP received, enable transmission of additional RTCP packet to indicate receiption
		*/ 		
		if (numOooDetected > 0) {
			/*
			* More OOO packets detected
			*/
            uint16_t diff2 = seqNr - oooLowSeqNr;
			if (diff2 > 60000u) {
				oooLowSeqNr = seqNr;
			} 
			
			numOooDetected++; 
		}
		else {
			oooLowSeqNr = seqNr;
			
			numOooDetected++;
		}
		isOooHist[ix] = true;

	}
#endif

	uint32_t seqNrExt = seqNr;
	uint32_t highestSeqNrExt = highestSeqNr;
	if (seqNr < highestSeqNr) {
		if (highestSeqNr - seqNr > 16384)
			seqNrExt += 65536;
	}
	if (highestSeqNr < seqNr) {
		if (seqNr - highestSeqNr > 16384)
			highestSeqNrExt += 65536;
	}

	/*
	*/
	if (seqNrExt >= highestSeqNrExt) {
		/*
		* Normal in-order reception
		*/
		highestSeqNr = seqNr;
	}
}

bool ScreamRx::Stream::getStandardizedFeedback(uint32_t time_ntp,
	unsigned char* buf,
	int& size) {
	uint16_t tmp_s;
	uint32_t tmp_l;
	size = 0;
	/*
	* Write RTP sender SSRC
	*/
	tmp_l = htonl(ssrc);
	memcpy(buf, &tmp_l, 4);
	size += 4;

	/*
	* Write 16bits report element for received RTP packets
	*/
	uint16_t sn_lo = highestSeqNr - uint16_t(nReportedRtpPackets - 1);

	if (highestSeqNr - highestSeqNrTx  > nReportedRtpPackets) {
		/*
		* The highest sequence number can jump > nReportedRtpPackets, for instance 
		* because of a long stretch of OOO packets. We mark the packets that are outside 
		* normal reporting range as out of order so that they can be reported later. 
		*/
		uint16_t numLeftover = highestSeqNr - highestSeqNrTx - nReportedRtpPackets;
		sn_lo -= numLeftover;
		
		highestSeqNrTx = sn_lo + uint16_t(nReportedRtpPackets-1);
	} else {
	   	highestSeqNrTx = highestSeqNr;
	}   	

	/*
	* Write begin_seq
	* always report nReportedRtpPackets RTP packets
	*/
	tmp_s = highestSeqNrTx - uint16_t(nReportedRtpPackets - 1);
	tmp_s = htons(tmp_s);
	memcpy(buf + 4, &tmp_s, 2);
	size += 2;

	/*
	* Write number of reports
	*/
	tmp_s = nReportedRtpPackets;
	tmp_s = htons(tmp_s);
	memcpy(buf + 6, &tmp_s, 2);
	size += 2;
	int ptr = 8;


	for (uint16_t k = 0; k < nReportedRtpPackets; k++) {
		uint16_t sn = sn_lo + k;		
		uint16_t ix = sn % kRxHistorySize;
		uint32_t ato = (time_ntp - rxTimeHist[ix]);
		ato = ato >> 6; // Q16->Q10
		if (ato > 8189)
			ato = 0x1FFE;
		tmp_s = 0x0000;
		if (seqNrHist[ix] == sn && rxTimeHist[ix] != 0) {
			tmp_s = 0x8000 | ((ceBitsHist[ix] & 0x03) << 13) | (ato & 0x01FFF);;
			if (isOooHist[ix]) {
				/*
				* Clear OOO flag if set att decrement number of OOO packets as the 
				* particular OOO packet is indeed ACKed here
				*/
				isOooHist[ix] = false;
				numOooDetected = std::max(0, numOooDetected - 1);
			}
		}

		tmp_s = htons(tmp_s);
		memcpy(buf + ptr, &tmp_s, 2);
		size += 2;
		ptr += 2;
	}
	/*
	* Zero pad with two extra octets if the number of reported packets is odd
	*/
	if (nReportedRtpPackets % 2 == 1) {
		tmp_s = 0x0000;
		memcpy(buf + ptr, &tmp_s, 2);
		size += 2;
		ptr += 2;
	}
	
	doFlush = false;
	if (highestSeqNrTx != highestSeqNr) {
		doFlush = true;
	}

	return true;
}

bool ScreamRx::Stream::getStandardizedFeedbackOoo(uint32_t time_ntp,
	unsigned char* buf,
	int& size) {
	uint16_t tmp_s;
	uint32_t tmp_l;
	size = 0;


	if (numOooDetected > 0) {
		int numOooReported = 1;


		/*
		* Write RTP sender SSRC
		*/
		tmp_l = htonl(ssrc);
		memcpy(buf, &tmp_l, 4);
		size += 4;
		/*
		* Write begin_seq
		* always report kReportedOooRtpPackets RTP packets
		*/
		tmp_s = oooLowSeqNr;
		tmp_s = htons(tmp_s);
		memcpy(buf + 4, &tmp_s, 2);
		size += 2;
		/*
		* Determine span of reported RTP packets, 
		* limit to no longer than kReportedRtpPackets
		*/
		uint16_t oooLowSeqNrUpdated = oooLowSeqNr;
		int nReportedPackets = 1;
		for (uint16_t n = 1; n < kReportedRtpPackets; n++) {
			uint16_t ix = (oooLowSeqNr + n) % kRxHistorySize;
			if (isOooHist[ix]) {
				oooLowSeqNrUpdated = seqNrHist[ix];
				nReportedPackets = n + 1;
				numOooReported++;
			}
		}

		if (numOooReported < numOooDetected) {
			/*
			* We're not able to report all OOO packets, so we need to move forward 
			* oooLowSeqNr to the next OOO packet
			*/
			uint16_t n = kReportedRtpPackets-1;
			uint16_t ix=0;
			do  {
				n++;
				ix = (oooLowSeqNr + n) % kRxHistorySize;
				if (seqNrHist[ix] == highestSeqNr) {
					/*
					* We are at the highest received sequence number and have not found any additional OOO packet
					*  because it has already been ACKed
					*/
					numOooDetected = numOooReported;
					continue;
				}
			} while (!isOooHist[ix]);

			oooLowSeqNrUpdated = seqNrHist[ix];
		}


		/*
		* Write number of reports- 1
		*/
		tmp_s = nReportedPackets - 1;
		tmp_s = htons(tmp_s);
		memcpy(buf + 6, &tmp_s, 2);
		size += 2;
		int ptr = 8;


		/*
		* Write 16bits report element for received RTP packets
		*/
		uint16_t sn_lo = oooLowSeqNr;

		for (uint16_t k = 0; k < nReportedPackets; k++) { // may be oooHighSeqNr-oooLowSeqNr+1 instead
			uint16_t sn = sn_lo + k;

			uint16_t ix = sn % kRxHistorySize;
			uint32_t ato = (time_ntp - rxTimeHist[ix]);
			ato = ato >> 6; // Q16->Q10
			if (ato > 8189)
				ato = 0x1FFE;

			tmp_s = 0x0000;
			if (seqNrHist[ix] == sn && rxTimeHist[ix] != 0) {
				tmp_s = 0x8000 | ((ceBitsHist[ix] & 0x03) << 13) | (ato & 0x01FFF);;
			}

			tmp_s = htons(tmp_s);
			memcpy(buf + ptr, &tmp_s, 2);
			size += 2;
			ptr += 2;
			isOooHist[ix] = false;

		}

		/*
		* Zero pad with two extra octets if the number of reported packets is odd
		*/
		if (nReportedPackets % 2 == 1) {
			tmp_s = 0x0000;
			memcpy(buf + ptr, &tmp_s, 2);
			size += 2;
			ptr += 2;
		}
		numOooDetected = std::max(0,numOooDetected-numOooReported);
		oooLowSeqNr = oooLowSeqNrUpdated;
		return true;
	}
	else {
		return false;
	}
}


ScreamRx::ScreamRx(uint32_t ssrc_, int ackDiff_, int nReportedRtpPackets_) {
	lastFeedbackT_ntp = 0;
	bytesReceived = 0;
	lastRateComputeT_ntp = 0;
	averageReceivedRate = 1e5;
	rtcpFbInterval_ntp = 13107; // 20ms in NTP domain
	ssrc = ssrc_;
	//ix = 0;
	nReportedRtpPackets = nReportedRtpPackets_;
	if (ackDiff_ > 0)
		ackDiff = ackDiff_;
	else
		ackDiff = std::max(1, nReportedRtpPackets / 2);

}

ScreamRx::~ScreamRx() {
	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			delete (*it);
		}
	}
}

bool ScreamRx::checkIfFlushAck() {
	if (ackDiff == 1)
		return true;
	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			if ((*it)->checkIfFlushAck(ackDiff))
				return true;
		}
	}
	return false;
}


bool ScreamRx::isOooDetected() {
	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			if ((*it)->numOooDetected > 0)
				return true;
		}
	}
	return false;
}


void ScreamRx::receive(uint32_t time_ntp,
	void* rtpPacket,
	uint32_t ssrc,
	int size,
	uint16_t seqNr,
	uint8_t ceBits,
	bool isMark,
	uint32_t timeStamp) {

	bytesReceived += size;
	if (lastRateComputeT_ntp == 0)
		lastRateComputeT_ntp = time_ntp;
	if (time_ntp - lastRateComputeT_ntp > 6554) { // 100ms in NTP domain
		/*
		* Media rate computation (for all medias) is done at least every 100ms
		* This is used for RTCP feedback rate calculation
		*/
		float delta = (time_ntp - lastRateComputeT_ntp) * ntp2SecScaleFactor;
		lastRateComputeT_ntp = time_ntp;
		averageReceivedRate = std::max(0.95f * averageReceivedRate, bytesReceived * 8 / delta);
		bytesReceived = 0;

		rtcpFbInterval_ntp = uint32_t(65536.0f*kMaxRtcpInterval); // Convert to NTP domain (Q16)
	}

	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			if ((*it)->isMatch(ssrc)) {
				/*
				* Packets for this SSRC received earlier
				* stream is thus already in list
				*/
				(*it)->receive(time_ntp, rtpPacket, size, seqNr, ceBits == 0x03, ceBits, isMark, timeStamp);
				return;

			}
		}
	}
	/*
	* New {SSRC,PT}
	*/
	Stream* stream = new Stream(ssrc);
	stream->nReportedRtpPackets = nReportedRtpPackets;
	stream->receive(time_ntp, rtpPacket, size, seqNr, ceBits == 0x03, ceBits, isMark, timeStamp);
	streams.push_back(stream);
}

uint32_t ScreamRx::getRtcpFbInterval() {
	return rtcpFbInterval_ntp;
}

bool ScreamRx::isFeedback(uint32_t time_ntp) {
	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			Stream* stream = (*it);
			if (stream->nRtpSinceLastRtcp >= 1) {
				return true;
			}
		}
	}
	return false;
}

bool ScreamRx::createStandardizedFeedback(uint32_t time_ntp, bool isMark, unsigned char* buf, int& size) {

	uint16_t tmp_s;
	uint32_t tmp_l;
	buf[0] = 0x8B; // FMT = CCFB
	buf[1] = 205;
	/*
	* Write RTCP sender SSRC
	*/
	tmp_l = htonl(ssrc);
	memcpy(buf + 4, &tmp_l, 4);
	size = 8;
	int ptr = 8;
	bool isFeedback = false;
	/*
	* Generate RTCP feedback size until a safe sizelimit ~kMaxRtcpSize+128 byte is reached
	*/
	while (size < kMaxRtcpSize) {
		/*
		* TODO, we do the above stream fetching over again even though we have the
		* stream in the first iteration, a bit unnecessary.
		*/
		Stream* stream = NULL;
		uint64_t minT_ntp = ULONG_MAX;
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			uint32_t diffT_ntp = time_ntp - (*it)->lastFeedbackT_ntp;
			int nRtpSinceLastRtcp = (*it)->nRtpSinceLastRtcp;
			if ((nRtpSinceLastRtcp >= std::min(kReportedRtpPackets/2, ackDiff) || 
				(*it)->doFlush || 
				diffT_ntp > rtcpFbInterval_ntp || 
				(isMark && nRtpSinceLastRtcp > 0)) &&
				(*it)->lastFeedbackT_ntp < minT_ntp) {
				stream = *it;
				minT_ntp = (*it)->lastFeedbackT_ntp;
				(*it)->doFlush = false;
			}
		}
		if (stream == NULL)
			break;
		isFeedback = true;
		int size_stream = 0;
		stream->getStandardizedFeedback(time_ntp, &buf[ptr], size_stream);
		size += size_stream;
		ptr += size_stream;
		stream->lastFeedbackT_ntp = time_ntp;
		stream->nRtpSinceLastRtcp = 0;
	}
	if (!isFeedback)
		return false;
	/*
	* Write report timestamp
	*/
	tmp_l = htonl(time_ntp);
	memcpy(buf + ptr, &tmp_l, 4);
	size += 4;

	/*
	* write length
	*/
	uint16_t length = size / 4 - 1;
	tmp_s = htons(length);
	memcpy(buf + 2, &tmp_s, 2);

	/*
	* Update stream RTCP feedback status
	*/
	lastFeedbackT_ntp = time_ntp;

	return true;
}

bool ScreamRx::createStandardizedFeedbackOoo(uint32_t time_ntp, bool isMark, unsigned char* buf, int& size) {

	uint16_t tmp_s;
	uint32_t tmp_l;
	buf[0] = 0x8B; // FMT = CCFB
	buf[1] = 205;
	/*
	* Write RTCP sender SSRC
	*/
	tmp_l = htonl(ssrc);
	memcpy(buf + 4, &tmp_l, 4);
	size = 8;
	int ptr = 8;
	bool isFeedback = false;
	/*
	* Generate RTCP feedback size until a safe sizelimit ~kMaxRtcpSize+128 byte is reached
	*/
	while (size < kMaxRtcpSize) {
		/*
		* TODO, we do the above stream fetching over again even though we have the
		* stream in the first iteration, a bit unnecessary.
		*/
		Stream* stream = NULL;
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			stream = *it;
			if (stream->numOooDetected > 0) {
				continue;
			} else {
				stream = NULL;
			}

		}
		if (stream == NULL)
			break;
		isFeedback = true;
		int size_stream = 0;
		stream->getStandardizedFeedbackOoo(time_ntp, &buf[ptr], size_stream);
		size += size_stream;
		ptr += size_stream;
	}
	if (!isFeedback)
		return false;
	/*
	* Write report timestamp
	*/
	tmp_l = htonl(time_ntp);
	memcpy(buf + ptr, &tmp_l, 4);
	size += 4;

	/*
	* write length
	*/
	uint16_t length = size / 4 - 1;
	tmp_s = htons(length);
	memcpy(buf + 2, &tmp_s, 2);

	/*
	* Update stream RTCP feedback status
	*/
	lastFeedbackT_ntp = time_ntp;

	return true;
}
