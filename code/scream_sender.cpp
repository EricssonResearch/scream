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
#define MIN_PACE_INTERVAL_S 0.002
#define MIN_PACE_INTERVAL_US 1900

#define ECN_CAPABLE
/*
* ECN capable
* -1 = Not-ECT
* 0 = ECT(0)
* 1 = ECT(1)
* 3 = CE
*/
int ect = -1;

char sierraLogString[BUFSIZE] = " 0, 0, 0, 0";
float FPS = 50.0f; // Frames per second
uint32_t SSRC=100;
int fixedRate = 0;
bool isKeyFrame = false;
bool disablePacing = false;
float keyFrameInterval = 0.0;
float keyFrameSize = 1.0;
int initRate = 1000;
int minRate = 1000;
int maxRate = 200000;
int rateIncrease = 10000;
float rateScale = 0.5f;
float dscale = 10.0f;
bool enableClockDriftCompensation = false;
float burstTime = -1.0;
float burstSleep = -1.0;
bool isBurst = false;
float burstStartTime = -1.0;
float burstSleepTime = -1.0;
bool pushTraffic = false;
float packetPacingHeadroom=1.25f;


uint16_t seqNr = 0;
uint32_t lastKeyFrameT_ntp = 0;
int mtu = 1200;
float runTime = -1.0;
bool stopThread = false;
pthread_t create_rtp_thread = 0;
pthread_t transmit_rtp_thread = 0;
pthread_t rtcp_thread = 0;
pthread_t sierra_python_thread = 0;
bool sierraLog;

float scaleFactor = 0.9f;
float delayTarget = 0.06f;
bool printSummary = true;

int fd_outgoing_rtp;
int fd_sierra_python;
ScreamTx *screamTx = 0;
RtpQueue *rtpQueue = 0;

// We don't bother about SSRC in this implementation, it is only one stream

char *DECODER_IP = "192.168.0.21";
int DECODER_PORT = 30110;
char *DUMMY_IP = "217.10.68.152"; // Dest address just to punch hole in NAT

int SIERRA_PYTHON_PORT = 35000;

struct sockaddr_in outgoing_rtp_addr;
struct sockaddr_in incoming_rtcp_addr;
struct sockaddr_in dummy_rtcp_addr;
struct sockaddr_in sierra_python_addr;

unsigned char buf_rtp[BUFSIZE];     /* receive buffer RTP packets */

unsigned char buf_rtcp[BUFSIZE];     /* receive buffer RTCP packets*/

unsigned char buf_sierra[BUFSIZE];     /* receive buffer RTCP packets*/


socklen_t addrlen_outgoing_rtp;
socklen_t addrlen_dummy_rtcp;
uint32_t lastLogT_ntp = 0;
uint32_t lastLogTv_ntp = 0;
uint32_t tD_ntp = 0;//(INT64_C(1) << 32)*1000 - 5000000;
socklen_t addrlen_incoming_rtcp = sizeof(incoming_rtcp_addr);
socklen_t addrlen_sierra_python_addr = sizeof(sierra_python_addr);
pthread_mutex_t lock_scream;
pthread_mutex_t lock_rtp_queue;
pthread_mutex_t lock_pace;

FILE *fp_log = 0;

char *ifname = 0;


bool ntp = false;
bool append = false;
bool itemlist = false;
bool detailed = false;
float randRate = 0.0f;

