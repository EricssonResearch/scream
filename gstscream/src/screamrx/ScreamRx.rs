#![allow(clippy::uninlined_format_args)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
use gst::prelude::*;

use std::cmp;
use std::convert::TryInto;
use std::sync::Arc;
use std::sync::Mutex;

extern crate array_init;
use hashbrown::HashMap;
use once_cell::sync::Lazy;

use crate::screamrx::imp::CAT;

const kReportedRtpPackets: usize = 64;
const kRxHistorySize: usize = 128;

/*
* This module implements the receiver side of SCReAM.
* As SCReAM is a sender based congestion control, the receiver side is
*  actually dumber than dumb. In essense the only thing that it does is to
*  + Record receive time stamps and RTP sequence numbers for incoming RTP packets
*  + Generate RTCP feedback elements
*  + Calculate an appropriate RTCP feedback interval
* See https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx
*  for details on how it is integrated in audio/video platforms.
* A full implementation needs the additional code for
*  + Other obvious stuff such as RTP payload depacketizer, video+audio deoders, rendering, dejitterbuffers
* It is recommended that RTCP feedback for multiple streams are bundled in one RTCP packet.
*  However as low bitrate media (e.g audio) requires a lower feedback rate than high bitrate media (e.g video)
*  it is possible to include RTCP feedback for the audio stream more seldom. The code for this is T.B.D
*
* Internal time is represented as the mid 32 bits of the NTP timestamp (see RFC5905)
* This means that the high order 16 bits is time in seconds and the low order 16 bits
* is the fraction. The NTP time stamp is thus in Q16 i.e 1.0sec is represented
* by the value 65536.
* All internal time is measured in NTP time, this is done to avoid wraparound issues
* that can otherwise occur every 18 hour or so
*/

const kMaxRtcpSize: usize = 900;
const ntp2SecScaleFactor: f64 = 1.0 / 65536.0;
struct Stream {
    ssrc: u32,                        // SSRC of stream (source SSRC)
    highestSeqNr: u16,                // Highest received sequence number
    highestSeqNrTx: u16,              // Highest fed back sequence number
    ceBitsHist: [u8; kRxHistorySize], // Vector of CE bits for last <kRxHistorySize>
    //  received RTP packets
    rxTimeHist: [u32; kRxHistorySize], // Receive time for last <kRxHistorySize>
    //  received RTP packets
    seqNrHist: [u16; kRxHistorySize], // Seq Nr of last received <kRxHistorySize>
    //  packets
    lastFeedbackT_ntp: u32, // Last time feedback transmitted for
    //  this SSRC
    nRtpSinceLastRtcp: i32, // Number of RTP packets since last transmitted RTCP
    nRecvRtpPackets: u64,
    lastSn: u16,
    firstReceived: bool,
    ix: i32,
    nReportedRtpPackets: i32,
    first_recv_ntp: u32,
}

pub struct ScreamRx {
    /*
     * One instance is created for each source SSRC
     */
    lastFeedbackT_ntp: u32,
    bytesReceived: u32,
    lastRateComputeT_ntp: u32,
    averageReceivedRate: f64,
    rtcpFbInterval_ntp: u32,
    ssrc: u32,
    ix: i32,
    ackDiff: i32,
    nReportedRtpPackets: usize,
    streams: HashMap<u32, Stream>,
    //    socket: Option<UdpSocket>,
    rtcp_srcpad: Option<Arc<Mutex<gst::Pad>>>,
    /*
     * Variables for multiple steams handling
     */
    //    std::list<Stream*> streams;
}
impl Default for ScreamRx {
    fn default() -> Self {
        ScreamRx {
            // SSRC of this RTCP session
            lastFeedbackT_ntp: 0,
            bytesReceived: 0,
            lastRateComputeT_ntp: 0,
            averageReceivedRate: 1e5,
            rtcpFbInterval_ntp: 13107 / 4, // 20ms in NTP domain
            ssrc: 10,
            ix: 0,
            ackDiff: i32::max(1, (kReportedRtpPackets / 4).try_into().unwrap()),
            // ackDiff: -1,
            // ackDiff: 1,
            nReportedRtpPackets: kReportedRtpPackets,
            streams: HashMap::new(),
            // socket: None,
            rtcp_srcpad: None,
        }
    }
}

