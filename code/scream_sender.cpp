// Scream sender side wrapper
#include "ScreamTx.h"
#include "RtpQueue.h"
#include "sys/socket.h"
#include "sys/types.h"
#include "netinet/in.h"
#include <string.h> /* needed for memset */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <iostream>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/timerfd.h>
struct itimerval timer;
struct sigaction sa;

struct periodicInfo
{
	int timer_fd;
	unsigned long long wakeupsMissed;
};

using namespace std;

#define BUFSIZE 2048

float minPaceInterval = 0.0005f;
int minPaceIntervalUs = 500;

#define ECN_CAPABLE
/*
* ECN capable
* -1 = Not-ECT
* 0 = ECT(0)
* 1 = ECT(1)
* 3 = CE
*/
int ect = -1;

float FPS = 50.0f; // Frames per second
uint32_t SSRC = 100;
int fixedRate = 0;
bool isKeyFrame = false;
bool disablePacing = false;
float keyFrameInterval = 0.0;
float keyFrameSize = 1.0;
int initRate = 1000;
int minRate = 1000;
int maxRate = 200000;
bool enableClockDriftCompensation = false;
float burstTime = -1.0;
float burstSleep = -1.0;
bool isBurst = false;
float burstStartTime = -1.0;
float burstSleepTime = -1.0;
bool pushTraffic = false;
float windowHeadroom = 5.0f;
bool relaxedPacing = false;
float packetPacingHeadroom = 1.5f;
float scaleFactor = 0.7f;
ScreamV2Tx* screamTx = 0;
float bytesInFlightHeadroom = 2.0f;
float multiplicativeIncreaseFactor = 0.05f;
float adaptivePaceHeadroom = 1.5f;
float hysteresis = 0.0f;
float reorderTime = 0.03f;
float enableDelayBasedCc = true;

uint16_t seqNr = 0;
uint32_t lastKeyFrameT_ntp = 0;

float runTime = -1.0;
bool stopThread = false;
pthread_t create_rtp_thread = 0;
pthread_t transmit_rtp_thread = 0;
pthread_t rtcp_thread = 0;

int mtu = 1200;
int mtuList[10];
int nMtuListItems = 1;

int minPktsInFlight = 0;

float delayTarget = 0.06f;
bool printSummary = true;

int fd_outgoing_rtp;
RtpQueue* rtpQueue = 0;

// We don't bother about SSRC in this implementation, it is only one stream

const char* DECODER_IP = "192.168.0.21";
int DECODER_PORT = 30110;
const char* DUMMY_IP = "217.10.68.152"; // Dest address just to punch hole in NAT

struct sockaddr_in outgoing_rtp_addr;
struct sockaddr_in incoming_rtcp_addr;
struct sockaddr_in dummy_rtcp_addr;

struct sockaddr_in6 outgoing_rtp_addr6;
struct sockaddr_in6 incoming_rtcp_addr6;
struct sockaddr_in6 dummy_rtcp_addr6;



socklen_t addrlen_outgoing_rtp;
socklen_t addrlen_dummy_rtcp;
socklen_t addrlen_incoming_rtcp, addrlen_incoming_rtcp6;// = sizeof(incoming_rtcp_addr);

unsigned char buf_rtcp[BUFSIZE];     /* receive buffer RTCP packets*/

uint32_t lastLogT_ntp = 0;
uint32_t lastLogTv_ntp = 0;
uint32_t tD_ntp = 0;//(INT64_C(1) << 32)*1000 - 5000000;

pthread_mutex_t lock_scream;
pthread_mutex_t lock_rtp_queue;
pthread_mutex_t lock_pace;

FILE* fp_log = 0;
FILE* fp_txrxlog = 0;

char* ifname = 0;

bool ntp = false;
bool append = false;
bool itemlist = false;
bool detailed = false;
float randRate = 0.0f;
bool ipv6 = false;

double t0 = 0;
/*
long getTimeInUs(){
struct timeval tp;
gettimeofday(&tp, NULL);
long us = tp.tv_sec * 1000000 + tp.tv_usec;
return us;
}
*/

void packet_free(void* buf, uint32_t ssrc) {
	free(buf);
}

