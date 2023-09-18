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
#include <getopt.h>
#include <errno.h>
using namespace std;

/*
* Scream receiver side wrapper
* Receives SCReAM congestion controlled RTP media and
*  generates RTCP feedback to the sender, over the same RTP port
* Media sources (max 6) are demultiplexed and forwarded to local RTP ports
*  given by local_port list
*/
#define BUFSIZE 2048

#define MAX_SOURCES 6
uint32_t SSRC_RTCP=10;


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



// Input UDP socket, RTP packets come here and we send RTCP packets in the
// reverse direction through this socket
int fd_in_rtp;

ScreamRx *screamRx = 0;


int fd_local_rtp[MAX_SOURCES];
uint32_t ssrcMap[MAX_SOURCES];

char* in_ip = "10.10.10.2";
char* out_ip = NULL;
int in_port = 30110;
int rtcp_port = 30110;
struct sockaddr_in in_bind_rtp_addr, in_rtp_addr, out_rtcp_addr, sender_rtcp_addr,in_l4s_addr;
struct sockaddr_in local_rtp_addr[MAX_SOURCES];
//test code for python
char* python_IP = "127.0.0.1";
int python_port = 30200;
int l4s_ctrl_port = 30300;
int fd_python;
int fd_l4s_ctrl;
bool isL4s = true;
struct sockaddr_in python_addr;

char* local_ip[MAX_SOURCES] =
   {"127.0.0.1",
	"127.0.0.1",
	"127.0.0.1",
	"127.0.0.1",
	"127.0.0.1",
	"127.0.0.1"};

int local_port[MAX_SOURCES] = {30112,30114,30116,30118,30120,30122};
int nSources = 0;

pthread_mutex_t lock_scream;

double t0 = 0;

/*
* Time in 32 bit NTP format
* 16 most  significant bits are seconds
* 16 least significant bits is fraction
*/
uint32_t getTimeInNtp(){
  struct timeval tp;
  gettimeofday(&tp, NULL);
  double time = (tp.tv_sec + tp.tv_usec*1e-6)-t0;
  uint64_t ntp64 = uint64_t(time*65536);
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

void parseRtp(unsigned char *buf, uint16_t* seqNr, uint32_t* timeStamp, uint32_t* ssrc) {
  uint16_t tmp_s;
  uint32_t tmp_l;
  memcpy(&tmp_s, buf + 2, 2);
  *seqNr = ntohs(tmp_s);
  memcpy(&tmp_l, buf + 4, 4);
  *timeStamp  = ntohl(tmp_l);
  memcpy(&tmp_l, buf + 8, 4);
  *ssrc  = ntohl(tmp_l);
}

uint32_t lastPunchNatT_ntp = 0;

#define KEEP_ALIVE_PKT_SIZE 1

void *rtcpPeriodicThread(void *arg) {
  unsigned char buf[BUFSIZE];
  int rtcpSize;
  uint32_t rtcpFbInterval_ntp = screamRx->getRtcpFbInterval();
  for (;;) {
    uint32_t time_ntp = getTimeInNtp();

    if (getTimeInNtp() - lastPunchNatT_ntp > 32768) { // 0.5s in !16
      /*
      * Send a small packet just to punch open a hole in the NAT,
      *  just one single byte will do.
      * This makes in possible to receive packets on the same port
      */
      int ret = sendto(fd_in_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr *)&out_rtcp_addr, sizeof(out_rtcp_addr));
      lastPunchNatT_ntp = getTimeInNtp();
      cerr << "." << endl;
      /*
      * Add lack of RTP packets warning here
      */
    }

    if (screamRx->isFeedback(time_ntp) &&
         (screamRx->checkIfFlushAck() ||
         (time_ntp - screamRx->getLastFeedbackT() > rtcpFbInterval_ntp))) {
      rtcpFbInterval_ntp = screamRx->getRtcpFbInterval();

      pthread_mutex_lock(&lock_scream);
      bool isFeedback = screamRx->createStandardizedFeedback(getTimeInNtp(), false, buf, rtcpSize);
      pthread_mutex_unlock(&lock_scream);
      if (isFeedback) {
         sendto(fd_in_rtp, buf, rtcpSize, 0, (struct sockaddr *)&out_rtcp_addr, sizeof(out_rtcp_addr));
         lastPunchNatT_ntp = getTimeInNtp();
      }
    }
    usleep(500);
  }
}

