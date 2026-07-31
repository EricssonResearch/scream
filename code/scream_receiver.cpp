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
#include <atomic>
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

// Flag to track if we know the peer address (seeded or learned).
// std::atomic gives a release store / acquire load so that the write of
// outgoing_rtcp_addr in learn-from-source mode is visible before the flag is set.
std::atomic<bool> peer_known{ false };
// True only when the receiver was started without an explicit sender_ip, i.e.
// learn-from-source mode. Set once during argument parsing (before any thread
// starts) and read-only thereafter. Gates the mid-session re-latch below so
// that seeded mode (explicit sender_ip) NEVER changes its feedback target.
bool learn_from_source = false;
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

/*
* Compare the ip:port of two socket addresses. Returns true when they are the
* same family and identical address+port. Used in learn-from-source mode to
* detect that the sender's observed source (its public NAT mapping) has moved
* mid-session, so the feedback target can be re-latched to the new address.
*/
static bool same_peer(const struct sockaddr_storage* a, const struct sockaddr* b) {
	if (a->ss_family != b->sa_family)
		return false;
	if (b->sa_family == AF_INET6) {
		const struct sockaddr_in6* aa = (const struct sockaddr_in6*)a;
		const struct sockaddr_in6* bb = (const struct sockaddr_in6*)b;
		return aa->sin6_port == bb->sin6_port &&
			memcmp(&aa->sin6_addr, &bb->sin6_addr, sizeof(aa->sin6_addr)) == 0;
	}
	const struct sockaddr_in* aa = (const struct sockaddr_in*)a;
	const struct sockaddr_in* bb = (const struct sockaddr_in*)b;
	return aa->sin_port == bb->sin_port &&
		aa->sin_addr.s_addr == bb->sin_addr.s_addr;
}

void* rtcpPeriodicThread(void* arg) {
	unsigned char buf[BUFSIZE];
	int rtcpSize;
	uint32_t rtcpFbInterval_ntp = screamRx->getRtcpFbInterval();
	for (;;) {
		// Snapshot the feedback target under lock_scream. In learn-from-source
		// mode the main thread may re-latch outgoing_rtcp_addr(6) mid-session
		// when the sender's NAT mapping moves; taking a local copy under the
		// lock keeps every sendto() below race-free while leaving the network
		// I/O itself outside the critical section. Seeded mode never re-latches,
		// so the snapshot simply mirrors the address set at startup.
		struct sockaddr_in fb_addr;
		struct sockaddr_in6 fb_addr6;
		if (peer_known) {
			pthread_mutex_lock(&lock_scream);
			fb_addr = outgoing_rtcp_addr;
			fb_addr6 = outgoing_rtcp_addr6;
			pthread_mutex_unlock(&lock_scream);
		}
		// Only send if we know the peer address (either seeded or learned from first RTP)
		if (peer_known && getTimeInNtp() - lastPunchNatT_ntp > 32768) { // 500ms in Q16
			/*
			* Send a small packet just to punch open a hole in the NAT,
			*  just one single byte will do.
			* This makes in possible to receive packets on the same port
			*/
			if (ipv6) {
				int ret = sendto(fd_incoming_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr*)&fb_addr6, sizeof(fb_addr6));
			}
			else {
				int ret = sendto(fd_incoming_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr*)&fb_addr, sizeof(fb_addr));
			}
			lastPunchNatT_ntp = getTimeInNtp();
			cerr << "." << endl;
		}
		uint32_t time_ntp = getTimeInNtp();
		// Evaluate the feedback decision and build the feedback packet while
		// holding lock_scream: getRtcpFbInterval/isFeedback/checkIfFlushAck/
		// getLastFeedbackT and createStandardizedFeedback all dereference
		// screamRx, which the main thread may delete and recreate in its
		// idle-reset path. The original code read screamRx in the condition
		// before taking the lock, racing that reset. Network I/O stays outside
		// the critical section.
		bool isFeedback = false;
		if (peer_known) {
			pthread_mutex_lock(&lock_scream);
			rtcpFbInterval_ntp = screamRx->getRtcpFbInterval();
			if (screamRx->isFeedback(time_ntp) &&
				(screamRx->checkIfFlushAck() ||
					(time_ntp - screamRx->getLastFeedbackT() > rtcpFbInterval_ntp))) {
				isFeedback = screamRx->createStandardizedFeedback(getTimeInNtp(), true, buf, rtcpSize);
			}
			pthread_mutex_unlock(&lock_scream);
		}
		if (isFeedback) {
			if (ipv6) {
				sendto(fd_incoming_rtp, buf, rtcpSize, 0, (struct sockaddr*)&fb_addr6, sizeof(fb_addr6));
			}
			else {
				sendto(fd_incoming_rtp, buf, rtcpSize, 0, (struct sockaddr*)&fb_addr, sizeof(fb_addr));
			}
			lastPunchNatT_ntp = getTimeInNtp();
		}
		usleep(500);
	}
}

