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

// Time stamp scale
static const int kTimeStampAtoScale = 1024;
static const float ntp2SecScaleFactor = 1.0 / 65536;

ScreamRx::Stream::Stream(uint32_t ssrc_) {
	ssrc = ssrc_;
	receiveTimestamp = 0x0;
	highestSeqNr = 0x0;
	highestSeqNrTx = 0x0;
	lastFeedbackT_ntp = 0;
	nRtpSinceLastRtcp = 0;
	firstReceived = false;

	for (int n = 0; n < kRxHistorySize; n++) {
		ceBitsHist[n] = 0x00;
		rxTimeHist[n] = 0;
		seqNrHist[n] = 0x0000;
	}

}

bool ScreamRx::Stream::checkIfFlushAck(int ackDiff) {
	uint32_t diff = highestSeqNr - highestSeqNrTx;
	return (diff >= ackDiff);
}

void ScreamRx::Stream::receive(uint32_t time_ntp,
	void* rtpPacket,
	int size,
	uint16_t seqNr,
	bool isEcnCe,
	uint8_t ceBits_) {

	/*
	* Count received RTP packets since last RTCP transmitted for this SSRC
	*/
	nRtpSinceLastRtcp++;

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
	int ix = seqNr % kRxHistorySize;
	ceBitsHist[ix] = ceBits_;
	rxTimeHist[ix] = time_ntp;
	seqNrHist[ix] = seqNr;

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
	* Update the ACK vector that indicates receiption '=1' of RTP packets prior to
	* the highest received sequence number.
	* The next highest SN is indicated by the least significant bit,
	* this means that for the first received RTP, the ACK vector is
	* 0x0, for the second received RTP, the ACK vector it is 0x1, for the third 0x3 and so
	* on, provided that no packets are lost.
	* A 64 bit ACK vector means that it theory it is possible to send one feedback every
	* 64 RTP packets, while this can possibly work at low bitrates, it is less likely to work
	* at high bitrates because the ACK clocking in SCReAM is disturbed then.
	*/
	if (seqNrExt >= highestSeqNrExt) {
		/*
		* Normal in-order reception
		*/
		highestSeqNr = seqNr;
	}
}

bool ScreamRx::Stream::getStandardizedFeedback(uint32_t time_ntp,
	unsigned char *buf,
	int &size) {
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
	* Write begin_seq
	* always report nReportedRtpPackets RTP packets
	*/
	tmp_s = highestSeqNr - uint16_t(nReportedRtpPackets - 1);
	tmp_s = htons(tmp_s);
	memcpy(buf + 4, &tmp_s, 2);
	size += 2;

	/*
	* Write number of reports- 1
	*/
	tmp_s = nReportedRtpPackets - 1;
	tmp_s = htons(tmp_s);
	memcpy(buf + 6, &tmp_s, 2);
	size += 2;
	int ptr = 8;

	/*
	* Write 16bits report element for received RTP packets
	*/
	uint16_t sn_lo = highestSeqNr - uint16_t(nReportedRtpPackets - 1);

	for (uint16_t k = 0; k < nReportedRtpPackets; k++) {
		uint16_t sn = sn_lo + k;

		int ix = sn % kRxHistorySize;
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

	return true;
}

ScreamRx::ScreamRx(uint32_t ssrc_, int ackDiff_, int nReportedRtpPackets_) {
	lastFeedbackT_ntp = 0;
	bytesReceived = 0;
	lastRateComputeT_ntp = 0;
	averageReceivedRate = 1e5;
	rtcpFbInterval_ntp = 13107; // 20ms in NTP domain
	ssrc = ssrc_;
	ix = 0;
	nReportedRtpPackets = nReportedRtpPackets_;
	if (ackDiff_ > 0)
		ackDiff = ackDiff_;
	else
		ackDiff = std::max(1, nReportedRtpPackets / 4);

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

void ScreamRx::receive(uint32_t time_ntp,
	void* rtpPacket,
	uint32_t ssrc,
	int size,
	uint16_t seqNr,
	uint8_t ceBits) {

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
		averageReceivedRate = std::max(0.95f*averageReceivedRate, bytesReceived * 8 / delta);
		bytesReceived = 0;
		/*
		* The RTCP feedback rate depends on the received media date
		* Target ~2% overhead but with feedback interval limited
		*  to the range [2ms,100ms]
		*/
		float rate = 0.02f*averageReceivedRate / (100.0f * 8.0f); // RTCP overhead
		rate = std::min(500.0f, std::max(10.0f, rate));
		/*
		* More than one stream ?, increase the feedback rate as
		*  we currently don't bundle feedback packets
		*/
		//rate *= streams.size();
		rtcpFbInterval_ntp = uint32_t(65536.0f / rate); // Convert to NTP domain (Q16)
	}

	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			if ((*it)->isMatch(ssrc)) {
				/*
				* Packets for this SSRC received earlier
				* stream is thus already in list
				*/
				(*it)->receive(time_ntp, rtpPacket, size, seqNr, ceBits == 0x03, ceBits);
				return;

			}
		}
	}
	/*
	* New {SSRC,PT}
	*/
	Stream *stream = new Stream(ssrc);
	stream->nReportedRtpPackets = nReportedRtpPackets;
	stream->ix = ix++;
	stream->receive(time_ntp, rtpPacket, size, seqNr, ceBits == 0x03, ceBits);
	streams.push_back(stream);
}

uint32_t ScreamRx::getRtcpFbInterval() {
	return rtcpFbInterval_ntp;
}

bool ScreamRx::isFeedback(uint32_t time_ntp) {
	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			Stream *stream = (*it);
			if (stream->nRtpSinceLastRtcp >= 1) {
				return true;
			}
		}
	}
	return false;
}

int ScreamRx::getIx(uint32_t ssrc) {
	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			Stream *stream = (*it);
			if (ssrc == stream->ssrc)
				return stream->ix;
		}
	}
	return -1;
}

bool ScreamRx::createStandardizedFeedback(uint32_t time_ntp, bool isMark, unsigned char *buf, int &size) {

	uint16_t tmp_s;
	uint32_t tmp_l;
	buf[0] = 0x80; // TODO FMT = CCFB in 5 LSB
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
		Stream *stream = NULL;
		uint64_t minT_ntp = ULONG_MAX;
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			uint32_t diffT_ntp = time_ntp - (*it)->lastFeedbackT_ntp;
			int nRtpSinceLastRtcp = (*it)->nRtpSinceLastRtcp;
			if ((nRtpSinceLastRtcp >= std::min(8, ackDiff) || diffT_ntp > 655 || (isMark && nRtpSinceLastRtcp > 0)) &&
				(*it)->lastFeedbackT_ntp < minT_ntp) {
				stream = *it;
				minT_ntp = (*it)->lastFeedbackT_ntp;
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
		stream->highestSeqNrTx = stream->highestSeqNr;
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
