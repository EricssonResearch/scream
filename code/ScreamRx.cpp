#include "ScreamRx.h"
#include "ScreamTx.h"
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


ScreamRx::Stream::Stream(uint32_t ssrc_) {
    ssrc = ssrc_;
    ackVector = 0;
    receiveTimestamp = 0x0;
    highestSeqNr = 0x0;
    highestSeqNrTx = 0x0;
    lastFeedbackT_us = 0;
    nRtpSinceLastRtcp = 0;
    firstReceived = false;
    ecnCeMarkedBytes = 0;
}

bool ScreamRx::Stream::checkIfFlushAck() {
    int diff = highestSeqNr - highestSeqNrTx;
    return (diff > (kAckVectorBits / 4));
}

void ScreamRx::Stream::receive(uint64_t time_us,
    void* rtpPacket,
    int size,
    uint16_t seqNr,
    bool isEcnCe) {
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
    receiveTimestamp = (uint32_t)(time_us / 1000);

    if (isEcnCe)
        ecnCeMarkedBytes += size;
}

ScreamRx::ScreamRx(uint32_t ssrc_) {
    lastFeedbackT_us = 0;
    bytesReceived = 0;
    lastRateComputeT_us = 0;
    averageReceivedRate = 1e5;
    rtcpFbInterval_us = 20000;
    ssrc = ssrc_;
    ix = 0;
}

ScreamRx::~ScreamRx() {
    if (!streams.empty()) {
        for (auto it = streams.begin(); it != streams.end(); ++it) {
            delete (*it); 
        }
    }
}

bool ScreamRx::checkIfFlushAck() {
    if (!streams.empty()) {
        for (auto it = streams.begin(); it != streams.end(); ++it) {
            if ((*it)->checkIfFlushAck())
               return true; 
        }
    }
    return false;
}

void ScreamRx::receive(uint64_t time_us,
    void* rtpPacket,
    uint32_t ssrc,
    int size,
    uint16_t seqNr,
    bool isEcnCe) {
    bytesReceived += size;
    if (lastRateComputeT_us == 0)
        lastRateComputeT_us = time_us;

    if (time_us - lastRateComputeT_us > 100000) {
        /*
        * Media rate computation (for all medias) is done at least every 100ms
        * This is used for RTCP feedback rate calculation
        */
        float delta = (time_us - lastRateComputeT_us) * 1e-6f;
        lastRateComputeT_us = time_us;
        averageReceivedRate = std::max(0.95f*averageReceivedRate, bytesReceived * 8 / delta);
        bytesReceived = 0;
        /*
        * The RTCP feedback rate depends on the received media date
        * Target ~2% overhead but with feedback interval limited
        *  to the range [2ms,100ms]
        */
        float rate = 0.02*averageReceivedRate / (70.0f * 8.0f); // RTCP overhead
        rate = std::min(500.0f, std::max(10.0f, rate));
        /*
        * More than one stream ?, increase the feedback rate as
        *  we currently don't bundle feedback packets
        */
        rate *= streams.size();
        rtcpFbInterval_us = uint64_t(1000000.0f / rate);
    }

    if (!streams.empty()) {
        for (auto it = streams.begin(); it != streams.end(); ++it) {
            if ((*it)->isMatch(ssrc)) {
                /*
                * Packets for this SSRC received earlier
                * stream is thus already in list
                */
                (*it)->receive(time_us, rtpPacket, size, seqNr, isEcnCe);
                return;

            }
        }
    }
    /*
    * New {SSRC,PT}
    */
    Stream *stream = new Stream(ssrc);
    stream->ix = ix++;
    stream->receive(time_us, rtpPacket, size, seqNr, isEcnCe);
    streams.push_back(stream);
}


uint64_t ScreamRx::getRtcpFbInterval() {
    return rtcpFbInterval_us;
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

bool ScreamRx::getFeedback(uint64_t time_us,
    uint32_t &ssrc,
    uint32_t &receiveTimestamp,
    uint16_t &highestSeqNr,
    uint64_t &ackVector,
    uint16_t &ecnCeMarkedBytes) {

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
    stream->highestSeqNrTx = highestSeqNr;
    ssrc = stream->ssrc;
    ackVector = stream->ackVector;
    ecnCeMarkedBytes = stream->ecnCeMarkedBytes;
    stream->lastFeedbackT_us = time_us;
    stream->nRtpSinceLastRtcp = 0;
    lastFeedbackT_us = time_us;
    return true;
}

/*
* Create feedback according to the format below. It is up to the
* wrapper application to prepend this RTCP with SR or RR when needed
* BT = 255, means that this is experimental use
* The code currently only handles one SSRC source per IP packet
*
* 0                   1                   2                   3
* 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |V=2|P|reserved |   PT=XR=207   |           length=6            |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                              SSRC                             |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |     BT=255    |    reserved   |         block length=4        |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                        SSRC of source                         |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* | Highest recv. seq. nr. (16b)  |         ECN_CE_bytes          |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                     Ack vector (b0-31)                        |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                     Ack vector (b32-63)                       |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                    Timestamp (32bits)                         |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
bool ScreamRx::createFeedback(uint64_t time_us, unsigned char *buf, int &size) {

    if (!isFeedback(time_us))
        return false;
    uint32_t timeStamp;
    uint16_t seqNr;
    uint64_t ackVector;
    uint16_t ecnCeMarkedBytes;
    uint32_t ssrc_src;
    size = 32;
    if (getFeedback(time_us, ssrc_src, timeStamp, seqNr, ackVector, ecnCeMarkedBytes)) {
        uint16_t tmp_s;
        uint32_t tmp_l;
        buf[0] = 0x80;
        buf[1] = 207;
        tmp_s = htons(6);
        memcpy(buf + 2, &tmp_s, 2);
        tmp_l = htonl(ssrc);
        memcpy(buf + 4, &tmp_l, 4);
        buf[8] = 0xFF; // BT=255
        buf[9] = 0x00;
        tmp_s = htons(4);
        memcpy(buf + 10, &tmp_s, 2);
        tmp_l = htonl(ssrc_src);
        memcpy(buf + 12, &tmp_l, 4);
        tmp_s = htons(seqNr);
        memcpy(buf + 16, &tmp_s, 2);
        tmp_s = htons(ecnCeMarkedBytes);
        memcpy(buf + 18, &tmp_s, 2);
        tmp_l = uint32_t((ackVector >> 32) & 0xFFFFFFFF);
        tmp_l = htonl(tmp_l);
        memcpy(buf + 20, &tmp_l, 4);
        tmp_l = uint32_t(ackVector & 0xFFFFFFFF);
        tmp_l = htonl(tmp_l);
        memcpy(buf + 24, &tmp_l, 4);
        tmp_l = htonl(timeStamp);
        memcpy(buf + 28, &tmp_l, 4);
        return true;
    }
    return false;
}