uint32_t getTimeInNtp() {
	struct timeval tp;
	gettimeofday(&tp, NULL);
	double time = tp.tv_sec + tp.tv_usec * 1e-6 - t0;
	uint64_t ntp64 = uint64_t(time * 65536.0);
	uint32_t ntp = 0xFFFFFFFF & ntp64;
	return ntp;
}

// Accumulated pace time, used to avoid starting very short pace timers
//  this can save some complexity at very higfh bitrates
float accumulatedPaceTime = 0.0f;

/*
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier            |
+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
|            contributing source -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
void parseRtp(unsigned char* buf, uint16_t* seqNr, uint32_t* timeStamp, unsigned char* pt) {
	uint16_t rawSeq;
	uint32_t rawTs;
	memcpy(&rawSeq, buf + 2, 2);
	memcpy(&rawTs, buf + 4, 4);
	memcpy(pt, buf + 1, 1);
	*seqNr = ntohs(rawSeq);
	*timeStamp = ntohl(rawTs);
}

void writeRtp(unsigned char* buf, uint16_t seqNr, uint32_t timeStamp, unsigned char pt) {
	seqNr = htons(seqNr);
	timeStamp = htonl(timeStamp);
	uint32_t tmp = htonl(SSRC);
	memcpy(buf + 2, &seqNr, 2);
	memcpy(buf + 4, &timeStamp, 4);
	memcpy(buf + 8, &tmp, 4);
	memcpy(buf + 1, &pt, 1);
	buf[0] = 0x80;
}


void sendPacket(void* buf, int size) {
	if (ipv6)
		sendto(fd_outgoing_rtp, buf, size, 0, (struct sockaddr*)&outgoing_rtp_addr6, sizeof(outgoing_rtp_addr6));
	else
		sendto(fd_outgoing_rtp, buf, size, 0, (struct sockaddr*)&outgoing_rtp_addr, sizeof(outgoing_rtp_addr));
}

/*
* Transmit a packet if possible.
* If not allowed due to packet pacing restrictions,
* then start a timer.
*/
void* transmitRtpThread(void* arg) {
	int size;
	uint16_t seqNr;
	uint32_t ts;
	bool isMark;
	char buf[2000];
	uint32_t time_ntp = getTimeInNtp();
	float retVal = 0.0f;
	struct timeval start, end;
	useconds_t diff = 0;

	accumulatedPaceTime = 0.0;
	int nTx = 0;

	for (;;) {
		if (stopThread) {
			return NULL;
		}
		retVal = -1.0f;

		while (retVal == -1.0f) {
			pthread_mutex_lock(&lock_scream);
			time_ntp = getTimeInNtp();
			retVal = screamTx->isOkToTransmit(time_ntp, SSRC);
			pthread_mutex_unlock(&lock_scream);
			if (retVal == -1.0f) {
				usleep(10);
				nTx = 0;
			}

		}
		

		
		if (accumulatedPaceTime > minPaceIntervalUs * 1e-6) {
			diff = 100;
			if (accumulatedPaceTime > 1.1 * minPaceIntervalUs * 1e-6)
				usleep(std::max(10, (int)(accumulatedPaceTime * 1e6 - diff)));
			else
				usleep(std::max(10u, minPaceIntervalUs - diff));
			gettimeofday(&end, 0);
			diff = end.tv_usec - start.tv_usec;
			accumulatedPaceTime = 0.0f;
			nTx = 0;
		}


		time_ntp = getTimeInNtp();
		void* buf;
		uint32_t ssrc_unused;

		pthread_mutex_lock(&lock_rtp_queue);
		float rtpQueueDelay = 0.0f;
		rtpQueueDelay = rtpQueue->getDelay((time_ntp) / 65536.0f);
		rtpQueue->pop(&buf, size, ssrc_unused, seqNr, isMark, ts);
		sendPacket(buf, size);
		nTx++;
		pthread_mutex_unlock(&lock_rtp_queue);

		packet_free(buf, SSRC);
		buf = NULL;

		pthread_mutex_lock(&lock_scream);
		time_ntp = getTimeInNtp();
		retVal = screamTx->addTransmitted(time_ntp, SSRC, size, seqNr, isMark, rtpQueueDelay, ts);
		pthread_mutex_unlock(&lock_scream);

		if (!disablePacing && retVal > 0.0) {
			accumulatedPaceTime += retVal;
		}
	}
	return NULL;
}