#define MAX_CTRL_SIZE 8192
#define MAX_BUF_SIZE 65536
#define ALL_CODE

/*
 * Turn off L4S : echo "0" > /dev/udp/127.0.0.1/30300
 * Turn on  L4S : echo "1" > /dev/udp/127.0.0.1/30300
 * This is a cheat for demo purposes only, when set to 0
 *  the ECN bits simply are viped. It is of course against
 *  all things good we all believe in, so now you know..
 */
void *rxL4sCtrlThread(void *arg) {
    unsigned char buf[100];
    for (;;) {
      int recvlen = recvfrom(fd_l4s_ctrl,
          buf,
          100,
          0,
          (struct sockaddr *)&in_l4s_addr, sizeof(in_l4s_addr));

      if (strstr((char*) buf,"1") != 0)
        isL4s = true;
      else
        isL4s = false;
      cerr << "  L4S CTRL rcvd " << buf << " " << isL4s << endl;
    }
    return NULL;
}

void usage (void)
{
    cerr << "Usage :" << endl << " >scream_receiver incoming_ip rtp_port rtcp_port <forward_ip forward_port ...>" << endl;
    cerr << "   --out_ip addr" << endl;
    cerr << "   forward ip and port pairs are specified for the case that RTP packets should " << endl;
    cerr << "   be forwarded for rendering in another machine, for example: " << endl;
    cerr << "  > scream_receiver 10.10.10.2 30110 10.10.10.12 30112 10.10.10.13 30114 " << endl;
    cerr << "   dictates that the 1st stream is forwarded to 10.10.10.12:30112 and " << endl;
    cerr << "   the 2nd stream is forwarded to 10.10.10.13:30114 " << endl;
}
int main(int argc, char* argv[])
{
  struct timeval tp;
  unsigned char bufRtcp[BUFSIZE];
  int       option_index = 0;
  int       c;
  static struct option long_options[] = {
        {"out_ip",     required_argument, 0, 'o'},
        {"history",    0, 0, 'h'},
        {0, 0, 0, 0}
  };
  int offs = 0;

  gettimeofday(&tp, NULL);
  t0 = (tp.tv_sec + tp.tv_usec*1e-6)-1e-3;

  unsigned char bufRtp[BUFSIZE];
  if (argc <= 1) {
      usage();
    exit(-1);
  }

     while (1) {
       c = getopt_long(argc, argv, "ho:", long_options, &option_index);
       if (c == -1) {
           break;
       }

       switch (c) {
         case 'o':
           if (!optarg) {
               fprintf(stderr, "missing ip addr\n");
               exit(11);
           }
           printf("%s\n", optarg);
           out_ip = strdup(optarg);
           offs += 1;
           break;
         case 'h':
           usage();
           break;
         default:
           printf ("?? getopt returned character code 0%o ??\n", c);
           exit(13);
       }
   }


  in_ip = argv[1 + offs];
  in_port = atoi(argv[2 + offs]);
  rtcp_port = atoi(argv[3 + offs]);
  if (argc >= 5 + offs) {
	// RTP packets should be forwarded to ip port pairs
	int N = (argc-4 + offs)/2;
	for (int n=0; n < N; n++) {
		local_ip[n] = argv[n*2+4 + offs];
		local_port[n] = atoi(argv[n*2+5 + offs]);
		printf("port to forward: %d\n", local_port[n]);
    }
  }

  screamRx = new ScreamRx(SSRC_RTCP);

  in_rtp_addr.sin_family = AF_INET;
  //in_rtp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  inet_aton(in_ip,(in_addr*) &in_rtp_addr.sin_addr.s_addr);
  in_rtp_addr.sin_port = htons(in_port);

  in_bind_rtp_addr.sin_family = AF_INET;
  //in_rtp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  inet_aton("0.0.0.0",(in_addr*) &in_bind_rtp_addr.sin_addr.s_addr);
  in_bind_rtp_addr.sin_port = htons(in_port);

  if ((fd_in_rtp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("cannot create socket for incoming RTP packets");
    return 0;
  }

  in_l4s_addr.sin_family = AF_INET;
  in_l4s_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  in_l4s_addr.sin_port = htons(l4s_ctrl_port);
  if ((fd_l4s_ctrl = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("cannot create socket for L4S control");
    return 0;
  }
  if (bind(fd_l4s_ctrl, (struct sockaddr *)&in_l4s_addr, sizeof(in_l4s_addr)) < 0) {
     char s[100];
     sprintf(s, "bind in_l4s_addr %d failed", l4s_ctrl_port);
     perror(s);
     return 0;
  }
  else {
     cerr << "Listen on port " << l4s_ctrl_port << " to receive L4S control " << endl;
  }

  out_rtcp_addr.sin_family = AF_INET;
  inet_aton((out_ip) ? out_ip: in_ip,(in_addr*) &out_rtcp_addr.sin_addr.s_addr);
  out_rtcp_addr.sin_port = htons(rtcp_port);

  for (int n=0; n < MAX_SOURCES; n++) {
    local_rtp_addr[n].sin_family = AF_INET;
    inet_aton(local_ip[n],(in_addr*) &local_rtp_addr[n].sin_addr.s_addr);
    local_rtp_addr[n].sin_port = htons(local_port[n]);
    if ((fd_local_rtp[n] = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
      perror("cannot create socket for outgoing RTP packets to renderer (video decoder)");
      return 0;
    }
  }
//test code for python
  python_addr.sin_family = AF_INET;
  python_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  python_addr.sin_port = htons(python_port);

  if((fd_python = socket(AF_INET, SOCK_DGRAM,0))<0){
	perror("cannot create socket for python package");
	return 0;
  }

  int enable = 1;
  if (setsockopt(fd_in_rtp, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
  }
  unsigned char set = 0x03;
  if (setsockopt(fd_in_rtp, IPPROTO_IP, IP_RECVTOS, &set,sizeof(set)) < 0) {
    cerr << "cannot set recvtos on incoming socket" << endl;
  } else {
    cerr << "socket set to recvtos" << endl;
  }

  uint64_t recv_buf_size = 1024*1024*20; //20MByte
  if (setsockopt(fd_in_rtp, SOL_SOCKET, SO_RCVBUF, &recv_buf_size,sizeof(recv_buf_size)) < 0) {
    cerr << "cannot set SO_RCVBUF on incoming socket" << endl;
  } else {
    cerr << "socket SO_RCVBUF set to " << recv_buf_size << endl;
  }

  if (bind(fd_in_rtp, (struct sockaddr *)&in_bind_rtp_addr, sizeof(in_bind_rtp_addr)) < 0) {
    perror("bind incoming_rtp_addr failed");
    return 0;
  } else{
    cerr << "Listen on port " << in_port <<" to receive RTP from sender " << endl;
  }

  struct sockaddr_in sender_rtp_addr;
  socklen_t addrlen_sender_rtp_addr = sizeof(sender_rtp_addr);

  int recvlen;

  uint32_t last_received_time_ntp = 0;
  uint32_t receivedRtp = 0;

  /*
  * Send a small packet just to punch open a hole in the NAT,
  *  just one single byte will do.
  * This makes in possible to receive packets on the same port
  */
  sendto(fd_in_rtp, bufRtp, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr *)&out_rtcp_addr, sizeof(out_rtcp_addr));
  lastPunchNatT_ntp = getTimeInNtp();

  pthread_t rtcp_thread;
  pthread_mutex_init(&lock_scream, NULL);
  pthread_create(&rtcp_thread, NULL, rtcpPeriodicThread, "Periodic RTCP thread...");


  pthread_t l4s_ctrl_thread;
  pthread_create(&l4s_ctrl_thread, NULL, rxL4sCtrlThread, "L4S ctrl thread");

  int *ecnptr;
  unsigned char received_ecn;

  struct msghdr rcv_msg;
  struct iovec rcv_iov[1];
  char rcv_ctrl_data[MAX_CTRL_SIZE];
  char rcv_buf[MAX_BUF_SIZE];

   /* Prepare message for receiving */
  rcv_iov[0].iov_base = rcv_buf;
  rcv_iov[0].iov_len = MAX_BUF_SIZE;

  struct sockaddr_in src;

  rcv_msg.msg_name = &src;	// Socket is connected
  rcv_msg.msg_namelen = sizeof(sockaddr_in);
  rcv_msg.msg_iov = rcv_iov;
  rcv_msg.msg_iovlen = 1;
  rcv_msg.msg_control = rcv_ctrl_data;
  rcv_msg.msg_controllen = MAX_CTRL_SIZE;

  for (;;) {
    /*
    * Wait for incoing RTP packet, this call can be blocking
    */

    /*
    * Extract ECN bits
    */
    int recvlen = recvmsg(fd_in_rtp, &rcv_msg, 0);
    unsigned char received_ecn;
    if (recvlen == -1) {
	    perror("recvmsg()");
	    close(fd_in_rtp);
	    return EXIT_FAILURE;
    } else {
	    struct cmsghdr *cmptr;
	    int *ecnptr;

      struct sockaddr_in *src = rcv_msg.msg_name;
      uint16_t src_port = ntohs(src->sin_port);
      //cerr << " Port " << src_port << endl;
      out_rtcp_addr.sin_port = htons(src_port);

	    for (cmptr = CMSG_FIRSTHDR(&rcv_msg);
			  cmptr != NULL;
			  cmptr = CMSG_NXTHDR(&rcv_msg, cmptr)) {
		    if (cmptr->cmsg_level == IPPROTO_IP && cmptr->cmsg_type == IP_TOS) {
			    ecnptr = (int*)CMSG_DATA(cmptr);
			    received_ecn = *ecnptr;
		    }
	    }
      memcpy(bufRtp,rcv_msg.msg_iov[0].iov_base,recvlen);
    }

    if (!isL4s) {
      received_ecn = 0;
    }

#ifdef TEST_L4S
    if (received_ecn == 0x1) {
      pMarkCarry += PMARK;
      if (pMarkCarry >= 1.0) {
    	  pMarkCarry -= 1.0;
    		received_ecn = 0x3;
    	}
    }
#endif

    uint32_t time_ntp = getTimeInNtp();
    if (recvlen > 1) {
      if (bufRtp[1] == 0x7F) {
        // Packet contains statistics
        recvlen -= 2; // 2 bytes
        char s[1000];
        memcpy(s,&bufRtp[2],recvlen);
        s[recvlen] = 0x00;
        cout << s << endl;
		sendto(fd_python,(const char*)s,strlen(s),MSG_CONFIRM,(const struct sockaddr*)&python_addr, sizeof(python_addr));
      } else if (bufRtp[1] == 0x7E) {
        // Packet contains an SSRC map
	      nSources = (recvlen-2)/4;
        for (int n=0; n < nSources; n++) {
          uint32_t tmp_l;
          memcpy(&tmp_l, bufRtp+2+n*4, 4);
          ssrcMap[n] = ntohl(tmp_l);
	}
      } else {
        if (time_ntp - last_received_time_ntp > 131072) { // 2s in Q16
          /*
          * It's been more than 5 seconds since we last received an RTP packet
          *  let's reset everything to be on the safe side
          */
          receivedRtp = 0;
          pthread_mutex_lock(&lock_scream);
          delete screamRx;
          screamRx = new ScreamRx(SSRC_RTCP);
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
        uint32_t ssrc;
        parseRtp(bufRtp,&seqNr, &ts, &ssrc);
        bool isMark = (bufRtp[1] & 0x80) != 0;

        /*
        * Map the RTP packets to correct display by means of the ssrcMap
        */
        int ix = -1;
        for (int n=0; n < nSources; n++) {
           if (ssrc == ssrcMap[n]) {
             ix = n;
             break;
           }
        }
        if (ix != -1) {
          /*
          * Forward RTP packet to the correct internal port, for e.g GStreamer playout
          */

          sendto(fd_local_rtp[ix], bufRtp, recvlen, 0, (struct sockaddr *)&local_rtp_addr[ix], sizeof(local_rtp_addr[ix]));
       	}

        /*
        * Register received RTP packet with ScreamRx
        */
        pthread_mutex_lock(&lock_scream);
        screamRx->receive(getTimeInNtp(), 0, ssrc, recvlen, seqNr, received_ecn, isMark);
        pthread_mutex_unlock(&lock_scream);

        if (screamRx->checkIfFlushAck()) {
          pthread_mutex_lock(&lock_scream);
          int rtcpSize;
          bool isFeedback = screamRx->createStandardizedFeedback(getTimeInNtp(), false, bufRtcp, rtcpSize);
          pthread_mutex_unlock(&lock_scream);
          if (isFeedback) {
             sendto(fd_in_rtp, bufRtcp, rtcpSize, 0, (struct sockaddr *)&out_rtcp_addr, sizeof(out_rtcp_addr));
             lastPunchNatT_ntp = getTimeInNtp();
          }
        }
      }
    }
  }
}