impl ScreamRx {
    pub fn ScreamReceiverPluginInit(&mut self, rtcp_srcpad: Option<Arc<Mutex<gst::Pad>>>) {
        gst::info!(CAT, "Init");
        self.rtcp_srcpad = rtcp_srcpad;
    }

    pub fn getLastFeedbackT(&self) -> u32 {
        self.lastFeedbackT_ntp
    }

    pub fn getRtcpFbInterval(&self) -> u32 {
        self.rtcpFbInterval_ntp
    }

    pub fn checkIfFlushAck(&mut self) -> bool {
        if self.ackDiff == 1 {
            return true;
        }
        for (_, it) in self.streams.iter_mut() {
            if it.checkIfFlushAck(self.ackDiff) {
                return true;
            }
        }
        false
    }

    pub fn isFeedback(&mut self, _time_ntp: u32) -> bool {
        for (_, it) in self.streams.iter_mut() {
            if it.nRtpSinceLastRtcp >= 1 {
                return true;
            }
        }
        false
    }

    pub fn receive_register(
        &mut self,
        time_ntp: u32,
        ssrc: u32,
        size: u32,
        seqNr: u16,
        ceBits: u8,
    ) {
        self.bytesReceived += size;
        if self.lastRateComputeT_ntp == 0 {
            self.lastRateComputeT_ntp = time_ntp;
        }

        if time_ntp > self.lastRateComputeT_ntp && time_ntp - self.lastRateComputeT_ntp > 6554 {
            // 100ms in NTP domain
            /*
             * Media rate computation (for all medias) is done at least every 100ms
             * This is used for RTCP feedback rate calculation
             */
            let delta: f64 = ((time_ntp - self.lastRateComputeT_ntp) as f64) * ntp2SecScaleFactor;
            self.lastRateComputeT_ntp = time_ntp;
            self.averageReceivedRate = f64::max(
                0.95 * self.averageReceivedRate,
                (self.bytesReceived * 8) as f64 / delta,
            );
            /*
             * The RTCP feedback rate depends on the received media date
             * Target ~2% overhead but with feedback interval limited
             *  to the range [2ms,100ms]
             */
            let mut rate: f64 = 0.02 * self.averageReceivedRate / (100.0 * 8.0); // RTCP overhead
            rate = rate.clamp(10., 500.);
            /*
             * More than one stream ?, increase the feedback rate as
             *  we currently don't bundle feedback packets
             */
            //rate *= streams.size();
            // rate *= 4.0;
            let ftmp: f64 = 65536.0 / rate;
            self.rtcpFbInterval_ntp = ftmp as u32;
            // Convert to NTP domain (Q16)
            gst::trace!(CAT, "time_ntp - lastRateComputeT_ntp {} , delta {}, averageReceivedRate {}, rate {},size {},  bytesReceived {}",
                       time_ntp - self.lastRateComputeT_ntp, delta, self.averageReceivedRate, rate, size, self.bytesReceived);
            self.bytesReceived = 0;
        }

        let v = self.streams.get_mut(&ssrc);
        match v {
            Some(it) => {
                it.receive(time_ntp, seqNr, ceBits);
                return;
            }
            None => gst::log!(CAT, "receive not found {}", ssrc),
        }

        /*
         * New {SSRC,PT}
         */
        let mut stream = Stream::new(ssrc);
        println!("ScreamRx.rs: new ssrc {}", ssrc);
        stream.nReportedRtpPackets = self.nReportedRtpPackets as i32;
        stream.ix = self.ix + 1;
        stream.ssrc = ssrc;
        stream.receive(time_ntp, seqNr, ceBits);
        gst::log!(CAT, "receive insert  {}", ssrc);
        self.streams.insert(ssrc, stream);
    }
    #[allow(clippy::too_many_arguments)]
    pub fn ScreamReceiver(
        &mut self,
        recvlen: u32,
        seqNr: u16,
        _payload_type: u8,
        _timestamp: u32,
        ssrc: u32,
        marker: bool,
        EcnCe: u8,
    ) {
        /*
         * Register received RTP packet with ScreamRx
         */
        let time_ntp = getTimeInNtp();
        self.receive_register(time_ntp, ssrc, recvlen, seqNr, EcnCe);

        if self.checkIfFlushAck() || marker {
            let mut bytes = Vec::with_capacity(300);
            let isFeedback = self.createStandardizedFeedback(getTimeInNtp(), marker, &mut bytes);
            gst::trace!(CAT, "isFeedback {} marker  {}", isFeedback, marker);
            if isFeedback {
                let buffer = gst::Buffer::from_mut_slice(bytes);
                let rtcp_srcpad = &self.rtcp_srcpad.as_ref().unwrap().lock().unwrap();
                rtcp_srcpad.push(buffer).unwrap();
            }
        }
    }
    pub fn periodic_flush(&mut self) {
        let time_ntp = getTimeInNtp();
        let rtcpFbInterval_ntp = self.getRtcpFbInterval();
        if self.isFeedback(time_ntp)
            && (self.checkIfFlushAck()
                || (time_ntp > self.getLastFeedbackT()
                    && time_ntp - self.getLastFeedbackT() > rtcpFbInterval_ntp))
        {
            gst::trace!(CAT, "periodic_flush ssrc {} time_ntp {}, .getLastFeedbackT {},diff {} rtcpFbInterval_ntp {} averageReceivedRate {} ",
                     self.ssrc, time_ntp, self.getLastFeedbackT(), time_ntp - self.getLastFeedbackT(), rtcpFbInterval_ntp,  self.averageReceivedRate);
            let mut bytes = Vec::with_capacity(300);
            let isFeedback = self.createStandardizedFeedback(time_ntp, true, &mut bytes);
            if isFeedback {
                gst::trace!(CAT, "periodic_flush ");
                let buffer = gst::Buffer::from_mut_slice(bytes);
                let rtcp_srcpad = &self.rtcp_srcpad.as_ref().unwrap().lock().unwrap();
                rtcp_srcpad.push(buffer).unwrap();
            }
        }
    }
}