static int makePeriodic(unsigned int period, struct periodicInfo* info)
{
	int ret;
	unsigned int ns;
	unsigned int sec;
	int fd;
	struct itimerspec itval;

	/* Create the timer */
	fd = timerfd_create(CLOCK_MONOTONIC, 0);
	info->wakeupsMissed = 0;
	info->timer_fd = fd;
	if (fd == -1)
		return fd;

	/* Make the timer periodic */
	sec = period / 1000000;
	ns = (period - (sec * 1000000)) * 1000;
	itval.it_interval.tv_sec = sec;
	itval.it_interval.tv_nsec = ns;
	itval.it_value.tv_sec = sec;
	itval.it_value.tv_nsec = ns;
	ret = timerfd_settime(fd, 0, &itval, NULL);
	return ret;
}

static void waitPeriod(struct periodicInfo* info)
{
	unsigned long long missed;
	int ret;

	/* Wait for the next timer event. If we have missed any the
	number is written to "missed" */
	ret = read(info->timer_fd, &missed, sizeof(missed));
	if (ret == -1)
	{
		perror("read timer");
		return;
	}

	/* "missed" should always be >= 1, but just to be sure, check it is not 0 anyway */
	if (missed > 0)
		info->wakeupsMissed += (missed - 1);
}

void* createRtpThread(void* arg) {
	uint32_t keyFrameInterval_ntp = (uint32_t)(keyFrameInterval * 65536.0f);
	float rateScale = 1.0f;
	if (isKeyFrame) {
		rateScale = 1.0f + 0.0 * keyFrameSize / (FPS * keyFrameInterval) / 2.0;
		rateScale = (1.0f / rateScale);
	}
	uint32_t dT_us = (uint32_t)(1e6 / FPS);
	unsigned char PT = 98;
	struct periodicInfo info;

	makePeriodic(dT_us, &info);

	/*
	* Infinite loop that generates RTP packets
	*/
	for (;;) {
		if (stopThread) {
			return NULL;
		}
		uint32_t time_ntp = getTimeInNtp();

		uint32_t ts = (uint32_t)(time_ntp / 65536.0 * 90000);
		float rateTx = screamTx->getTargetBitrate(time_ntp, SSRC) * rateScale;

		mtu = screamTx->getRecommendedMss(time_ntp);

		screamTx->setCwndMinLow((mtu+12)*2);

		float randVal = float(rand()) / RAND_MAX - 0.5;
		int bytes = (int)(rateTx / FPS / 8 * (1.0 + randVal * randRate));

		if (isKeyFrame && time_ntp - lastKeyFrameT_ntp >= keyFrameInterval_ntp) {
			/*
			* Fake a key frame
			*/
			bytes = (int)(bytes * keyFrameSize);
			lastKeyFrameT_ntp = time_ntp;
		}

		if (!pushTraffic && burstTime < 0) {
			float time_s = time_ntp / 65536.0f;
			if (burstStartTime < 0) {
				burstStartTime = time_s;
				isBurst = true;
			}
			if (time_s > burstStartTime + burstTime && isBurst) {
				isBurst = false;
				burstSleepTime = time_s;
			}
			if (time_s > burstSleepTime + burstSleep && !isBurst) {
				isBurst = true;
				burstStartTime = time_s;
			}
			if (!isBurst)
				bytes = 0;
		}

		while (bytes > 0) {
			int pl_size = min(bytes, mtu);
			int recvlen = pl_size + 12;

			bytes = std::max(0, bytes - pl_size);
			unsigned char pt = PT;
			bool isMark;
			if (bytes == 0) {
				// Last RTP packet, set marker bit
				pt |= 0x80;
				isMark = true;
			}
			else {
				isMark = false;
			}
			uint8_t* buf_rtp = (uint8_t*)malloc(BUFSIZE);
			writeRtp(buf_rtp, seqNr, ts, pt);

			if (pushTraffic) {
				sendPacket(buf_rtp, recvlen);
				packet_free(buf_rtp, SSRC);
				buf_rtp = NULL;
			}
			else {
				pthread_mutex_lock(&lock_rtp_queue);
				rtpQueue->push(buf_rtp, recvlen, SSRC, seqNr, isMark, (time_ntp) / 65536.0f, ts);
				pthread_mutex_unlock(&lock_rtp_queue);

				pthread_mutex_lock(&lock_scream);
				time_ntp = getTimeInNtp();
				screamTx->newMediaFrame(time_ntp, SSRC, recvlen, isMark);
				pthread_mutex_unlock(&lock_scream);
			}
			seqNr++;
		}
		waitPeriod(&info);

	}

	return NULL;
}

