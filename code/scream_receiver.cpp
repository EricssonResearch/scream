// Scream sender side wrapper
#include "ScreamRx.h"
#include "sys/socket.h"
#include "sys/types.h"
#include "netinet/in.h"
#include <string.h> /* needed for memset */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
using namespace std;

#define BUFSIZE 2048
#define ECN_CAPABLE
#define IS_STANDARD_FEEDBACK

/*
* Simple code to verify that L4S works
* as intended
* The intention is that
* CWND = 2*mss/pMark [byte]
* The bitrate should then become
* rate = 2*mss*8/(pMark*RTT)
*/
//#define TEST_L4S
double pMarkCarry = 0.0;
#define PMARK 0.1;

// Scream receiver side wrapper

int fd_incoming_rtp;
// Input UDP socket, RTP packets come here and we send RTCP packets in the
// reverse direction through this socket

int fd_outgoing_rtcp;
ScreamRx* screamRx = 0;

//int fd_local_rtp;

const char* SENDER_IP = "192.168.0.20";
int INCOMING_RTP_PORT = 30122;
struct sockaddr_in incoming_rtp_addr, outgoing_rtcp_addr, sender_rtcp_addr;
struct sockaddr_in local_rtp_addr;

struct sockaddr_in6 incoming_rtp_addr6, outgoing_rtcp_addr6, sender_rtcp_addr6;
struct sockaddr_in6 local_rtp_addr6;
const char* LOCAL_IP = "127.0.0.1";
const char* LOCAL_IPV6 = "::1";
int LOCAL_PORT = 30124;

int ackDiff = -1;
int nReportedRtpPackets = 64;

char* ifname = 0;

int nPrint = 0;
pthread_mutex_t lock_scream;
double t0 = 0;

bool ipv6 = false;

/*
* Time in 32 bit NTP format
* 16 most  significant bits are seconds
* 16 least significant bits is fraction
*/
uint32_t getTimeInNtp() {
	struct timeval tp;
	gettimeofday(&tp, NULL);
	double time = (tp.tv_sec + tp.tv_usec * 1e-6) - t0;
	uint64_t ntp64 = uint64_t(time * 65536);
	uint32_t ntp = 0xFFFFFFFF & (ntp64); // NTP in Q16
	return ntp;
}

/*
Extract the sequence number and the timestamp from the RTP header
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier            |
+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
|            contributing source (CSRC) identifiers             |
|                             ....                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
void parseRtp(unsigned char* buf, uint16_t* seqNr, uint32_t* timeStamp) {
	uint16_t rawSeq;
	uint32_t rawTs;
	memcpy(&rawSeq, buf + 2, 2);
	memcpy(&rawTs, buf + 4, 4);
	*seqNr = ntohs(rawSeq);
	*timeStamp = ntohl(rawTs);
}

uint16_t lastFbSn = 0;
uint64_t lastPunchNatT_ntp = 0;
uint32_t SSRC = 100; // We don't bother with SSRC yet, this is for experiments only so far.
#define KEEP_ALIVE_PKT_SIZE 1

void* rtcpPeriodicThread(void* arg) {
	unsigned char buf[BUFSIZE];
	int rtcpSize;
	uint32_t rtcpFbInterval_ntp = screamRx->getRtcpFbInterval();
	for (;;) {
		if (getTimeInNtp() - lastPunchNatT_ntp > 32768) { // 500ms in Q16
			/*
			* Send a small packet just to punch open a hole in the NAT,
			*  just one single byte will do.
			* This makes in possible to receive packets on the same port
			*/
			if (ipv6) {
				int ret = sendto(fd_incoming_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr*)&outgoing_rtcp_addr6, sizeof(outgoing_rtcp_addr6));
			}
			else {
				int ret = sendto(fd_incoming_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr*)&outgoing_rtcp_addr, sizeof(outgoing_rtcp_addr));
			}
			lastPunchNatT_ntp = getTimeInNtp();
			cerr << "." << endl;
		}
		uint32_t time_ntp = getTimeInNtp();
		rtcpFbInterval_ntp = screamRx->getRtcpFbInterval();
		if (screamRx->isFeedback(time_ntp) &&
			(screamRx->checkIfFlushAck() ||
				(time_ntp - screamRx->getLastFeedbackT() > rtcpFbInterval_ntp))) {
			pthread_mutex_lock(&lock_scream);
			bool isFeedback = screamRx->createStandardizedFeedback(getTimeInNtp(), true, buf, rtcpSize);
			pthread_mutex_unlock(&lock_scream);
			if (isFeedback) {
				if (ipv6) {
					sendto(fd_incoming_rtp, buf, rtcpSize, 0, (struct sockaddr*)&outgoing_rtcp_addr6, sizeof(outgoing_rtcp_addr6));
				}
				else {
					sendto(fd_incoming_rtp, buf, rtcpSize, 0, (struct sockaddr*)&outgoing_rtcp_addr, sizeof(outgoing_rtcp_addr));
				}
				lastPunchNatT_ntp = getTimeInNtp();
			}
		}
		usleep(500);
	}
}