impl Drop for ScreamRx {
    fn drop(&mut self) {}
}

impl Stream {
    pub fn new(ssrc_: u32) -> Stream {
        Stream {
            ssrc: ssrc_,
            highestSeqNr: 0,
            highestSeqNrTx: 0,
            lastFeedbackT_ntp: 0,
            nRtpSinceLastRtcp: 0,
            nRecvRtpPackets: 0,
            firstReceived: false,
            ix: 0,
            lastSn: 0,
            nReportedRtpPackets: 0,
            first_recv_ntp: 0,
            rxTimeHist: array_init::array_init(|_| 0),
            seqNrHist: array_init::array_init(|_| 0),
            ceBitsHist: array_init::array_init(|_| 0),
        }
    }
    /*
     * Check to ensure that ACKs can cover also large holes in
     *  in the received sequence number space. These cases can frequently occur when
     *  SCReAM is used in frame discard mode i.e. when real video rate control is
     *  not possible
     */
    pub fn checkIfFlushAck(&self, ackDiff: i32) -> bool {
        let diff: i32 = self.highestSeqNr as i32 - self.highestSeqNrTx as i32;
        if diff <= ackDiff {
            /*
                        trace!(
                            CAT,
                            "highestSeqNr {}, highestSeqNrTx {} ackDiff {} nRecvRtpPackets {}",
                            self.highestSeqNr as i32,
                            self.highestSeqNrTx as i32,
                            ackDiff,
                            self.nRecvRtpPackets
                        );
            */
        }

        diff >= ackDiff
    }

