#include "ScreamRx.h"
#include "ScreamTx.h"
#include <string.h>
#include <climits>
#include <iostream>
using namespace std;

const int kRtcpIntervalPackets = 5;
const int kRtcpIntervalMin_us = 20000;
const int kRtcpIntervalMax_us = 200000;

ScreamRx::Stream::Stream(uint32_t ssrc_) {
	ssrc = ssrc_;
	ackVector = 0;
	receiveTimestamp = 0x0;
	highestSeqNr = 0x0;
	lastFeedbackT_us = 0;
	nRtpSinceLastRtcp = 0;  
}

void ScreamRx::Stream::receive(uint64_t time_us,
	void* rtpPacket,
	int size,
	uint16_t seqNr) {
	nRtpSinceLastRtcp++;

	/*
	* Make things wrap-around safe
	*/
	uint32_t seqNrExt = seqNr;
	uint32_t highestSeqNrExt = highestSeqNr;
	if (seqNrExt < highestSeqNrExt && highestSeqNrExt - seqNrExt > 20000)
		seqNrExt += 65536;
	else if (seqNrExt > highestSeqNrExt && seqNrExt - highestSeqNrExt > 20000)
		highestSeqNrExt += 65536;

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
		uint16_t diff = seqNrExt - highestSeqNrExt;
		if (diff != 0) {
			if (diff >= kAckVectorBits) {
				ackVector = 0x0000;
			}
			else {
				// Fill with potential zeros
				ackVector = ackVector << diff;
				// Add previous highest seq nr to ack vector
				ackVector |= (1i64 << (diff - 1));
			}
		}
		highestSeqNr = seqNr;
	}
	else {
		/*
		* Out-of-order reception
		*/
		uint16_t diff = highestSeqNrExt - seqNrExt;
		if (diff < kAckVectorBits) {
			ackVector = ackVector | (1i64 << (diff - 1));
		}
	}

	receiveTimestamp = (uint32_t)(time_us / kTimestampRate);
}

ScreamRx::ScreamRx() {
	lastFeedbackT_us = 0;
}

void ScreamRx::receive(uint64_t time_us,
	void* rtpPacket,
	uint32_t ssrc,
	int size,
	uint16_t seqNr) {

	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			if ((*it)->isMatch(ssrc)) {
				/*
				* Packets for this SSRC received earlier
				* stream is thus already in list
				*/
				(*it)->receive(time_us, rtpPacket, size, seqNr);
				return;

			}

		}

	}
	/*
	* New {SSRC,PT}
	*/
	Stream *stream = new Stream(ssrc);
	stream->receive(time_us, rtpPacket, size, seqNr);
	streams.push_back(stream);
}

bool ScreamRx::isFeedback(uint64_t time_us) {
	if (!streams.empty()) {
		for (auto it = streams.begin(); it != streams.end(); ++it) {
			Stream *stream = (*it);
			uint64_t delta = time_us - stream->lastFeedbackT_us;
			if (stream->nRtpSinceLastRtcp >= kRtcpIntervalPackets && delta > kRtcpIntervalMin_us || delta > kRtcpIntervalMax_us) {
				return true;
			}
		}
	}
	return false;
}

bool ScreamRx::getFeedback(uint64_t time_us,
	uint32_t &ssrc,
	uint32_t &receiveTimestamp,
	uint16_t &highestSeqNr,
	uint64_t &ackVector) {


	Stream *stream = NULL;
	uint64_t minT_us = ULONG_MAX;
	for (auto it = streams.begin(); it != streams.end(); ++it) {
		if ((*it)->nRtpSinceLastRtcp > 0 && (*it)->lastFeedbackT_us < minT_us) {
			stream = *it;
			minT_us = (*it)->lastFeedbackT_us;

		}
	}

	if (stream == NULL)
		return false;

	receiveTimestamp = stream->receiveTimestamp;
	highestSeqNr = stream->highestSeqNr;
	ssrc = stream->ssrc;
	ackVector = stream->ackVector;

	//cerr << (time_us - stream->lastFeedbackT_us) / 1e3 << " " << stream->nRtpSinceLastRtcp << endl;
	stream->lastFeedbackT_us = time_us;
	stream->nRtpSinceLastRtcp = 0;
	lastFeedbackT_us = time_us;


	return true;
}