int main(int argc, char* argv[])
{
	unsigned char buf[BUFSIZE];
	unsigned char buf_rtcp[BUFSIZE];
	if (argc <= 1) {
		cerr << "SCReAM BW test tool, receiver. Ericsson AB. Version 2025-05-09 " << endl;
		cerr << "Usage :" << endl << " > scream_bw_test_rx <options> sender_ip sender_port" << endl;
		cerr << "     -ipv6               IPv6" << endl;
		cerr << "     -ackdiff            set the max distance in received RTPs to send an ACK " << endl;
		cerr << "     -nreported          set the number of reported RTP packets per ACK " << endl;
		cerr << "     -if name            bind to specific interface" << endl;
		exit(-1);
	}

	int ix = 1;

	while (strstr(argv[ix], "-")) {
		if (strstr(argv[ix], "-ipv6")) {
			ipv6 = true;
			ix++;
			continue;
		}

		if (argc > (ix + 1) && strstr(argv[ix], "-ackdiff")) {
			ackDiff = atoi(argv[ix + 1]);
			ix += 2;
		}

		if (argc > (ix + 1) && strstr(argv[ix], "-nreported")) {
			nReportedRtpPackets = atoi(argv[ix + 1]);
			ix += 2;
		}

		if (argc > (ix + 1) && strstr(argv[ix], "-if")) {
			ifname = argv[ix + 1];
			ix += 2;
		}
	}

	if (argc > (ix + 1)) {
		SENDER_IP = argv[ix];
		INCOMING_RTP_PORT = atoi(argv[ix + 1]);
	}
	else {
		cerr << "Insufficient parameters." << endl;
		exit(-1);
	}


	struct timeval tp;
	gettimeofday(&tp, NULL);
	t0 = (tp.tv_sec + tp.tv_usec * 1e-6) - 1e-3;

	screamRx = new ScreamRx(10, ackDiff, nReportedRtpPackets);

	if (ipv6) {
		incoming_rtp_addr6.sin6_family = AF_INET6;
		incoming_rtp_addr6.sin6_addr = in6addr_any;
		incoming_rtp_addr6.sin6_port = htons(INCOMING_RTP_PORT);

		outgoing_rtcp_addr6.sin6_family = AF_INET6;
		inet_pton(AF_INET6, SENDER_IP, &outgoing_rtcp_addr6.sin6_addr);
		//inet_aton(SENDER_IP.c_str(), (in_addr*)&outgoing_rtcp_addr.sin_addr.s_addr);
		outgoing_rtcp_addr6.sin6_port = htons(INCOMING_RTP_PORT);


		local_rtp_addr6.sin6_family = AF_INET6;
		inet_pton(AF_INET6, LOCAL_IPV6, &local_rtp_addr6.sin6_addr);
		//inet_aton(LOCAL_IP.c_str(), (in_addr*)&local_rtp_addr.sin_addr.s_addr);
		local_rtp_addr6.sin6_port = htons(LOCAL_PORT);

		if ((fd_incoming_rtp = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
			perror("cannot create socket for incoming RTP packets");
			return 0;
		}
	}
	else {
		incoming_rtp_addr.sin_family = AF_INET;
		incoming_rtp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		incoming_rtp_addr.sin_port = htons(INCOMING_RTP_PORT);

		outgoing_rtcp_addr.sin_family = AF_INET;
		inet_aton(SENDER_IP, (in_addr*)&outgoing_rtcp_addr.sin_addr.s_addr);
		outgoing_rtcp_addr.sin_port = htons(INCOMING_RTP_PORT);


		local_rtp_addr.sin_family = AF_INET;
		inet_aton(LOCAL_IP, (in_addr*)&local_rtp_addr.sin_addr.s_addr);
		local_rtp_addr.sin_port = htons(LOCAL_PORT);

		if ((fd_incoming_rtp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			perror("cannot create socket for incoming RTP packets");
			return 0;
		}

	}

	if (ifname != 0) {
		const int len = strlen(ifname);
		if (setsockopt(fd_incoming_rtp, SOL_SOCKET, SO_BINDTODEVICE, ifname, len) < 0) {
			perror("setsockopt(SO_BINDTODEVICE) failed");
			return 0;
		}
	}

	int enable = 1;
	if (setsockopt(fd_incoming_rtp, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
		perror("setsockopt(SO_REUSEADDR) failed");
	}


	if (ipv6) {
		int set = 1;
		if (setsockopt(fd_incoming_rtp, IPPROTO_IPV6, IPV6_RECVTCLASS, &set, sizeof(set)) < 0) {
			cerr << "cannot set IPV6_RECVTCLASS on incoming socket" << endl;
			return 0;
		}
		else {
			cerr << "socket set to IPV6_RECVTCLASS" << endl;
		}
	}
	else {
		unsigned char set = 0x03;
		if (setsockopt(fd_incoming_rtp, IPPROTO_IP, IP_RECVTOS, &set, sizeof(set)) < 0) {
			cerr << "cannot set IP_RECVTOS on incoming socket" << endl;
			return 0;
		}
		else {
			cerr << "socket set to IP_RECVTOS" << endl;
		}
	}

	uint64_t recv_buf_size = 1024 * 1024 * 20; //20MByte
	if (setsockopt(fd_incoming_rtp, SOL_SOCKET, SO_RCVBUF, &recv_buf_size, sizeof(recv_buf_size)) < 0) {
		cerr << "cannot set SO_RCVBUF on incoming socket" << endl;
	}
	else {
		cerr << "socket SO_RCVBUF set to " << recv_buf_size << endl;
	}

	if (ipv6) {
		if (::bind(fd_incoming_rtp, (struct sockaddr*)&incoming_rtp_addr6, sizeof(incoming_rtp_addr6)) < 0) {
			perror("bind incoming_rtp_addr failed");
			return 0;
		}
		else {
			cerr << "Listen on port " << INCOMING_RTP_PORT << " to receive RTP from sender " << endl;
		}
	}
	else {
		if (::bind(fd_incoming_rtp, (struct sockaddr*)&incoming_rtp_addr, sizeof(incoming_rtp_addr)) < 0) {
			perror("bind incoming_rtp_addr failed");
			return 0;
		}
		else {
			cerr << "Listen on port " << INCOMING_RTP_PORT << " to receive RTP from sender " << endl;
		}
	}

	struct sockaddr_in sender_rtp_addr;
	socklen_t addrlen_sender_rtp_addr = sizeof(sender_rtp_addr);

	struct sockaddr_in6 sender_rtp_addr6;
	socklen_t addrlen_sender_rtp_addr6 = sizeof(sender_rtp_addr6);

	int recvlen;

	uint32_t last_received_time_ntp = 0;
	uint32_t receivedRtp = 0;

	/*
	* Send a small packet just to punmax distance in received RTPs to send an ACKch open a hole in the NAT,
	*  just one single byte will do.
	* This makes in possible to receive packets on the same port
	*/
	if (ipv6) {
		sendto(fd_incoming_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr*)&outgoing_rtcp_addr6, sizeof(outgoing_rtcp_addr6));
	}
	else {
		sendto(fd_incoming_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr*)&outgoing_rtcp_addr, sizeof(outgoing_rtcp_addr));
	}
	lastPunchNatT_ntp = getTimeInNtp();

	pthread_t rtcp_thread;
	pthread_create(&rtcp_thread, NULL, rtcpPeriodicThread, (void*)"Periodic RTCP thread...");

	uint16_t lastSn = 0;
	pthread_mutex_init(&lock_scream, NULL);


#define MAX_CTRL_SIZE 8192
#define MAX_BUF_SIZE 65536

	int* ecnptr;
	unsigned char received_ecn;

	struct msghdr rcv_msg;
	struct iovec rcv_iov[1];
	char rcv_ctrl_data[MAX_CTRL_SIZE];
	char rcv_buf[MAX_BUF_SIZE];

	/* Prepare message for receiving */
	rcv_iov[0].iov_base = rcv_buf;
	rcv_iov[0].iov_len = MAX_BUF_SIZE;

	rcv_msg.msg_name = NULL;	// Socket is connected
	rcv_msg.msg_namelen = 0;
	rcv_msg.msg_iov = rcv_iov;
	rcv_msg.msg_iovlen = 1;
	rcv_msg.msg_control = rcv_ctrl_data;
	rcv_msg.msg_controllen = MAX_CTRL_SIZE;

	for (;;) {
		//usleep(1);
		/*
		* Wait for incoing RTP packet, this call can be blocking
		*/

		/*
		* Extract ECN bits
		*/
		unsigned char received_ecn;
#ifdef ECN_CAPABLE
		int recvlen = recvmsg(fd_incoming_rtp, &rcv_msg, 0);
		if (recvlen == -1) {
			perror("recvmsg()");
			close(fd_incoming_rtp);
			return EXIT_FAILURE;
		}
		else {
			struct cmsghdr* cmptr;
			int* ecnptr;
			for (cmptr = CMSG_FIRSTHDR(&rcv_msg);
				cmptr != NULL;
				cmptr = CMSG_NXTHDR(&rcv_msg, cmptr)) {
				if (ipv6) {
					if (cmptr->cmsg_level == IPPROTO_IPV6 && cmptr->cmsg_type == IPV6_TCLASS) {
						ecnptr = (int*)CMSG_DATA(cmptr);
						received_ecn = *ecnptr;
					}
				}
				else {
					if (cmptr->cmsg_level == IPPROTO_IP && cmptr->cmsg_type == IP_TOS) {
						ecnptr = (int*)CMSG_DATA(cmptr);
						received_ecn = *ecnptr;
					}
				}
			}
			memcpy(buf, rcv_msg.msg_iov[0].iov_base, recvlen);
		}
#else
		if (ipv6) {
			recvlen = recvfrom(fd_incoming_rtp, buf, BUFSIZE, 0, (struct sockaddr*)&sender_rtp_addr6, &addrlen_sender_rtp_addr6);
		}
		else {
			recvlen = recvfrom(fd_incoming_rtp, buf, BUFSIZE, 0, (struct sockaddr*)&sender_rtp_addr, &addrlen_sender_rtp_addr);
		}
#endif
		uint32_t time_ntp = getTimeInNtp();
		if (recvlen > 1) {
			if (buf[1] == 0x7F) {
				// Packet contains statistics
				recvlen -= 2; // 2 bytes
				char s[1000];
				memcpy(s, &buf[2], recvlen);
				s[recvlen] = 0x00;
				cout << s << endl;
			}
			else {
				if (time_ntp - last_received_time_ntp > 2 * 65536) { // 2 sec in Q16
					/*
					* It's been more than 2 seconds since we last received an RTP packet
					*  let's reset everything to be on the safe side
					*/
					receivedRtp = 0;
					delete screamRx;
					screamRx = new ScreamRx(10, ackDiff, nReportedRtpPackets);
					cerr << "Receiver state reset due to idle input" << endl;
				}
				last_received_time_ntp = time_ntp;
				receivedRtp++;
				/*
				* Parse RTP header
				*/
				uint16_t seqNr;
				uint32_t ts;
				parseRtp(buf, &seqNr, &ts);
				bool isMark = (buf[1] & 0x80) != 0;
				uint16_t diff = seqNr - lastSn;
				if (diff > 1) {
					fprintf(stderr, "Packet(s) lost or reordered : %5d was received, previous rcvd is %5d \n", seqNr, lastSn);
				}
				lastSn = seqNr;
				/*
				* Generate RTCP feedback
				*/
				pthread_mutex_lock(&lock_scream);
#ifdef TEST_L4S
				if (received_ecn == 0x1) {
					pMarkCarry += PMARK;
					if (pMarkCarry >= 1.0) {
						pMarkCarry -= 1.0;
						received_ecn = 0x3;
					}
				}
#endif
				screamRx->receive(getTimeInNtp(), 0, SSRC, recvlen, seqNr, received_ecn, isMark,ts);
				pthread_mutex_unlock(&lock_scream);

				if (screamRx->checkIfFlushAck() || isMark) {
					pthread_mutex_lock(&lock_scream);
					int rtcpSize;
					bool isFeedback = screamRx->createStandardizedFeedback(getTimeInNtp(), isMark, buf_rtcp, rtcpSize);
					pthread_mutex_unlock(&lock_scream);
					if (isFeedback) {
						if (ipv6) {
							sendto(fd_incoming_rtp, buf_rtcp, rtcpSize, 0, (struct sockaddr*)&outgoing_rtcp_addr6, sizeof(outgoing_rtcp_addr6));
						}
						else {
							sendto(fd_incoming_rtp, buf_rtcp, rtcpSize, 0, (struct sockaddr*)&outgoing_rtcp_addr, sizeof(outgoing_rtcp_addr));

						}
				        while (screamRx->isOooDetected()) {
					        /*
					        * OOO RTP detected, additional "transmission" of RTCP
					        */
				        	pthread_mutex_lock(&lock_scream);					           
				        	bool isFeedbackOoo = screamRx->createStandardizedFeedbackOoo(getTimeInNtp(), false, buf_rtcp, rtcpSize);
				        	pthread_mutex_unlock(&lock_scream);
				        	if (isFeedbackOoo) {
				        		if (ipv6) {
				        			sendto(fd_incoming_rtp, buf_rtcp, rtcpSize, 0, (struct sockaddr*)&outgoing_rtcp_addr6, sizeof(outgoing_rtcp_addr6));
				        		}
				        		else {
				        			sendto(fd_incoming_rtp, buf_rtcp, rtcpSize, 0, (struct sockaddr*)&outgoing_rtcp_addr, sizeof(outgoing_rtcp_addr));

				        		}
				        	}
				        }
						lastPunchNatT_ntp = getTimeInNtp();
					}
				}
			}
		}
	}
}