    /*
     * Function is called each time an RTP packet is received
     */
    pub fn receive(&mut self, time_ntp: u32, seqNr: u16, ceBits: u8) {
        self.nRecvRtpPackets += 1;
        /*
         * Count received RTP packets since last RTCP transmitted for this SSRC
         */
        self.nRtpSinceLastRtcp += 1;
        /*
         * Initialize on first received packet
         */
        if !self.firstReceived {
            self.first_recv_ntp = time_ntp;
            self.highestSeqNr = seqNr;
            self.highestSeqNr -= 1;
            for n in 0..kRxHistorySize {
                // Initialize seqNr list properly
                self.seqNrHist[n] = seqNr + 1;
            }
            self.firstReceived = true;
        }
        /*
         * Update CE bits and RX time vectors
         */
        let ix: usize = (seqNr as usize) % kRxHistorySize;
        self.ceBitsHist[ix] = ceBits;
        self.rxTimeHist[ix] = time_ntp;
        self.seqNrHist[ix] = seqNr;

        let mut seqNrExt: u32 = seqNr.into();
        let mut highestSeqNrExt: u32 = self.highestSeqNr.into();

        if seqNr < self.highestSeqNr && self.highestSeqNr - seqNr > 16384 {
            seqNrExt += 65536;
        }

        if self.highestSeqNr < seqNr && seqNr - self.highestSeqNr > 16384 {
            highestSeqNrExt += 65536;
        }

        let diff: i32 = seqNr as i32 - self.lastSn as i32;
        if diff != 1 {
            gst::debug!(CAT,
                "Packet(s) lost or reordered time_ntp {} : {} was received, previous rcvd is {}, nRecvRtpPackets {}, ssrc {}",
                time_ntp - self. first_recv_ntp, seqNr, self.lastSn, self.nRecvRtpPackets, self.ssrc
            );
        }
        self.lastSn = seqNr;

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
        if seqNrExt >= highestSeqNrExt {
            /*
             * Normal in-order reception
             */
            gst::trace!(
                CAT,
                "old / new highestSeqNr {} {}  seqNrExt {} highestSeqNrExt {}",
                self.highestSeqNr,
                seqNr,
                seqNrExt,
                highestSeqNrExt
            );
            self.highestSeqNr = seqNr;
        } else {
            gst::log!(
                CAT,
                "old / no-new highestSeqNr {} {}  seqNrExt {} highestSeqNrExt {}",
                self.highestSeqNr,
                seqNr,
                seqNrExt,
                highestSeqNrExt
            );
        }
    }

    /*
     * Get SCReAM standardized RTCP feedback
     * return FALSE if no pending feedback available
     */

