#ifndef SCREAM_RX
#define SCREAM_RX
#include <cstdint>
#include <list>
const int kReportedRtpPackets = 64;
const int kRxHistorySize = 128;

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

class ScreamRx {
public:
	ScreamRx(uint32_t ssrc, int ackDiff = -1, int nReportedRtpPackets = kReportedRtpPackets); // SSRC of this RTCP session
	~ScreamRx();

	/*
	* One instance is created for each source SSRC
	*/
	class Stream {
	public:
		Stream(uint32_t ssrc);

		bool isMatch(uint32_t ssrc_) { return ssrc == ssrc_; };

		bool checkIfFlushAck(int ackDiff);

		/*
		* Receive RTP packet
		*/
		void receive(uint32_t time_ntp,
			void *rtpPacket,
			int size,
			uint16_t seqNr,
			bool isEcnCe,
			uint8_t ceBits);

		/*
		* Get SCReAM standardized RTCP feedback
		* return FALSE if no pending feedback available
		*/
		bool getStandardizedFeedback(uint32_t time_ntp,
			unsigned char *buf,
			int &size);


		uint32_t ssrc;                       // SSRC of stream (source SSRC)
		uint16_t highestSeqNr;               // Highest received sequence number
		uint16_t highestSeqNrTx;             // Highest fed back sequence number
		uint32_t receiveTimestamp;           // Wall clock time
		uint8_t  ceBitsHist[kRxHistorySize]; // Vector of CE bits for last <kRxHistorySize>
		//  received RTP packets
		uint32_t rxTimeHist[kRxHistorySize]; // Receive time for last <kRxHistorySize>
		//  received RTP packets
		uint16_t seqNrHist[kRxHistorySize];  // Seq Nr of last received <kRxHistorySize>
		//  packets
		uint32_t lastFeedbackT_ntp;          // Last time feedback transmitted for
		//  this SSRC
		int nRtpSinceLastRtcp;               // Number of RTP packets since last transmitted RTCP

		bool firstReceived;

		float timeStampConversionFactor;

		int ix;

		int nReportedRtpPackets;
	};

	/*
	* Check to ensure that ACKs can cover also large holes in
	*  in the received sequence number space. These cases can frequently occur when
	*  SCReAM is used in frame discard mode i.e. when real video rate control is
	*  not possible
	*/
	bool checkIfFlushAck();

	/*
	* Function is called each time an RTP packet is received
	*/
	void receive(uint32_t time_ntp,
		void* rtpPacket,
		uint32_t ssrc,
		int size,
		uint16_t seqNr,
		uint8_t ceBits = 0x00);

	/*
	* Return TRUE if an RTP packet has been received and there is
	* pending feedback
	*/
	bool isFeedback(uint32_t time_ntp);

	/*
	* Return RTCP feedback interval (Q16)
	*/
	uint32_t getRtcpFbInterval();

	/*
	* Create standardized feedback according to
	* https://tools.ietf.org/wg/avtcore/draft-ietf-avtcore-cc-feedback-message/
	* Current implementation implements -02 version
	* It is up to the wrapper application to prepend this RTCP
	*  with SR or RR when needed
	*/
	bool createStandardizedFeedback(uint32_t time_ntp, bool isMark, unsigned char *buf, int &size);

	/*
	* Get last feedback transmission time in NTP domain (Q16)
	*/
	uint32_t getLastFeedbackT() { return lastFeedbackT_ntp; };

	uint32_t lastFeedbackT_ntp;
	int bytesReceived;
	uint32_t lastRateComputeT_ntp;
	float averageReceivedRate;
	uint32_t rtcpFbInterval_ntp;
	uint32_t ssrc;

	int getIx(uint32_t ssrc);
	int ix;
	int ackDiff;

	int nReportedRtpPackets;
	/*
	* Variables for multiple steams handling
	*/
	std::list<Stream*> streams;
};

#endif