uint32_t rtcp_rx_time_ntp = 0;
#define KEEP_ALIVE_PKT_SIZE 1
void* readRtcpThread(void* arg) {
	/*
	* Wait for RTCP packets from receiver
	*/
	for (;;) {
		int recvlen = 0;
		if (ipv6) {
			recvlen = recvfrom(fd_outgoing_rtp, buf_rtcp, BUFSIZE, 0, (struct sockaddr*)&incoming_rtcp_addr6, &addrlen_incoming_rtcp6);
		}
		else {
			recvlen = recvfrom(fd_outgoing_rtp, buf_rtcp, BUFSIZE, 0, (struct sockaddr*)&incoming_rtcp_addr, &addrlen_incoming_rtcp);
		}
		if (stopThread)
			return NULL;
		if (recvlen > KEEP_ALIVE_PKT_SIZE) {
			pthread_mutex_lock(&lock_scream);
			uint32_t time_ntp = getTimeInNtp(); // We need time in microseconds, roughly ms granularity is OK
			char s[100];
			if (ntp) {
				struct timeval tp;
				gettimeofday(&tp, NULL);
				double time = tp.tv_sec + tp.tv_usec * 1e-6;
				sprintf(s, "%1.6f", time);
			}
			else {
				sprintf(s, "%1.4f", time_ntp / 65536.0f);
			}
			screamTx->setTimeString(s);

			screamTx->incomingStandardizedFeedback(time_ntp, buf_rtcp, recvlen);

			pthread_mutex_unlock(&lock_scream);
			rtcp_rx_time_ntp = time_ntp;
		}
		usleep(10);
	}
	return NULL;
}