    pub fn getStandardizedFeedback(&mut self, time_ntp: u32, bytes: &mut Vec<u8>) {
        /*
         * Write RTP sender SSRC
         */
        bytes.extend_from_slice(&self.ssrc.to_be_bytes());

        /*
         * Write begin_seq
         * always report nReportedRtpPackets RTP packets
         */
        gst::trace!(
            CAT,
            "getStandardizedFeedback (self.ceBitsHist[ix] & 0x03) {} {}",
            self.highestSeqNr,
            self.nReportedRtpPackets - 1
        );
        let tmp_s: u16 = if self.highestSeqNr < ((self.nReportedRtpPackets - 1) as u16) {
            (u16::MAX - ((self.nReportedRtpPackets - 1) as u16)) + 1 + self.highestSeqNr
        } else {
            self.highestSeqNr - ((self.nReportedRtpPackets - 1) as u16)
        };
        bytes.extend_from_slice(&tmp_s.to_be_bytes());
        /*
         * Write number of reports- 1
         */
        let tmp_s: u16 = (self.nReportedRtpPackets - 1).try_into().unwrap();
        bytes.extend_from_slice(&tmp_s.to_be_bytes());

        /*
         * Write 16bits report element for received RTP packets
         */
        let sn_lo: u16 = self
            .highestSeqNr
            .overflowing_sub((self.nReportedRtpPackets - 1) as u16)
            .0;

        gst::log!(CAT, "getStandardizedFeedback time_diff  {} begin_seq {}, num_reports {} , end_seq {} nRecvRtpPackets {} ",
                   time_ntp - self.lastFeedbackT_ntp, sn_lo,  self.nReportedRtpPackets,  self.highestSeqNr, self.nRecvRtpPackets);
        for k in 0..self.nReportedRtpPackets {
            let sn: u16 = ((sn_lo as u32 + k as u32) & u16::MAX as u32) as u16;

            let ix = (sn as usize) % kRxHistorySize;
            let mut ato: u32 = time_ntp - self.rxTimeHist[ix];
            ato >>= 6; // Q16->Q10
            if ato > 8189 {
                ato = 0x1FFE;
            }

            let mut tmp_s: u16 = 0x0000;
            if self.seqNrHist[ix] == sn && self.rxTimeHist[ix] != 0 {
                tmp_s = (0x8000_u32
                    | (((self.ceBitsHist[ix] & 0x03) as u32) << 13)
                    | (ato & (0x01FFF_u32))) as u16;
            }

            bytes.extend_from_slice(&tmp_s.to_be_bytes());
        }
        /*
         * Zero pad with two extra octets if the number of reported packets is odd
         */
        if self.nReportedRtpPackets % 2 == 1 {
            let tmp_s: u16 = 0x0000;
            bytes.extend_from_slice(&tmp_s.to_be_bytes());
        }
    }
}
impl ScreamRx {
    pub fn createStandardizedFeedback(
        &mut self,
        time_ntp: u32,
        isMark: bool,
        bytes: &mut Vec<u8>,
    ) -> bool {
        bytes.push(0x80); // TODO FMT = CCFB in 5 LSB
        bytes.push(205);
        /* Padding */
        bytes.push(0);
        bytes.push(0);
        /*
         * Write RTCP sender SSRC
         */
        gst::trace!(CAT, "isMark {}", isMark);
        if isMark {
            gst::trace!(CAT, "isMark {}", isMark);
        }
        bytes.extend_from_slice(&self.ssrc.to_be_bytes());
        let mut isFeedback = false;
        /*
         * Generate RTCP feedback size until a safe sizelimit ~kMaxRtcpSize+128 byte is reached
         */
        while bytes.len() < kMaxRtcpSize {
            /*
             * TODO, we do the above stream fetching over again even though we have the
             * stream in the first iteration, a bit unnecessary.
             */
            let mut stream_opt: Option<&mut Stream> = None;
            let mut minT_ntp: u32 = u32::MAX;
            let mut diffT_ntp: u32;
            let mut nRtpSinceLastRtcp: i32;
            for (_, it) in self.streams.iter_mut() {
                diffT_ntp = time_ntp - it.lastFeedbackT_ntp;
                nRtpSinceLastRtcp = it.nRtpSinceLastRtcp;
                if ((it).nRtpSinceLastRtcp >= cmp::min(8,self.ackDiff) || diffT_ntp > 655 ||
                    (isMark && (it).nRtpSinceLastRtcp > 0)) && // 10ms in Q16
                    (it).lastFeedbackT_ntp < minT_ntp
                {
                    minT_ntp = (it).lastFeedbackT_ntp;

                    gst::trace!(CAT, "diff_ntp {}, time_ntp {}, lastFeedbackT_ntp {}, isMark {}, nRtpSinceLastRtcp {}, ackDiff {} len {}",
                             diffT_ntp, &time_ntp, &it.lastFeedbackT_ntp, &isMark, &(it).nRtpSinceLastRtcp, &self.ackDiff, bytes.len());
                    stream_opt = Some(it);
                } else if nRtpSinceLastRtcp >= 0 {
                    gst::trace!(
                        CAT,
                        "isMark {}, nRtpSinceLastRtcp {} nRecvRtpPackets {} diffT_ntp {}",
                        isMark,
                        (it).nRtpSinceLastRtcp,
                        (it).nRecvRtpPackets,
                        diffT_ntp
                    );
                }
            }
            if stream_opt.is_none() {
                break;
            }
            let stream = stream_opt.unwrap();
            isFeedback = true;
            stream.getStandardizedFeedback(time_ntp, bytes);
            stream.lastFeedbackT_ntp = time_ntp;
            stream.nRtpSinceLastRtcp = 0;
            stream.highestSeqNrTx = stream.highestSeqNr;
        }
        if !isFeedback {
            gst::trace!(CAT, "createStandardizedFeedback: no feedback");
            return false;
        }
        /*
         * Write report timestamp
         */
        bytes.extend_from_slice(&time_ntp.to_be_bytes());

        /*
         * write length
         */
        let length: u16 = (bytes.len() / 4 - 1).try_into().unwrap();
        let lbe = &length.to_be_bytes();
        bytes[2] = lbe[0];
        bytes[3] = lbe[1];

        /*
         * Update stream RTCP feedback status
         */
        self.lastFeedbackT_ntp = time_ntp;
        true
    }
}
/*
static t0: Lazy<std::time::Instant> = Lazy::new(||
    std::time::Instant::now()
);
*/

static t0: Lazy<std::time::Instant> = Lazy::new(std::time::Instant::now);

pub fn getTimeInNtp() -> u32 {
    let time = t0.elapsed().as_secs_f64();
    let ntp64: u64 = (time * 65536.0) as u64;
    (0xFFFFFFFF & ntp64) as u32 // NTP in Q16
}