int main(int argc, char* argv[])
{
	unsigned char buf[BUFSIZE];
	unsigned char buf_rtcp[BUFSIZE];
	if (argc <= 1) {
		cerr << "SCReAM BW test tool, receiver. Ericsson AB. Version 2026-06-30 " << endl;
		cerr << "Usage :" << endl << " > scream_bw_test_rx <options> [sender_ip] sender_port" << endl;
		cerr << "     -ipv6               IPv6" << endl;
		cerr << "     -ackdiff            set the max distance in received RTPs to send an ACK " << endl;
		cerr << "     -nreported          set the number of reported RTP packets per ACK " << endl;
		cerr << "     -if name            bind to specific interface" << endl;
		cerr << endl;
		cerr << "  If sender_ip is omitted, the receiver will learn the sender address" << endl;
		cerr << "  from the first incoming RTP packet (reply-to-source mode)." << endl;
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

	// Two modes: seeded (sender_ip + port) or learn-from-source (just port)
	if (argc > (ix + 1)) {
		// Seeded mode: sender_ip and port provided
		SENDER_IP = argv[ix];
		INCOMING_RTP_PORT = atoi(argv[ix + 1]);
		peer_known = true; // Peer address is known upfront
	}
	else if (argc == (ix + 1)) {
		// Learn-from-source mode: only port provided
		INCOMING_RTP_PORT = atoi(argv[ix]);
		peer_known = false; // Will learn peer from first RTP packet
		learn_from_source = true; // Allow mid-session re-latch when the NAT mapping moves
		cerr << "Running in learn-from-source mode (sender_ip will be learned from first RTP)" << endl;
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

		// Only seed outgoing address if peer is known upfront (seeded mode)
		if (peer_known) {
			outgoing_rtcp_addr6.sin6_family = AF_INET6;
			inet_pton(AF_INET6, SENDER_IP, &outgoing_rtcp_addr6.sin6_addr);
			outgoing_rtcp_addr6.sin6_port = htons(INCOMING_RTP_PORT);
		}


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

		// Only seed outgoing address if peer is known upfront (seeded mode)
		if (peer_known) {
			outgoing_rtcp_addr.sin_family = AF_INET;
			inet_aton(SENDER_IP, (in_addr*)&outgoing_rtcp_addr.sin_addr.s_addr);
			outgoing_rtcp_addr.sin_port = htons(INCOMING_RTP_PORT);
		}


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
		if (bind(fd_incoming_rtp, (struct sockaddr*)&incoming_rtp_addr6, sizeof(incoming_rtp_addr6)) < 0) {
			perror("bind incoming_rtp_addr failed");
			return 0;
		}
		else {
			cerr << "Listen on port " << INCOMING_RTP_PORT << " to receive RTP from sender " << endl;
		}
	}
	else {
		if (bind(fd_incoming_rtp, (struct sockaddr*)&incoming_rtp_addr, sizeof(incoming_rtp_addr)) < 0) {
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
	* Send a small packet just to punch open a hole in the NAT (only in seeded mode).
	* In learn-from-source mode, the sender punches the hole outbound, so skip this.
	*/
	if (peer_known) {
		if (ipv6) {
			sendto(fd_incoming_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr*)&outgoing_rtcp_addr6, sizeof(outgoing_rtcp_addr6));
		}
		else {
			sendto(fd_incoming_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr*)&outgoing_rtcp_addr, sizeof(outgoing_rtcp_addr));
		}
		lastPunchNatT_ntp = getTimeInNtp();
	}

	uint16_t lastSn = 0;
	// Initialize the mutex before starting the periodic thread: that thread
	// now locks lock_scream on every iteration (see rtcpPeriodicThread), so
	// it must be valid before the thread can run.
	pthread_mutex_init(&lock_scream, NULL);

	pthread_t rtcp_thread;
	pthread_create(&rtcp_thread, NULL, rtcpPeriodicThread, (void*)"Periodic RTCP thread...");


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

	// Prepare to capture source address (needed for learn-from-source mode)
	struct sockaddr_storage peer_addr;
	rcv_msg.msg_name = &peer_addr;
	rcv_msg.msg_namelen = sizeof(peer_addr);
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
		// Reset msg_namelen before each recvmsg (kernel updates it)
		rcv_msg.msg_namelen = sizeof(peer_addr);
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
				// Determine the source address of this RTP packet. On the ECN
				// path recvmsg fills rcv_msg.msg_name (peer_addr); otherwise
				// recvfrom fills sender_rtp_addr(6). Normalise both into a
				// sockaddr_storage so learn and re-latch share one code path.
				struct sockaddr_storage src_addr;
#ifdef ECN_CAPABLE
				memcpy(&src_addr, &peer_addr, sizeof(src_addr));
#else
				if (ipv6) {
					memcpy(&src_addr, &sender_rtp_addr6, sizeof(sender_rtp_addr6));
				}
				else {
					memcpy(&src_addr, &sender_rtp_addr, sizeof(sender_rtp_addr));
				}
#endif
				// Learn peer address from first RTP packet (if not already
				// known), and in learn-from-source mode also re-latch it when
				// the sender's observed source moves mid-session (a CGNAT/NAT
				// rebind, common on cellular links after tens of minutes). A
				// stale target would otherwise strand SCReAM feedback at an
				// address that no longer routes to the sender, blinding its
				// rate controller. Seeded mode (learn_from_source == false)
				// keeps its startup target unconditionally.
				// The current feedback target is only read here (this recv
				// thread is its sole writer), so comparing it lock-free is safe.
				struct sockaddr* cur_target = ipv6
					? (struct sockaddr*)&outgoing_rtcp_addr6
					: (struct sockaddr*)&outgoing_rtcp_addr;
				bool relatch = peer_known && learn_from_source &&
					!same_peer(&src_addr, cur_target);
				if (!peer_known || relatch) {
					pthread_mutex_lock(&lock_scream);
					if (ipv6) {
						memcpy(&outgoing_rtcp_addr6, &src_addr, sizeof(outgoing_rtcp_addr6));
					}
					else {
						memcpy(&outgoing_rtcp_addr, &src_addr, sizeof(outgoing_rtcp_addr));
					}
					pthread_mutex_unlock(&lock_scream);
					if (relatch) {
						cerr << "Sender source address changed; re-latched feedback target" << endl;
					}
					else {
						peer_known = true; // Mark peer as known (enables periodic thread + feedback)
						cerr << "Learned sender address from first RTP packet" << endl;
					}
				}

				if (time_ntp - last_received_time_ntp > 2 * 65536) { // 2 sec in Q16
					/*
					* It's been more than 2 seconds since we last received an RTP packet
					*  let's reset everything to be on the safe side.
					* Hold lock_scream across the delete/new: rtcpPeriodicThread
					* dereferences screamRx concurrently, so swapping the pointer
					* unlocked is a use-after-free. This fires when the RTP source
					* stalls then resumes (e.g. a flapping cellular/NAT link).
					*/
					receivedRtp = 0;
					pthread_mutex_lock(&lock_scream);
					delete screamRx;
					screamRx = new ScreamRx(10, ackDiff, nReportedRtpPackets);
					pthread_mutex_unlock(&lock_scream);
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