int setup() {
	if (ipv6) {
		outgoing_rtp_addr6.sin6_family = AF_INET6;
		//inet_aton(DECODER_IP, (in_addr*)&outgoing_rtp_addr6.sin6_addr.s6_addr);
		inet_pton(AF_INET6, DECODER_IP, &outgoing_rtp_addr6.sin6_addr);
		outgoing_rtp_addr6.sin6_port = htons(DECODER_PORT);
		addrlen_outgoing_rtp = sizeof(outgoing_rtp_addr6);

		incoming_rtcp_addr6.sin6_family = AF_INET6;
		incoming_rtcp_addr6.sin6_port = htons(DECODER_PORT);
		incoming_rtcp_addr6.sin6_addr = in6addr_any;

		dummy_rtcp_addr6.sin6_family = AF_INET6;
		inet_aton(DUMMY_IP, (in_addr*)&dummy_rtcp_addr6.sin6_addr.s6_addr);
		dummy_rtcp_addr6.sin6_port = htons(DECODER_PORT);

		cerr << DECODER_IP << endl;

		//return 0;
		if ((fd_outgoing_rtp = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
			perror("cannot create socket");
			return 0;
		}


		if (ifname != 0) {
			const int len = strlen(ifname);
			if (setsockopt(fd_outgoing_rtp, SOL_SOCKET, SO_BINDTODEVICE, ifname, len) < 0) {
				perror("setsockopt(SO_BINDTODEVICE) failed");
				return 0;
			}
		}

		if (bind(fd_outgoing_rtp, (struct sockaddr*)&incoming_rtcp_addr6, sizeof(incoming_rtcp_addr6)) < 0) {
			perror("bind outgoing_rtp_addr failed");
			return 0;
		}
		else {
			if (!pushTraffic)
				cerr << "Listen on port " << DECODER_PORT << " to receive RTCP from decoder " << endl;
		}

	}
	else {
		outgoing_rtp_addr.sin_family = AF_INET;
		inet_aton(DECODER_IP, (in_addr*)&outgoing_rtp_addr.sin_addr.s_addr);
		outgoing_rtp_addr.sin_port = htons(DECODER_PORT);
		addrlen_outgoing_rtp = sizeof(outgoing_rtp_addr);

		incoming_rtcp_addr.sin_family = AF_INET;
		incoming_rtcp_addr.sin_port = htons(DECODER_PORT);
		incoming_rtcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);

		dummy_rtcp_addr.sin_family = AF_INET;
		inet_aton(DUMMY_IP, (in_addr*)&dummy_rtcp_addr.sin_addr.s_addr);
		dummy_rtcp_addr.sin_port = htons(DECODER_PORT);

		if ((fd_outgoing_rtp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			perror("cannot create socket");
			return 0;
		}

		if (ifname != 0) {
			const int len = strlen(ifname);
			if (setsockopt(fd_outgoing_rtp, SOL_SOCKET, SO_BINDTODEVICE, ifname, len) < 0) {
				perror("setsockopt(SO_BINDTODEVICE) failed");
				return 0;
			}
		}

		if (bind(fd_outgoing_rtp, (struct sockaddr*)&incoming_rtcp_addr, sizeof(incoming_rtcp_addr)) < 0) {
			perror("bind outgoing_rtp_addr failed");
			return 0;
		}
		else {
			if (!pushTraffic)
				cerr << "Listen on port " << DECODER_PORT << " to receive RTCP from decoder " << endl;
		}

	}


	/*
	* Set ECN capability for outgoing socket using IP_TOS
	*/
#ifdef ECN_CAPABLE
	int iptos = 0;
	if (ect == 0 || ect == 1)
		iptos = 2 - ect;
	if (ect == 3)
		iptos = 3;
	int res;
	if (ipv6)
		res = setsockopt(fd_outgoing_rtp, IPPROTO_IPV6, IPV6_TCLASS, &iptos, sizeof(iptos));
	else
		res = setsockopt(fd_outgoing_rtp, IPPROTO_IP, IP_TOS, &iptos, sizeof(iptos));

	if (res < 0) {
		cerr << "Not possible to set ECN bits" << endl;
	}
	int tmp = 0;
#endif
	char buf[10];
	if (fixedRate > 0) {
		screamTx = new ScreamV2Tx(
			1.0f,
			1.0f,
			delayTarget,
			(mtu+12)*2,
			1.5f,
			1.5f,
			2.0f,
			0.05f,
			ect == 1,
			5.0,
			false,
			enableClockDriftCompensation);
	}
	else {
		screamTx = new ScreamV2Tx(
			scaleFactor,
			scaleFactor,
			delayTarget,
			(initRate * 100) / 8,
			packetPacingHeadroom,
			adaptivePaceHeadroom,
			bytesInFlightHeadroom,
			multiplicativeIncreaseFactor,
			ect == 1,
			windowHeadroom,
			false,
			enableClockDriftCompensation);
	}
	rtpQueue = new RtpQueue();
	screamTx->setCwndMinLow((mtu+12)*2);
	screamTx->enableRelaxedPacing(relaxedPacing);
	screamTx->setMssListMinPacketsInFlight(mtuList, nMtuListItems, minPktsInFlight);
	screamTx->setReorderTime(reorderTime);
	screamTx->enableDelayBasedCongestionControl(enableDelayBasedCc);

	if (disablePacing)
		screamTx->enablePacketPacing(false);


	if (fixedRate > 0) {
		screamTx->registerNewStream(rtpQueue,
			SSRC,
			1.0f,
			fixedRate * 1000.0f,
			fixedRate * 1000.0f,
			fixedRate * 1000.0f,
			10.0f,
			false,
			hysteresis);
	}
	else {
		screamTx->registerNewStream(rtpQueue,
			SSRC,
			1.0f,
			minRate * 1000,
			initRate * 1000,
			maxRate * 1000,
			0.2f,
			false,
			hysteresis);
	}
	return 1;
}

uint32_t lastT_ntp;

volatile sig_atomic_t done = 0;

void stopAll(int signum)
{
	stopThread = true;
}

int main(int argc, char* argv[]) {

    mtuList[0] = mtu;
	struct timeval tp;
	gettimeofday(&tp, NULL);
	t0 = tp.tv_sec + tp.tv_usec * 1e-6 - 1e-3;
	lastT_ntp = getTimeInNtp();

	/*
	* Parse command line
	*/
	if (argc <= 1) {
		cerr << "SCReAM V2 BW test tool, sender. Ericsson AB. Version 2025-09-28 " << endl;
		cerr << "Usage : " << endl << " > scream_bw_test_tx <options> decoder_ip decoder_port " << endl;
		cerr << "     -if name                 Bind to specific interface" << endl;
		cerr << "     -ipv6                    IPv6" << endl;
		cerr << "     -time val                Run for time seconds (default infinite)" << endl;
		cerr << "     -burst val1 val2         Burst media for a given time and then sleeps a given time" << endl;
		cerr << "         example -burst 1.0 0.2 burst media for 1s then sleeps for 0.2s " << endl;
		cerr << "     -nopace                  Disable packet pacing" << endl;
		cerr << "     -fixedrate val           Set a fixed 'coder' bitrate " << endl;
		cerr << "     -pushtraffic             just pushtraffic at a fixed bitrate, no feedback needed" << endl;
		cerr << "                                must be used with -fixedrate option" << endl;
		cerr << "     -key val1 val2           Set a given key frame interval [s] and size multiplier " << endl;
		cerr << "                               example -key 2.0 5.0 " << endl;
		cerr << "     -rand val                Framesizes vary randomly around the nominal " << endl;
		cerr << "                               example -rand 10 framesize vary +/- 10% " << endl;
		cerr << "     -initrate val            Set a start bitrate [kbps]" << endl;
		cerr << "                               example -initrate 2000 " << endl;
		cerr << "     -minrate  val            Set a min bitrate [kbps], default 1000kbps" << endl;
		cerr << "                               example -minrate 1000 " << endl;
		cerr << "     -maxrate val             Set a max bitrate [kbps], default 200000kbps" << endl;
		cerr << "                               example -maxrate 10000 " << endl;
		cerr << "     -ect n                   ECN capable transport, n = 0 or 1 for ECT(0) or ECT(1)," << endl;
		cerr << "                               -1 for not-ECT (default)" << endl;
		cerr << "     -scale value             Scale factor in case of loss or ECN event (default 0.7) " << endl;
		cerr << "     -delaytarget val         Set a queue delay target (default = 0.06s) " << endl;
		cerr << "     -nodelaycc               Disable delay based congestion control " << endl;
		cerr << "     -paceheadroom val        Set a packet pacing headroom (default = 1.5) " << endl;
		cerr << "     -windowheadroom val      How much bytes in flight can exceed cwnd  (default = 5.0) " << endl;
		cerr << "     -adaptivepaceheadroom val Set adaptive packet pacing headroom (default = 1.5) " << endl;
		cerr << "     -relaxedpacing           Allow increased pacing rate when max rate reached (default = false) " << endl;
		cerr << "     -inflightheadroom val    Set a bytes in flight headroom (default = 2.0) " << endl;
		cerr << "     -mulincrease val         Multiplicative increase factor for (default 0.05)" << endl;
		cerr << "     -fps value               Set the frame rate (default 50)" << endl;
		cerr << "     -clockdrift              Enable clock drift compensation for the case that the" << endl;
		cerr << "                               receiver end clock is faster" << endl;
		cerr << "     -verbose                 Print a more extensive log" << endl;
		cerr << "     -nosummary               Don't print summary" << endl;
		cerr << "     -log logfile             Save detailed per-ACK log to file" << endl;
		cerr << "     -txrxlog logfile         Save tx and rx timestamp for RTP packets" << endl;
		cerr << "     -ntp                     Use NTP timestamp in logfile" << endl;
		cerr << "     -append                  Append logfile" << endl;
		cerr << "     -mtu values              List of mtu values separated by , without space"  << endl;
		cerr << "                               list should be in increasing order (default 1200)" << endl;
		cerr << "     -minpktsinflight val     Min pkts in flight (default 0)" << endl;
		cerr << "     -itemlist                Add item list in beginning of log file" << endl;
		cerr << "     -detailed                Detailed log, per ACKed RTP" << endl;
		cerr << "     -microburstinterval val  Microburst interval [ms] for packet pacing (default 0.5ms)" << endl;
		cerr << "     -hysteresis  val         Inhibit updated target rate to encoder if the rate change is small" << endl;
		cerr << "                               a value of 0.1 means a hysteresis of +10%/-2.5%" << endl;
		cerr << "     -reordertime val         Set packet reordering margin [s] (default 0.03)" << endl;

		exit(-1);
	}
	int ix = 1;
	bool verbose = false;
	char* logFile = 0;
	char* txRxLogFile = 0;
	/* First find options */
	while (strstr(argv[ix], "-")) {
		if (strstr(argv[ix], "-ect")) {
			ect = atoi(argv[ix + 1]);
			ix += 2;
			if (!(ect == 1 || ect == 0 || ect == 1 || ect == 3)) {
				cerr << "ect must be -1, 0, 1 or 3 " << endl;
				exit(0);

			}
			continue;
		}
		if (strstr(argv[ix], "-ipv6")) {
			ipv6 = true;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-time")) {
			runTime = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-scale")) {
			scaleFactor = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-delaytarget")) {
			delayTarget = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-nodelaycc")) {
			enableDelayBasedCc = false;
			ix ++;
			continue;
		}
		if (strstr(argv[ix], "-paceheadroom")) {
			packetPacingHeadroom = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}

		if (strstr(argv[ix], "-adaptivepaceheadroom")) {
			adaptivePaceHeadroom = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}

		if (strstr(argv[ix], "-inflightheadroom")) {
			bytesInFlightHeadroom = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}

		if (strstr(argv[ix], "-mtu")) {
			char s[100];
			strcpy(s,argv[ix + 1]);
            char *t = strtok(s,",");
            nMtuListItems = 0;
            cerr << t << endl;
            mtuList[nMtuListItems++] = atoi(t);
            while (t != 0) {
            	t = strtok(0,",");
            	if (t != 0) {
                   mtuList[nMtuListItems++] = atoi(t);
            	}
            }

            mtu = mtuList[0];

			ix += 2;
			continue;
		}

		if (strstr(argv[ix], "-minpktsinflight")) {
			minPktsInFlight = atoi(argv[ix + 1]);
			ix += 2;
			continue;
		}

		if (strstr(argv[ix], "-fixedrate")) {
			fixedRate = atoi(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-burst")) {
			burstTime = atof(argv[ix + 1]);
			burstSleep = atof(argv[ix + 2]);
			ix += 3;
			continue;
		}
		if (strstr(argv[ix], "-key")) {
			isKeyFrame = true;
			keyFrameInterval = atof(argv[ix + 1]);
			keyFrameSize = atof(argv[ix + 2]);
			ix += 3;
			continue;
		}
		if (strstr(argv[ix], "-windowheadroom")) {
			windowHeadroom = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-nopace")) {
			disablePacing = true;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-relaxedpacing")) {
			relaxedPacing = true;
			ix++;
			continue;
		}

		if (strstr(argv[ix], "-reordertime")) {
			reorderTime = atof(argv[ix + 1]);;
			ix+=2;
			continue;
		}

		if (strstr(argv[ix], "-fps")) {
			FPS = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-rand")) {
			randRate = atof(argv[ix + 1]) / 100.0;
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-initrate")) {
			initRate = atoi(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-minrate")) {
			minRate = atoi(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-maxrate")) {
			maxRate = atoi(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-verbose")) {
			verbose = true;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-nosummary")) {
			printSummary = false;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-log")) {
			logFile = argv[ix + 1];
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-txrxlog")) {
			txRxLogFile = argv[ix + 1];
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-ntp")) {
			ntp = true;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-append")) {
			append = true;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-itemlist")) {
			itemlist = true;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-detailed")) {
			detailed = true;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-pushtraffic")) {
			pushTraffic = true;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-clockdrift")) {
			enableClockDriftCompensation = true;
			ix++;
			continue;
		}
		if (strstr(argv[ix], "-if")) {
			ifname = argv[ix + 1];
			ix += 2;
			continue;
		}

		if (strstr(argv[ix], "-microburstinterval")) {
			minPaceInterval = 0.001 * (atof(argv[ix + 1]));
			minPaceIntervalUs = (int)(minPaceInterval * 1e6f);
			ix += 2;
			if (minPaceInterval < 0.0002f || minPaceInterval > 0.020f) {
				cerr << "microburstinterval must be in range 0.2..20ms" << endl;
				exit(0);
			}
			continue;
		}
		if (strstr(argv[ix], "-hysteresis")) {
			hysteresis = atof(argv[ix + 1]);
			ix += 2;
			if (hysteresis < 0.0f || hysteresis > 0.2f) {
				cerr << "hysteresis must be in range 0.0...0.2" << endl;
				exit(0);
			}
			continue;
		}
		
		if (strstr(argv[ix], "-mulincrease")) {
			multiplicativeIncreaseFactor = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}
		cerr << "unexpected arg " << argv[ix] << endl;
		exit(0);
	}


	if (pushTraffic && fixedRate == 0) {
		cerr << "Error : pushtraffic can only be used with fixedrate" << endl;
		exit(-1);
	}
	if (logFile) {
		if (append)
			fp_log = fopen(logFile, "a");
		else
			fp_log = fopen(logFile, "w");
	}
	if (txRxLogFile) {
		if (append)
			fp_txrxlog = fopen(txRxLogFile, "a");
		else
			fp_txrxlog = fopen(txRxLogFile, "w");
	}
	if (minRate > initRate)
		initRate = minRate;
	DECODER_IP = argv[ix];ix++;
	DECODER_PORT = atoi(argv[ix]);ix++;

	if (setup() == 0)
		return 0;

	if (logFile && !append && itemlist) {
		fprintf(fp_log, "%s\n", screamTx->getDetailedLogItemList());
	}

	struct sigaction action;
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = stopAll;
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);

	screamTx->setDetailedLogFp(fp_log);
	screamTx->useExtraDetailedLog(detailed);
	screamTx->setTxRxLogFp(fp_txrxlog);

	pthread_mutex_init(&lock_scream, NULL);
	pthread_mutex_init(&lock_rtp_queue, NULL);
	pthread_mutex_init(&lock_pace, NULL);

	/* Create RTP thread */
	pthread_create(&create_rtp_thread, NULL, createRtpThread, (void*)"Create RTP thread...");
	if (pushTraffic) {
		cerr << "Scream sender started in push traffic mode " << fixedRate << "kbps" << endl;

		while (!stopThread && (runTime < 0 || getTimeInNtp() < runTime * 65536.0f)) {
			usleep(50000);
		}
		stopThread = true;

	}
	else {
		cerr << "Scream sender started! " << endl;

		/* Create RTCP thread */
		pthread_create(&rtcp_thread, NULL, readRtcpThread, (void*)"RTCP thread...");
		/* Transmit RTP thread */
		pthread_create(&transmit_rtp_thread, NULL, transmitRtpThread, (void*)"Transmit RTP thread...");

		while (!stopThread && (runTime < 0 || getTimeInNtp() < runTime * 65536.0f)) {
			uint32_t time_ntp = getTimeInNtp();
			bool isFeedback = time_ntp - rtcp_rx_time_ntp < 65536; // 1s in Q16
			if ((printSummary || !isFeedback) && time_ntp - lastLogT_ntp > 2 * 65536) { // 2s in Q16
				if (!isFeedback) {
					cerr << "No RTCP feedback received" << endl;
				}
				else {
					float time_s = time_ntp / 65536.0f;
					char s[500];
					screamTx->getStatistics(time_s, s);

					cout << s << ", MTU = " << mtu <<endl;
				}
				lastLogT_ntp = time_ntp;
			}
			if (verbose && time_ntp - lastLogTv_ntp > 13107) { // 0.2s in Q16
				if (isFeedback) {
					float time_s = time_ntp / 65536.0f;
					char s[3000];
					char s1[500];
					screamTx->getVeryShortLog(time_s, s1);

					sprintf(s, "%8.3f, %s ", time_s, s1);

					cout << s << endl;
					/*
					* Send statistics to receiver this can be used to
					* verify reliability of remote control
					*/
					s1[0] = 0x80;
					s1[1] = 0x7F; // Set PT = 0x7F for statistics packet
					memcpy(&s1[2], s, strlen(s));
					sendPacket(s1, strlen(s) + 2);
				}
				lastLogTv_ntp = time_ntp;
			}
			usleep(50000);
		};
		stopThread = true;
	}
	usleep(500000);
	close(fd_outgoing_rtp);
	if (fp_log)
		fclose(fp_log);
	if (fp_txrxlog)
		fclose(fp_txrxlog);
	screamTx->printFinalSummary();
}