double t0=0;
/*
long getTimeInUs(){
  struct timeval tp;
  gettimeofday(&tp, NULL);
  long us = tp.tv_sec * 1000000 + tp.tv_usec;
  return us;
}
*/
uint32_t getTimeInNtp(){
  struct timeval tp;
  gettimeofday(&tp, NULL);
  double time = tp.tv_sec + tp.tv_usec*1e-6-t0;
  uint64_t ntp64 = uint64_t(time*65536.0);
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
void parseRtp(unsigned char *buf, uint16_t* seqNr, uint32_t* timeStamp, unsigned char *pt) {
  uint16_t rawSeq;
  uint32_t rawTs;
  memcpy(&rawSeq, buf + 2, 2);
  memcpy(&rawTs, buf + 4, 4);
  memcpy(pt,buf+1,1);
  *seqNr = ntohs(rawSeq);
  *timeStamp  = ntohl(rawTs);
}

void writeRtp(unsigned char *buf, uint16_t seqNr, uint32_t timeStamp, unsigned char pt) {
  seqNr = htons(seqNr);
  timeStamp  = htonl(timeStamp);
  uint32_t tmp = htonl(SSRC);
  memcpy(buf + 2, &seqNr, 2);
  memcpy(buf + 4, &timeStamp, 4);
  memcpy(buf + 8, &tmp, 4);
  memcpy(buf+1, &pt, 1);
  buf[0] = 0x80;
}


void sendPacket(void* buf, int size) {
  sendto(fd_outgoing_rtp, buf, size, 0, (struct sockaddr *)&outgoing_rtp_addr, sizeof(outgoing_rtp_addr));
}

/*
 * Transmit a packet if possible.
 * If not allowed due to packet pacing restrictions,
 * then start a timer.
 */
void *transmitRtpThread(void *arg) {
  int size;
  uint16_t seqNr;
  char buf[2000];
  uint32_t time_ntp = getTimeInNtp();
  int sleepTime_us = 10;
  float retVal = 0.0f;
  int sizeOfQueue;
  struct timeval start, end;
  useconds_t diff = 0;
  float paceIntervalFixedRate = 0.0f;
  if (fixedRate > 0 && !disablePacing) {
     paceIntervalFixedRate = (mtu+40)*8.0f/(fixedRate*1000)*0.9;
  }
  for (;;) {
    if (stopThread) {
      return NULL;
    }

    sleepTime_us = 1;
    retVal = 0.0f;
    pthread_mutex_lock(&lock_scream);
    time_ntp = getTimeInNtp();
    retVal = screamTx->isOkToTransmit(time_ntp, SSRC);
    pthread_mutex_unlock(&lock_scream);

    if (retVal != -1.0f) {
      pthread_mutex_lock(&lock_rtp_queue);
      sizeOfQueue = rtpQueue->sizeOfQueue();
      pthread_mutex_unlock(&lock_rtp_queue);
      do {
         gettimeofday(&start, 0);
         time_ntp = getTimeInNtp();

         retVal = screamTx->isOkToTransmit(time_ntp, SSRC);
         if (fixedRate > 0 && retVal >= 0.0f && sizeOfQueue > 0)
            retVal = paceIntervalFixedRate;
         if (disablePacing && sizeOfQueue > 0 && retVal > 0.0f)
            retVal = 0.0f;
         if (retVal > 0.0f)
            accumulatedPaceTime += retVal;
         if (retVal != -1.0) {
           pthread_mutex_lock(&lock_rtp_queue);
           rtpQueue->pop(buf, size, seqNr);
           sendPacket(buf,size);
           pthread_mutex_unlock(&lock_rtp_queue);
           pthread_mutex_lock(&lock_scream);
           time_ntp = getTimeInNtp();
           retVal = screamTx->addTransmitted(time_ntp, SSRC, size, seqNr, (buf[1] & 0x80) != 0);
           pthread_mutex_unlock(&lock_scream);
         }

         pthread_mutex_lock(&lock_rtp_queue);
         sizeOfQueue = rtpQueue->sizeOfQueue();
         pthread_mutex_unlock(&lock_rtp_queue);
         gettimeofday(&end, 0);
         diff = end.tv_usec-start.tv_usec;
         accumulatedPaceTime = std::max(0.0f, accumulatedPaceTime-diff*1e-6f);
      } while (accumulatedPaceTime <= MIN_PACE_INTERVAL_S &&
           retVal != -1.0f &&
           sizeOfQueue > 0);
      if (accumulatedPaceTime > 0) {
	sleepTime_us = std::min((int)(accumulatedPaceTime*1e6f), MIN_PACE_INTERVAL_US);
	accumulatedPaceTime = 0.0f;
      }
    }
    usleep(sleepTime_us);
    sleepTime_us = 0;
  }
  return NULL;
}

static int makePeriodic (unsigned int period, struct periodicInfo *info)
{
	int ret;
	unsigned int ns;
	unsigned int sec;
	int fd;
	struct itimerspec itval;

	/* Create the timer */
	fd = timerfd_create (CLOCK_MONOTONIC, 0);
	info->wakeupsMissed = 0;
	info->timer_fd = fd;
	if (fd == -1)
		return fd;

	/* Make the timer periodic */
	sec = period/1000000;
	ns = (period - (sec * 1000000)) * 1000;
	itval.it_interval.tv_sec = sec;
	itval.it_interval.tv_nsec = ns;
	itval.it_value.tv_sec = sec;
	itval.it_value.tv_nsec = ns;
	ret = timerfd_settime (fd, 0, &itval, NULL);
	return ret;
}

static void waitPeriod (struct periodicInfo *info)
{
	unsigned long long missed;
	int ret;

	/* Wait for the next timer event. If we have missed any the
	   number is written to "missed" */
	ret = read (info->timer_fd, &missed, sizeof (missed));
	if (ret == -1)
	{
		perror ("read timer");
		return;
	}

	/* "missed" should always be >= 1, but just to be sure, check it is not 0 anyway */
	if (missed > 0)
		info->wakeupsMissed += (missed - 1);
}

void *createRtpThread(void *arg) {
  uint32_t keyFrameInterval_ntp = (uint32_t) (keyFrameInterval * 65536.0f);
  float rateScale = 1.0f;
  if (isKeyFrame) {
    rateScale = 1.0f+0.0*keyFrameSize/(FPS*keyFrameInterval)/2.0;
    rateScale = (1.0f/rateScale);
  }
  uint32_t dT_us= (uint32_t) (1e6/FPS);
  unsigned char PT = 98;
  struct periodicInfo info;

  makePeriodic (dT_us, &info);

  /*
  * Infinite loop that generates RTP packets
  */
  for (;;) {
    if (stopThread) {
      return NULL;
    }
    uint32_t time_ntp = getTimeInNtp();

    uint32_t ts = (uint32_t) (time_ntp/65536.0*90000);
    float rateTx = screamTx->getTargetBitrate(SSRC)*rateScale;
    float randVal = float(rand())/RAND_MAX-0.5;
    int bytes = (int) (rateTx/FPS/8*(1.0+randVal*randRate));

    if (isKeyFrame && time_ntp-lastKeyFrameT_ntp >= keyFrameInterval_ntp) {
      /*
      * Fake a key frame
      */
      bytes = (int) (bytes*keyFrameSize);
      lastKeyFrameT_ntp = time_ntp;
    }

    if (!pushTraffic && burstTime < 0) {
      if ((time_ntp/6554) % 600 > 598) {
        /*
        * Drop bitrate for 100ms every 30s
        *  this ensures that the queue delay is estimated correctly
        * A normal video encoder use does not necessitate this as the
        *  video stream typically drops in bitrate every once in a while
        */
        bytes = 10;
      }
    } else {
      float time_s = time_ntp/65536.0f;
      if (burstStartTime < 0) {
        burstStartTime = time_s;
        isBurst = true;
      }
      if (time_s > burstStartTime+burstTime && isBurst) {
        isBurst = false;
        burstSleepTime = time_s;
      }
      if (time_s > burstSleepTime+burstSleep && !isBurst) {
        isBurst = true;
        burstStartTime = time_s;
      }
      if (!isBurst)
        bytes=0;
    }

    while (bytes > 0) {
      int pl_size = min(bytes,mtu);
      int recvlen = pl_size+12;

      bytes = std::max(0, bytes-pl_size);
      unsigned char pt = PT;
      if (bytes == 0) {
        // Last RTP packet, set marker bit
        pt |= 0x80;
      }
      writeRtp(buf_rtp,seqNr,ts,pt);

      if (pushTraffic) {
         sendPacket(buf_rtp,recvlen);
      } else {
         pthread_mutex_lock(&lock_rtp_queue);
         rtpQueue->push(buf_rtp, recvlen, seqNr, (time_ntp)/65536.0f);
         pthread_mutex_unlock(&lock_rtp_queue);

         pthread_mutex_lock(&lock_scream);
         time_ntp = getTimeInNtp();
         screamTx->newMediaFrame(time_ntp, SSRC, recvlen);
         pthread_mutex_unlock(&lock_scream);
      }
      seqNr++;
    }
    waitPeriod (&info);

  }

  return NULL;
}

uint32_t rtcp_rx_time_ntp = 0;
#define KEEP_ALIVE_PKT_SIZE 1
void *readRtcpThread(void *arg) {
  /*
  * Wait for RTCP packets from receiver
  */
  for (;;) {
    int recvlen = recvfrom(fd_outgoing_rtp, buf_rtcp, BUFSIZE, 0, (struct sockaddr *)&incoming_rtcp_addr, &addrlen_incoming_rtcp);
    if (stopThread)
      return NULL;
    if (recvlen > KEEP_ALIVE_PKT_SIZE) {
      pthread_mutex_lock(&lock_scream);
      uint32_t time_ntp = getTimeInNtp(); // We need time in microseconds, roughly ms granularity is OK
      char s[100];
      if (ntp) {
         struct timeval tp;
         gettimeofday(&tp, NULL);
         double time = tp.tv_sec + tp.tv_usec*1e-6;
         sprintf(s,"%1.6f", time);
      } else {
         sprintf(s,"%1.4f",time_ntp/65536.0f);
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

void *sierraPythonThread(void *arg) {
  const char *TOK = " {}':,";
  /*
  * Wait for RTCP packets from receiver
  */
  for (;;) {
    int recvlen = recvfrom(fd_sierra_python, buf_sierra, BUFSIZE, 0, (struct sockaddr *)&sierra_python_addr, &addrlen_sierra_python_addr);
    for (int n=0; n < strlen((char*)buf_sierra); n++) {
      if (buf_sierra[n] == '"')
         buf_sierra[n] = ' ';
    }
    int RSSI = 0;
    int RSRP = 0;
    int CellId = 0;
    int SINR = 0;
    char *s = strtok((char*)buf_sierra,TOK);
    while (s != 0) {
      if (strstr(s,"RSSI")) {
        s = strtok(NULL,TOK);
        RSSI = atoi(s);
      }
      if (strstr(s,"ID")) {
        s = strtok(NULL,TOK);
        CellId = atoi(s);
      }
      if (strstr(s,"SINR")) {
        s = strtok(NULL,TOK);
        SINR = atoi(s);
      }
      if (strstr(s,"RSRP")) {
        s = strtok(NULL,TOK);
        RSRP = atoi(s);
      }
      s = strtok(NULL,TOK);
    }

    sprintf(sierraLogString," %9d, %4d, %4d, %4d", CellId, RSRP, RSSI, SINR);
    screamTx->setDetailedLogExtraData(sierraLogString);
    if (stopThread)
      return NULL;
    usleep(500000);
  }
  return NULL;
}

int setup() {
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

  if (bind(fd_outgoing_rtp, (struct sockaddr *)&incoming_rtcp_addr, sizeof(incoming_rtcp_addr)) < 0) {
      perror("bind outgoing_rtp_addr failed");
      return 0;
  } else{
    if (!pushTraffic)
       cerr<< "Listen on port "<< DECODER_PORT<<" to receive RTCP from decoder "<<endl;
  }

  if (sierraLog) {
    sierra_python_addr.sin_family = AF_INET;
    sierra_python_addr.sin_port = htons(SIERRA_PYTHON_PORT);
    sierra_python_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if ((fd_sierra_python = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
       perror("cannot create socket");
       return 0;
    }

    if (bind(fd_sierra_python, (struct sockaddr *)&sierra_python_addr, sizeof(sierra_python_addr)) < 0) {
        perror("bind incoming_sierra_addr failed");
        return 0;
    } else{
      cerr<< "Listen on port "<< SIERRA_PYTHON_PORT<<" to receive data from sierra python log script  "<<endl;
    }
  }
  /*
  * Set ECN capability for outgoing socket using IP_TOS
  */
#ifdef ECN_CAPABLE
  int iptos = 0;
  if (ect == 0 || ect == 1)
    iptos = 2-ect;
  if (ect == 3)
    iptos = 3;
  int res = setsockopt(fd_outgoing_rtp, IPPROTO_IP, IP_TOS,  &iptos, sizeof(iptos));
  if (res < 0) {
      cerr << "Not possible to set ECN bits" << endl;
  }
  int tmp = 0;
#endif
  char buf[10];
  if (fixedRate > 0)
    screamTx = new ScreamTx(1.0f,1.0f,
                            delayTarget,
                            false,
                            1.0f,2.0f,
                            (fixedRate*100)/8,
                            1.5f,
                            20,
                            ect==1,
                            true,
                            enableClockDriftCompensation);
  else
    screamTx = new ScreamTx(scaleFactor, scaleFactor,
                            delayTarget,
                            false,
                            1.0f,dscale,
                            (initRate*100)/8,
                            packetPacingHeadroom,
                            20,
                            ect==1,
                            false,
                            enableClockDriftCompensation);
  rtpQueue = new RtpQueue();
  screamTx->setCwndMinLow(5000);

  if (fixedRate > 0) {
    screamTx->registerNewStream(rtpQueue,
                                SSRC,
                                1.0f,
                                fixedRate*1000.0f,
                                fixedRate*1000.0f,
                                fixedRate*1000.0f,
                                1.0e6f, 0.5f,
                                10.0f, 0.0f, 0.0f, scaleFactor, scaleFactor);
  } else  {
    screamTx->registerNewStream(rtpQueue,
                                SSRC,
                                1.0f,
                                minRate*1000,
                                initRate*1000,
                                maxRate*1000,
                                rateIncrease*1000, rateScale,
                                0.2f, 0.1f, 0.05f, scaleFactor, scaleFactor);
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

  struct timeval tp;
  gettimeofday(&tp, NULL);
  t0 = tp.tv_sec + tp.tv_usec*1e-6 - 1e-3;
  lastT_ntp = getTimeInNtp();

  /*
  * Parse command line
  */
  if (argc <= 1) {
    cerr << "SCReAM BW test tool, sender. Ericsson AB. Version 2020-07-03" << endl;
    cerr << "Usage : " << endl << " > scream_bw_test_tx <options> decoder_ip decoder_port " << endl;
    cerr << "     -if name            bind to specific interface" << endl;
    cerr << "     -time value         run for time seconds (default infinite)" << endl;
    cerr << "     -burst val1 val2    burst media for a given time and then sleeps a given time" << endl;
    cerr << "         example -burst 1.0 0.2 burst media for 1s then sleeps for 0.2s " << endl;
    cerr << "     -nopace             disable packet pacing" << endl;
    cerr << "     -fixedrate value    set a fixed 'coder' bitrate " << endl;
    cerr << "     -pushtraffic        just pushtraffic at a fixed bitrate, no feedback needed" << endl;
    cerr << "                           must be used with -fixedrate option" << endl;
    cerr << "     -key val1 val2      set a given key frame interval [s] and size multiplier " << endl;
    cerr << "                          example -key 2.0 5.0 " << endl;
    cerr << "     -rand value         framesizes vary randomly around the nominal " << endl;
    cerr << "                          example -rand 10 framesize vary +/- 10% " << endl;
    cerr << "     -initrate value     set a start bitrate [kbps]" << endl;
    cerr << "                          example -initrate 2000 " << endl;
    cerr << "     -minrate  value     set a min bitrate [kbps], default 1000kbps" << endl;
    cerr << "                          example -minrate 1000 " << endl;
    cerr << "     -maxrate value      set a max bitrate [kbps], default 200000kbps" << endl;
    cerr << "                          example -maxrate 10000 " << endl;
    cerr << "     -rateincrease value set a max allowed rate increase speed [kbps/s]," << endl;
    cerr << "                          default 10000kbps/s" << endl;
    cerr << "                          example -rateincrease 1000 " << endl;
    cerr << "     -ratescale value    set a max allowed rate increase speed as a fraction of the " << endl;
    cerr << "                          current rate, default 0.5" << endl;
    cerr << "                          example -ratescale 1.0 " << endl;
    cerr << "     -ect n              ECN capable transport, n = 0 or 1 for ECT(0) or ECT(1)," << endl;
    cerr << "                          -1 for not-ECT (default)" << endl;
    cerr << "     -scale value        scale factor in case of loss or ECN event (default 0.9) " << endl;
    cerr << "     -dscale value       scale factor in case of increased delay (default 10.0) " << endl;
    cerr << "     -delaytarget value  set a queue delay target (default = 0.06s) " << endl;
    cerr << "     -paceheadroom value set a packet pacing headroom (default = 1.25s) " << endl;
    cerr << "     -mtu value          set the max RTP payload size (default 1200 byte)" << endl;
    cerr << "     -fps value          set the frame rate (default 50)"  << endl;
    cerr << "     -clockdrift         enable clock drift compensation for the case that the"  << endl;
    cerr << "                          receiver end clock is faster" << endl;
    cerr << "     -verbose            print a more extensive log" << endl;
    cerr << "     -nosummary          don't print summary" << endl;
    cerr << "     -log logfile        save detailed per-ACK log to file" << endl;
    cerr << "     -ntp                use NTP timestamp in logfile" << endl;
    cerr << "     -append             append logfile" << endl;
    cerr << "     -itemlist           add item list in beginning of log file" << endl;
    cerr << "     -detailed           detailed log, per ACKed RTP" << endl;
    cerr << "     -sierralog          get logs from python script that logs a sierra modem" << endl;
    exit(-1);
  }
  int ix = 1;
  bool verbose = false;
  char *logFile = 0;
  /* First find options */
  while (strstr(argv[ix],"-")) {
    if (strstr(argv[ix],"-ect")) {
      ect = atoi(argv[ix+1]);
      ix+=2;
      if (!(ect == 1 || ect == 0 || ect == 1 || ect == 3)) {
        cerr << "ect must be -1, 0, 1 or 3 " << endl;
        exit(0);

      }
			continue;
    }
    if (strstr(argv[ix],"-time")) {
      runTime = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-scale")) {
      scaleFactor = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-dscale")) {
      dscale = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-delaytarget")) {
      delayTarget = atof(argv[ix+1]);
      ix+=2;
			continue;
    }

    if (strstr(argv[ix],"-paceheadroom")) {
      packetPacingHeadroom = atof(argv[ix+1]);
      ix+=2;
			continue;
    }

    if (strstr(argv[ix],"-mtu")) {
      mtu = atoi(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-fixedrate")) {
      fixedRate = atoi(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-burst")) {
      burstTime = atof(argv[ix+1]);
      burstSleep = atof(argv[ix+2]);
      ix+=3;
			continue;
    }
    if (strstr(argv[ix],"-key")) {
      isKeyFrame = true;
      keyFrameInterval = atof(argv[ix+1]);
      keyFrameSize = atof(argv[ix+2]);
      ix+=3;
			continue;
    }
    if (strstr(argv[ix],"-nopace")) {
      disablePacing = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-fps")) {
      FPS = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-rand")) {
      randRate = atof(argv[ix+1])/100.0;
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-initrate")) {
      initRate = atoi(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-minrate")) {
      minRate = atoi(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-maxrate")) {
      maxRate = atoi(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-rateincrease")) {
      rateIncrease = atoi(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-ratescale")) {
      rateScale = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-verbose")) {
      verbose = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-nosummary")) {
      printSummary = false;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-log")) {
      logFile = argv[ix+1];
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-sierralog")) {
      sierraLog = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-ntp")) {
      ntp = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-append")) {
      append = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-itemlist")) {
      itemlist = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-detailed")) {
      detailed = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-pushtraffic")) {
      pushTraffic = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-clockdrift")) {
      enableClockDriftCompensation = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-if")) {
      ifname = argv[ix+1];
      ix+=2;
			continue;
    }
		cerr << "unexpected arg " << argv[ix] << endl;
		exit(0);
  }


  if (pushTraffic && fixedRate==0) {
     cerr << "Error : pushtraffic can only be used with fixedrate" << endl;
     exit(-1);
  }
  if (logFile) {
      if (append)
         fp_log = fopen(logFile,"a");
      else
         fp_log = fopen(logFile,"w");
  }
  if (minRate > initRate)
    initRate = minRate;
  DECODER_IP = argv[ix];ix++;
  DECODER_PORT = atoi(argv[ix]);ix++;

  if (setup() == 0)
    return 0;

  if (logFile && !append && itemlist) {
    fprintf(fp_log,"%s\n", screamTx->getDetailedLogItemList());
  }

  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));
  action.sa_handler = stopAll;
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGINT, &action, NULL);

  screamTx->setDetailedLogFp(fp_log);
  screamTx->useExtraDetailedLog(detailed);

  pthread_mutex_init(&lock_scream, NULL);
  pthread_mutex_init(&lock_rtp_queue, NULL);
  pthread_mutex_init(&lock_pace, NULL);

  /* Create RTP thread */
  pthread_create(&create_rtp_thread,NULL,createRtpThread,(void*)"Create RTP thread...");
  if (pushTraffic) {
    cerr << "Scream sender started in push traffic mode " << fixedRate << "kbps" <<endl;

    while (!stopThread && (runTime < 0 || getTimeInNtp() < runTime*65536.0f)) {
       usleep(50000);
    }
    stopThread = true;

  } else {
     cerr << "Scream sender started! "<<endl;

     /* Create RTCP thread */
     pthread_create(&rtcp_thread,NULL,readRtcpThread,(void*)"RTCP thread...");
     /* Transmit RTP thread */
     pthread_create(&transmit_rtp_thread,NULL,transmitRtpThread,(void*)"Transmit RTP thread...");
     if (sierraLog) {
       /* Sierra python log thread */
       pthread_create(&sierra_python_thread,NULL,sierraPythonThread,(void*)"Sierra Python log thread...");
     }

     while(!stopThread && (runTime < 0 || getTimeInNtp() < runTime*65536.0f)) {
       uint32_t time_ntp = getTimeInNtp();
       bool isFeedback = time_ntp-rtcp_rx_time_ntp < 65536; // 1s in Q16
       if ((printSummary || !isFeedback) && time_ntp-lastLogT_ntp > 2*65536) { // 2s in Q16
         if (!isFeedback) {
	        cerr << "No RTCP feedback received" << endl;
         } else {
	        float time_s = time_ntp/65536.0f;
           char s[500];
           screamTx->getStatistics(time_s, s);
           if (sierraLog)
             cout << s << endl << "      CellId, RSRP, RSSI, SINR: {" << sierraLogString << "}" << endl << endl;
           else
             cout << s << endl;
         }
         lastLogT_ntp = time_ntp;
       }
       if (verbose && time_ntp-lastLogTv_ntp > 13107) { // 0.2s in Q16
         if (isFeedback) {
           float time_s = time_ntp/65536.0f;
           char s[500];
           char s1[500];
           screamTx->getVeryShortLog(time_s, s1);
           if (sierraLog)
             sprintf(s,"%8.3f, %s %s ", time_s, s1, sierraLogString);
           else
             sprintf(s,"%8.3f, %s ", time_s, s1);

           cout << s << endl;
           /*
            * Send statistics to receiver this can be used to
            * verify reliability of remote control
            */
           s1[0] = 0x80;
           s1[1] = 0x7F; // Set PT = 0x7F for statistics packet
           memcpy(&s1[2],s,strlen(s));
           sendPacket(s1, strlen(s)+2);
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
}
