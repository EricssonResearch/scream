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

const int kMaxRtcpSize = 500;

ScreamRx::Stream::Stream(uint32_t ssrc_) {
    ssrc = ssrc_;
    ackVector = 0;
    receiveTimestamp = 0x0;
    highestSeqNr = 0x0;
    highestSeqNrTx = 0x0;
    lastFeedbackT_ntp = 0;
    nRtpSinceLastRtcp = 0;
    firstReceived = false;
    ecnCeMarkedBytes = 0;

    for (int n = 0; n < kRxHistorySize; n++) {
        ceBitsHist[n] = 0x00;
        rxTimeHist[n] = 0;
        seqNrHist[n] = 0x0000;
    }

}

bool ScreamRx::Stream::checkIfFlushAck() {
    int diff = highestSeqNr - highestSeqNrTx;
    return (diff > (kAckVectorBits / 4));
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
    receiveTimestamp = time_ntp;

    if (isEcnCe)
        ecnCeMarkedBytes += size;
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
    * always report kReportedRtpPackets RTP packets
    */
    tmp_s = highestSeqNr - uint16_t(kReportedRtpPackets - 1);
    tmp_s = htons(tmp_s);
    memcpy(buf + 4, &tmp_s, 2);
    size += 2;

    /*
    * Write end_seq
    */
    tmp_s = highestSeqNr + 1;
    tmp_s = htons(tmp_s);
    memcpy(buf + 6, &tmp_s, 2);
    size += 2;
    int ptr = 8;

    /*
    * Write 16bits report element for received RTP packets
    */
    uint16_t sn_lo = highestSeqNr - uint16_t(kReportedRtpPackets - 1);

    for (uint16_t k = 0; k < kReportedRtpPackets; k++) {
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
    if (kReportedRtpPackets % 2 == 1) {
        tmp_s = 0x0000;
        memcpy(buf + ptr, &tmp_s, 2);
        size += 2;
        ptr += 2;
    }

    return true;
}

ScreamRx::ScreamRx(uint32_t ssrc_) {
    lastFeedbackT_ntp = 0;
    bytesReceived = 0;
    lastRateComputeT_ntp = 0;
    averageReceivedRate = 1e5;
    rtcpFbInterval_ntp = 13107; // 20ms in NTP domain
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
        float rate = 0.02f*averageReceivedRate / (70.0f * 8.0f); // RTCP overhead
        rate = std::min(500.0f, std::max(10.0f, rate));
        /*
        * More than one stream ?, increase the feedback rate as
        *  we currently don't bundle feedback packets
        */
        rate *= streams.size();
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

bool ScreamRx::createStandardizedFeedback(uint32_t time_ntp, unsigned char *buf, int &size) {

    /*
    * First check if there is any stream available that has any feedback
    * Note, only one stream per RTCP feedback packet,
    * TODO to enable more streams per RTCP some day...
    */
    Stream *stream = NULL;
    uint32_t minT_ntp = ULONG_MAX;
    for (auto it = streams.begin(); it != streams.end(); ++it) {
        if ((*it)->nRtpSinceLastRtcp > 0 && (*it)->lastFeedbackT_ntp < minT_ntp) {
            stream = *it;
            minT_ntp = (*it)->lastFeedbackT_ntp;

        }
    }
    if (stream == NULL)
        return false;

    uint16_t tmp_s;
    uint32_t tmp_l;
    buf[0] = 0x80; // TODO FMT = CCFB in 5 LSB
    buf[1] = 207;
    /*
    * Write RTCP sender SSRC
    */
    tmp_l = htonl(ssrc);
    memcpy(buf + 4, &tmp_l, 4);
    size = 8;
    int ptr = 8;
    /*
    * Generate RTCP feedback size until a safe sizelimit ~kMaxRtcpSize+128 byte is reached
    */
    while (size < kMaxRtcpSize) {
        /*
        * TODO, we do the above stream fetching over again even though we have the
        * stream in the first iteration, a bit unnecessary.
        */
        Stream *stream = NULL;
        uint32_t minT_ntp = ULONG_MAX;
        for (auto it = streams.begin(); it != streams.end(); ++it) {
            if ((*it)->nRtpSinceLastRtcp > 0 && (*it)->lastFeedbackT_ntp < minT_ntp) {
                stream = *it;
                minT_ntp = (*it)->lastFeedbackT_ntp;
            }
        }
        if (stream == NULL)
            break;

        int size_stream = 0;
        stream->getStandardizedFeedback(time_ntp, &buf[ptr], size_stream);
        size += size_stream;
        ptr += size_stream;
        stream->lastFeedbackT_ntp = time_ntp;
        stream->nRtpSinceLastRtcp = 0;
        stream->highestSeqNrTx = stream->highestSeqNr;
    }
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

#ifdef OLD_CODE
bool ScreamRx::getProprietaryFeedback(uint32_t time_ntp,
    uint32_t &ssrc,
    uint32_t &receiveTimestamp,
    uint16_t &highestSeqNr,
    uint64_t &ackVector,
    uint16_t &ecnCeMarkedBytes) {

    Stream *stream = NULL;
    uint32_t minT_ntp = ULONG_MAX;
    for (auto it = streams.begin(); it != streams.end(); ++it) {
        if ((*it)->nRtpSinceLastRtcp > 0 && (*it)->lastFeedbackT_ntp < minT_ntp) {
            stream = *it;
            minT_ntp = (*it)->lastFeedbackT_ntp;

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
    stream->lastFeedbackT_ntp = time_ntp;
    stream->nRtpSinceLastRtcp = 0;
    lastFeedbackT_ntp = time_ntp;

    return true;
}
bool ScreamRx::createProprietaryFeedback(uint32_t time_ntp, unsigned char *buf, int &size) {

    if (!isFeedback(time_ntp))
        return false;
    uint32_t timeStamp;
    uint16_t seqNr;
    uint64_t ackVector;
    uint16_t ecnCeMarkedBytes;
    uint32_t ssrc_src;
    size = 32;
    if (getProprietaryFeedback(time_ntp, ssrc_src, timeStamp, seqNr, ackVector, ecnCeMarkedBytes)) {
        uint16_t tmp_s;
        uint32_t tmp_l;
        buf[0] = 0x80;
        buf[1] = 205;
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
#endif
