#include "ScreamRx.h"
#include "ScreamTx.h"
#include <string.h>
#include <climits>
#include <algorithm>
#include <iostream>
using namespace std;


ScreamRx::Stream::Stream(uint32_t ssrc_) {
    ssrc = ssrc_;
    ackVector = 0;
    receiveTimestamp = 0x0;
    highestSeqNr = 0x0;
    lastFeedbackT_us = 0;
    nRtpSinceLastRtcp = 0;
    firstReceived = false;
}

bool ScreamRx::Stream::checkIfFlushAck(int seqNr) {

    return (seqNr - highestSeqNr > kAckVectorBits / 2) && nRtpSinceLastRtcp >= 1;
}
void ScreamRx::Stream::receive(uint64_t time_us,
    void* rtpPacket,
    int size,
    uint16_t seqNr) {
    nRtpSinceLastRtcp++;
    /*
    * Make things wrap-around safe
    */
    if (firstReceived == false) {
        highestSeqNr = seqNr;
        highestSeqNr--;
        firstReceived = true;
    }

    uint32_t seqNrExt = seqNr;
    uint32_t highestSeqNrExt = highestSeqNr;
    if (seqNr < highestSeqNr) {
        if (highestSeqNr - seqNr > 1000)
            seqNrExt += 65536;
    }
    if (highestSeqNr < seqNr) {
        if (seqNr - highestSeqNr > 1000)
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
        uint16_t diff = seqNrExt - highestSeqNrExt;
        if (diff != 0) {
            if (diff >= kAckVectorBits) {
                ackVector = 0x0000;
            }
            else {
                // Fill with potential zeros
                ackVector = ackVector << diff;
                // Add previous highest seq nr to ack vector
                ackVector |= (INT64_C(1) << (diff - 1));
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
            ackVector = ackVector | (INT64_C(1) << (diff - 1));
        }
    }
    receiveTimestamp = (uint32_t)(time_us / (int(1000000 / kTimestampRate)));

}

ScreamRx::ScreamRx() {
    lastFeedbackT_us = 0;
    bytesReceived = 0;
    lastRateComputeT_us = 0;
    averageReceivedRate = 1e5;
}

bool ScreamRx::checkIfFlushAck(
    uint32_t ssrc,
    uint16_t seqNr) {
    if (!streams.empty()) {
        for (auto it = streams.begin(); it != streams.end(); ++it) {
            if ((*it)->isMatch(ssrc)) {
                /*
                * Packets for this SSRC received earlier
                * stream is thus already in list
                */
                return (*it)->checkIfFlushAck(seqNr);
            }
        }
    }
    return false;
}

void ScreamRx::receive(uint64_t time_us,
    void* rtpPacket,
    uint32_t ssrc,
    int size,
    uint16_t seqNr) {
    bytesReceived += size;
    if (lastRateComputeT_us == 0)
        lastRateComputeT_us = time_us;

    if (time_us - lastRateComputeT_us > 200000) {
        /*
        * Media rate computation (for all medias) is done at least every 200ms
        * This is used for RTCP feedback rate calculation
        */
        float delta = (time_us - lastRateComputeT_us) / 1e6f;
        lastRateComputeT_us = time_us;
        averageReceivedRate = std::max(0.95f*averageReceivedRate, bytesReceived * 8 / delta);
        bytesReceived = 0;
    }

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


uint64_t ScreamRx::getRtcpFbInterval() {
    /*
    * The RTCP feedback rate depends on the received media date
    *  at very low bitrates (<50kbps) an RTCP feedback interval of ~200ms is sufficient
    *  while higher rates (>2Mbps) a feedback interval of ~20ms is sufficient
    */
    float res = 1000000 / std::min(100.0f, std::max(10.0f, averageReceivedRate / 10000.0f));
    return uint64_t(res);
}

bool ScreamRx::isFeedback(uint64_t time_us) {
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

bool ScreamRx::getFeedback(uint64_t time_us,
    uint32_t &ssrc,
    uint32_t &receiveTimestamp,
    uint16_t &highestSeqNr,
    uint64_t &ackVector) {

    Stream *stream = NULL;
    uint64_t minT_us = ULLONG_MAX;
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
    stream->lastFeedbackT_us = time_us;
    stream->nRtpSinceLastRtcp = 0;
    lastFeedbackT_us = time_us;
    return true;
}
