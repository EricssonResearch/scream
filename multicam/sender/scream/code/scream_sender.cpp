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
#include <math.h>
struct itimerval timer;
struct sigaction sa;

using namespace std;

//#define V2
/*
 * Stub function
 */
void packet_free(void* a, uint32_t ssrc) {//
}

/*
* This define is commented if diagnostics is
* run with a fake video coder
*/
#define USE_REAL_IP_CAM

#define BUFSIZE 2048
#define HTTP_BUF_SIZE 10000
#define MAX_SOURCES 4
#define KEEP_ALIVE_PKT_SIZE 1


int PT = 96;
int http_sock = 0;
int fd_out_rtp;
int fd_out_ctrl;
int fd_in_rtp[MAX_SOURCES];
int fd_out_rtcp[MAX_SOURCES];
RtpQueue *rtpQueue[MAX_SOURCES] = { 0, 0, 0};
int nSources = 0;
float delayTarget = 0.1f;
float minPaceInterval = 0.002f;

//test code for python
char* python_IP = "127.0.0.1";
int python_port = 30200;
int fd_python;
struct sockaddr_in python_addr;

FILE *fp_log = 0;
#define ECN_CAPABLE
/*
* ECN capable
* -1 = Not-ECT
* 0 = ECT(0)
* 1 = ECT(1)
*/
int ect = -1;

char *out_ip = "192.168.0.21";
int out_port = 30110;
char *in_ip = "127.0.0.1";
char *in_ip_rtcp = "";
int in_port[MAX_SOURCES] = {30000,30002,30004,30006};
uint32_t in_ssrc[MAX_SOURCES] = {1, 2, 3, 4};
uint32_t in_ssrc_network[MAX_SOURCES]; // Network ordered versions of in_ssrc
float priority[MAX_SOURCES] = { 1.0, 0.5, 0.1, 1.0};
unsigned char *rtpBufs[MAX_SOURCES][kRtpQueueSize];
float congestionScaleFactor = 0.9;
int bytesInFlightHistSize = 5;
float maxTotalRate = 100000000;
float rateInit[MAX_SOURCES]={3000,3000,1000,1000};
float rateMin[MAX_SOURCES]={3000,3000,1000,1000};
float rateMax[MAX_SOURCES]={30000,30000,10000,10000};
#ifdef V2
ScreamV2Tx *screamTx = 0;
float pacingHeadroom = 1.5;
float multiplicativeIncreaseFactor = 1.0;
#else
ScreamV1Tx *screamTx = 0;
float rateIncrease[MAX_SOURCES]={10000,10000,10000,10000};
float fastIncreaseFactor = 1.0;
float pacingHeadroom = 1.2;
bool isNewCc = false;
#endif
float rateScale[MAX_SOURCES]={1.0,1.0,1.0,1.0};

struct sockaddr_in in_rtp_addr[MAX_SOURCES];
struct sockaddr_in out_rtcp_addr[MAX_SOURCES];
struct sockaddr_in out_rtp_addr;
struct sockaddr_in out_ctrl_addr;
struct sockaddr_in in_rtcp_addr;
struct sockaddr_in http_addr;

socklen_t addrlen_out_rtp;
socklen_t addrlen_dummy_rtcp;
uint32_t lastLogT_ntp = 0;
uint32_t lastQualityControlT_ntp;

socklen_t addrlen_in_rtp[MAX_SOURCES] = {
    sizeof(in_rtp_addr[0]),
    sizeof(in_rtp_addr[1]),
    sizeof(in_rtp_addr[2])};
socklen_t addrlen_in_rtcp = sizeof(in_rtcp_addr);
pthread_mutex_t lock_scream;
pthread_mutex_t lock_rtp_queue;

// Accumulated pace time, used to avoid starting very short pace timers
//  this can save some complexity at very higfh bitrates
float accumulatedPaceTime = 0.0f;
bool paceTimerRunning = false;

const void sendCoderCommand(char *buf, char *ip);
void *txRtpThread(void *arg);
void *videoControlThread(void *arg);
int setup();
void *rxRtcpThread(void *arg);
void *rxRtpThread0(void *arg);
void *rxRtpThread1(void *arg);
void *rxRtpThread2(void *arg);
void *rxRtpThread3(void *arg);

double t0;
uint32_t lastT_ntp = 0;
uint32_t lastRtcpT_ntp = 0;

uint32_t getTimeInNtp(){
  struct timeval tp;
  gettimeofday(&tp, NULL);
  double time = tp.tv_sec + tp.tv_usec*1e-6-t0;
  uint64_t ntp64 = uint64_t(time*65536.0);
  uint32_t ntp = 0xFFFFFFFF & ntp64;
  return ntp;
}

volatile sig_atomic_t done = 0;
bool stopThread = false;
void stopAll(int signum)
{
    stopThread = true;
}

void closeSockets(int signum)
{
    exit(0);
}

/*
* Send a packet
*/
void sendPacket(char* buf, int size) {
    sendto(fd_out_rtp, buf, size, 0, (struct sockaddr *)&out_rtp_addr, sizeof(out_rtp_addr));
}

void readList(char *buf, float *list) {
   char s1[100];
   strcpy(s1,buf);
   char *s = strtok(s1,":");
   int n=0;
   while (s != NULL && n < MAX_SOURCES) {
	   	cerr << s << endl;
	   	list[n] = atof(s);
	   	n++;
	   	s = strtok(NULL,":");
   }
}

int main(int argc, char* argv[]) {

   struct timeval tp;
   gettimeofday(&tp, NULL);
   t0 = tp.tv_sec + tp.tv_usec*1e-6;
   lastT_ntp = getTimeInNtp();lastLogT_ntp = lastT_ntp;
   lastQualityControlT_ntp = lastT_ntp;
   /*
    * Parse command line
    */
    if (argc <= 1) {
#ifdef V2
        cerr << "Usage : " << endl << " scream_sender <options> nsources out_ip out_port prio_1 ..  prio_n" << endl;
        cerr << " -ect n             : ECN capable transport. n = 0 for ECT(0), n=1 for ECT(1). Default Not-ECT" << endl;
        cerr << " -cscale val        : Congestion scale factor, range [0.5..1.0], default = 0.9" << endl;
        cerr << "                      it can be necessary to set scale 1.0 if the LTE modem drops packets" << endl;
        cerr << "                      already at low congestion levels." << endl;
        cerr << " -delaytarget val   : Sets a queue delay target (default = 0.1s) " << endl;
        cerr << " -mulincrease val   : multiplicative increase factor (default 0.05)" << endl;
        cerr << " -in_ip_rtcp_addr   : Set in_ip_rtcp_addr" << endl;
        cerr << "                      a larger memory can be beneficial in remote applications where the video input" << endl;
        cerr << "                      is static for long periods. " << endl;
        cerr << " -maxtotalrate val  : Set max total bitrate [kbps], default 100000." << endl;
        cerr << " -pacingheadroom val: Set packet pacing headroom, default 1.5." << endl;
        cerr << " -log log_file      : Save detailed per-ACK log to file" << endl;
        cerr << " -ratemax list      : Set max rate [kbps] for streams" << endl;
        cerr << "    example -ratemax 30000:20000" << endl;
        cerr << " -ratemin list      : Set min rate [kbps] for streams" << endl;
        cerr << "    example -ratemin 3000:3000" << endl;
        cerr << " -rateinit list     : Set init rate [kbps] for streams" << endl;
        cerr << "    example -rateinit 3000:3000}" << endl;
        cerr << " -priority list     : Set stream priorities" << endl;
        cerr << "    example -priority 1.0:0.5:0.2:0.1" << endl;
        cerr << " -ratescale list    : Compensate for systematic error in actual vs desired rate" << endl;
        cerr << "    example -ratescale 0.6:0.5:1.0:1.0" << endl;

        cerr << " nsources           : Number of sources, min=1, max=" << MAX_SOURCES << endl;
        cerr << " out_ip             : remote (SCReAM receiver) IP address" << endl;
        cerr << " out_port           : remote (SCReAM receiver) port" << endl;
        cerr << " Media sources: " << endl;
        cerr << "   0 = Front camera encoded with omxh264" << endl;
        cerr << "   1 = Rear camera encoded with omxh264" << endl;
        cerr << "   2 = Not used" << endl;
        cerr << "   3 = Not used" << endl;
        cerr << endl;
        cerr << "  Note. lists should not contain white space and can have 1 to 4 elements, delimited by ':' " << endl;
        exit(-1);
#else
        cerr << "Usage : " << endl << " scream_sender <options> nsources out_ip out_port prio_1 ..  prio_n" << endl;
        cerr << " -newcc             : Use new congestion control algorithm (dec 2022)" << endl;
        cerr << " -ect n             : ECN capable transport. n = 0 for ECT(0), n=1 for ECT(1). Default Not-ECT" << endl;
        cerr << " -cscale val        : Congestion scale factor, range [0.5..1.0], default = 0.9" << endl;
        cerr << "                      it can be necessary to set scale 1.0 if the LTE modem drops packets" << endl;
        cerr << "                      already at low congestion levels." << endl;
        cerr << " -delaytarget val   : Sets a queue delay target (default = 0.1s) " << endl;
        cerr << " -cwvmem val        : Sets the memory of the congestion window validation (default 5s), max 60s" << endl;
        cerr << " -fincrease val     : Fast increase factor for newcc (default 1.0)" << endl;
        cerr << " -in_ip_rtcp_addr   : Set in_ip_rtcp_addr" << endl;
        cerr << "                      a larger memory can be beneficial in remote applications where the video input" << endl;
        cerr << "                      is static for long periods. " << endl;
        cerr << " -maxtotalrate val  : Set max total bitrate [kbps], default 100000." << endl;
        cerr << " -pacingheadroom val: Set packet pacing headroom, default 1.2." << endl;
        cerr << " -log log_file      : Save detailed per-ACK log to file" << endl;
        cerr << " -ratemax list      : Set max rate [kbps] for streams" << endl;
        cerr << "    example -ratemax 30000:20000" << endl;
        cerr << " -ratemin list      : Set min rate [kbps] for streams" << endl;
        cerr << "    example -ratemin 3000:3000" << endl;
        cerr << " -rateinit list     : Set init rate [kbps] for streams" << endl;
        cerr << "    example -rateinit 3000:3000}" << endl;
        cerr << " -rateincrease list : Set rate increase speed [kbps/s] for streams" << endl;
        cerr << "    example -rateincrease 10000,10000" << endl;
        cerr << " -priority list     : Set stream priorities" << endl;
        cerr << "    example -priority 1.0:0.5:0.2:0.1" << endl;
        cerr << " -ratescale list    : Compensate for systematic error in actual vs desired rate" << endl;
        cerr << "    example -ratescale 0.6:0.5:1.0:1.0" << endl;

        cerr << " nsources           : Number of sources, min=1, max=" << MAX_SOURCES << endl;
        cerr << " out_ip             : remote (SCReAM receiver) IP address" << endl;
        cerr << " out_port           : remote (SCReAM receiver) port" << endl;
        cerr << " Media sources: " << endl;
        cerr << "   0 = Front camera encoded with omxh264" << endl;
        cerr << "   1 = Rear camera encoded with omxh264" << endl;
        cerr << "   2 = Not used" << endl;
        cerr << "   3 = Not used" << endl;
        cerr << endl;
        cerr << "  Note. lists should not contain white space and can have 1 to 4 elements, delimited by ':' " << endl;
        exit(-1);
#endif
    }
    int ix = 1;
    int nExpectedArgs = 1 + 2 + 1;
    while (strstr(argv[ix], "-")) {
        if (strstr(argv[ix], "-ect")) {
            ect = atoi(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            if (ect < 0 || ect > 1) {
                cerr << "ect must be 0 or 1" << endl;
                exit(0);
            }
        }
        if (strstr(argv[ix], "-cscale")) {
            congestionScaleFactor = atof(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
        if (strstr(argv[ix], "-delaytarget")) {
            delayTarget = atof(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
        if (strstr(argv[ix], "-priority")) {
			readList(argv[ix + 1],priority);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
        if (strstr(argv[ix], "-ratemax")) {
			readList(argv[ix + 1],rateMax);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
        if (strstr(argv[ix], "-ratemin")) {
			readList(argv[ix + 1],rateMin);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
        if (strstr(argv[ix], "-rateinit")) {
			readList(argv[ix + 1],rateInit);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
        if (strstr(argv[ix], "-ratescale")) {
			 readList(argv[ix + 1],rateScale);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
        if (strstr(argv[ix], "-maxtotalrate")) {
            maxTotalRate = atoi(argv[ix + 1])*1000.0f;
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
        if (strstr(argv[ix],"-log")) {
           fp_log = fopen(argv[ix+1],"w");
           ix+=2;
           nExpectedArgs += 2;
           continue;
        }
        if (strstr(argv[ix], "-in_ip_rtcp_addr")) {
            in_ip_rtcp = strdup(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
        if (strstr(argv[ix], "-pacingheadroom")) {
            pacingHeadroom = atof(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
#ifdef V2
        if (strstr(argv[ix], "-mulincrease")) {
            multiplicativeIncreaseFactor = atof(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
#else
        if (strstr(argv[ix], "-fincrease")) {
            fastIncreaseFactor = atof(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }

        if (strstr(argv[ix], "-newcc")) {
            isNewCc = true;
            ix += 1;
            nExpectedArgs += 1;
            continue;
        }
        if (strstr(argv[ix], "-cwvmem")) {
            bytesInFlightHistSize = atoi(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            if (bytesInFlightHistSize > kBytesInFlightHistSizeMax || bytesInFlightHistSize < 2) {
                cerr << "cwvmem must be in range [2 .. " << kBytesInFlightHistSizeMax << "]" << endl;
                exit(0);
            }
            continue;
        }
        if (strstr(argv[ix], "-rateincrease")) {
      			readList(argv[ix + 1],rateIncrease);
            ix += 2;
            nExpectedArgs += 2;
            continue;
        }
#endif
        fprintf(stderr, "unexpected arg %s\n", argv[ix]);
        ix += 1;
        nExpectedArgs += 1;
    }

  //test code for python
  python_addr.sin_family = AF_INET;
  python_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  python_addr.sin_port = htons(python_port);

  if((fd_python = socket(AF_INET, SOCK_DGRAM,0))<0){
	perror("cannot create socket for python package");
	return 0;
  }

    nSources = atoi(argv[ix]);
    //nExpectedArgs += nSources; // prio
    ix++;
    if (nSources < 1 || nSources > MAX_SOURCES) {
        cerr << "number of sources must be in interval [0.." << MAX_SOURCES << "]" << endl;
        exit(0);
    }
    if (argc - 1 != nExpectedArgs - 1) {
        cerr << "expected " << (nExpectedArgs - 1) << " arguments, but see " << (argc - 1) << " ditto ?" << endl;
        exit(0);
    }
    out_ip = argv[ix]; ix++;
    out_port = atoi(argv[ix]); ix++;
    /*for (int n = 0; n < nSources; n++) {
        char s[20];
        priority[n] = atof(argv[ix]); ix++;
    }
    */

    if (setup() == 0)
        return 0;



    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = stopAll;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    char buf[HTTP_BUF_SIZE];

    cerr << "Scream sender started " << endl;
    screamTx->setDetailedLogFp(fp_log);
    pthread_mutex_init(&lock_scream, NULL);

    pthread_t tx_rtp_thread;
    pthread_t rx_rtp_thread[MAX_SOURCES];
    pthread_t rx_rtcp_thread;
    pthread_t video_thread;
    /* Create Transmit RTP thread */
    pthread_create(&tx_rtp_thread, NULL, txRtpThread, "RTCP thread...");
    cerr << "RX RTP thread(s) started" << endl;

    /* Create Receive RTP thread(s) */
    pthread_create(&rx_rtp_thread[0], NULL, rxRtpThread0, "RTP thread 0...");

    if (nSources > 1)
        pthread_create(&rx_rtp_thread[1], NULL, rxRtpThread1, "RTP thread 1...");
    if (nSources > 2)
        pthread_create(&rx_rtp_thread[2], NULL, rxRtpThread2, "RTP thread 2...");
    if (nSources > 2)
        pthread_create(&rx_rtp_thread[3], NULL, rxRtpThread3, "RTP thread 3...");
    cerr << "RX RTP thread(s) started" << endl;

    /* Create RTCP thread */
    pthread_create(&rx_rtcp_thread, NULL, rxRtcpThread, "RTCP thread...");
    cerr << "RTCP thread started" << endl;

    /* Create Video control thread */
    pthread_create(&video_thread, NULL, videoControlThread, "Video control thread...");
    cerr << "Media control thread started" << endl;

    while (!stopThread) {
        uint32_t time_ntp;
        float time_s;
        char s[1000];
        time_ntp = getTimeInNtp();


        if (time_ntp - lastLogT_ntp > 16384) { // 0.25s in Q16
			lastLogT_ntp = time_ntp;
            char s1[1000];

            //pthread_mutex_lock(&lock_scream);
            time_ntp = getTimeInNtp();
            time_s = (time_ntp) / 65536.0f;
            screamTx->getShortLog(time_s, s1);
            int16_t qualityIndex = int16_t(screamTx->getQualityIndex(time_s,rateMax[0]*1000.0f,0.05f)+0.5f);

            //pthread_mutex_unlock(&lock_scream);
            sprintf(s,"%8.3f, %d, %s ", time_s, qualityIndex, s1);
            cout << s << endl;
            /*
            * Send statistics to receiver this can be used to
            * verify reliability of remote control
            */
            s1[0] = 0x80;
            s1[1] = 0x7F; // Set PT = 0x7F for statistics packet
            memcpy(&s1[2], s, strlen(s));
            sendPacket(s1, strlen(s) + 2);
            /*
            * Send SSRC map to receiver to enable
            * correct mapping of cameras to displays
            */
            s1[0] = 0x80;
            s1[1] = 0x7E; // Set PT = 0x7E for SSRC map
            for (int n = 0; n < nSources; n++) {
                /*
                * Write the SSRCs (in network byte order)
                */
                uint32_t tmp_l;
                memcpy(s1 + 2 + n * 4, &in_ssrc_network[n], 4);
            }
            sendPacket(s1, 2 + nSources * 4);


           // pthread_mutex_lock(&lock_scream);
            time_ntp = getTimeInNtp();
            time_s = (time_ntp) / 65536.0f;
            qualityIndex = int16_t(screamTx->getQualityIndex(time_s,rateMax[0]*1000.0f,0.05f)+0.5f);
            //pthread_mutex_unlock(&lock_scream);
            bool sendBreakSignal=false;
            if (time_ntp-lastRtcpT_ntp > 8192) {
                sendBreakSignal = true;
            }

            if (sendBreakSignal) {
                cerr << "RTCP feedback missing" << endl;
                /*
                * Signal to RC car to break
                */

                qualityIndex=-1;
            }
            sprintf(s,"%d",qualityIndex);
            sendto(fd_python,(const char*)s,strlen(s),MSG_CONFIRM,(const struct sockaddr*)&python_addr, sizeof(python_addr));

            //pthread_mutex_lock(&lock_scream);
            time_ntp = getTimeInNtp();
            time_s = (time_ntp) / 65536.0f;
            qualityIndex = int16_t(screamTx->getQualityIndex(time_s,rateMax[0]*1000.0f,0.05f)+0.5f);
            //pthread_mutex_unlock(&lock_scream);

            qualityIndex = htons(qualityIndex);

            sendto(fd_out_ctrl, &qualityIndex, 2, 0, (struct sockaddr *)&out_ctrl_addr, sizeof(out_ctrl_addr));

            if (fp_log) {
                fflush(fp_log);
            }
        }

        usleep(50000);
    };

    usleep(500000);
    close(fd_out_rtp);
    close(fd_out_ctrl);
    for (int n = 0; n < nSources; n++)
        close(fd_in_rtp[n]);
    if (fp_log)
      fclose(fp_log);
}

/*
* Lotsalotsa functions...
*/

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
  |            contributing source -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  */
void parseRtp(unsigned char *buf, uint16_t* seqNr, uint32_t* timeStamp, unsigned char *pt) {
    uint16_t rawSeq;
    uint32_t rawTs;
    memcpy(&rawSeq, buf + 2, 2);
    memcpy(&rawTs, buf + 4, 4);
    memcpy(pt, buf + 1, 1);
    *seqNr = ntohs(rawSeq);
    *timeStamp = ntohl(rawTs);
}

/*
 * Transmit a packet if possible.
 * If not allowed due to packet pacing restrictions,
 * then sleep
 */
void *txRtpThread(void *arg) {
    int size;
    uint16_t seqNr;
    //char buf[2000];
    char *buf;
    uint32_t time_ntp = getTimeInNtp();
    int sleepTime_us = 10;
    float retVal = 0.0f;
    int sizeOfQueue;
    uint32_t ssrc;

    for (;;) {
        if (stopThread) {
            return NULL;
        }
        time_ntp = getTimeInNtp();
        sleepTime_us = 1000;
        retVal = 0.0f;

        /*
        * Check if send window allows transmission and there is atleast one stream
        *  with RTP packets in queue
        */
        pthread_mutex_lock(&lock_scream);
        retVal = screamTx->isOkToTransmit(getTimeInNtp(), ssrc);
        pthread_mutex_unlock(&lock_scream);

        if (retVal != -1.0f) {
            /*
            * Send window allows transmission and atleast one stream has packets in RTP queue
            * Get RTP queue for selected stream (ssrc)
            */
            RtpQueue *rtpQueue = (RtpQueue*)screamTx->getStreamQueue(ssrc);

            pthread_mutex_lock(&lock_rtp_queue);
            sizeOfQueue = rtpQueue->sizeOfQueue();
            pthread_mutex_unlock(&lock_rtp_queue);
            do {
                if (retVal == -1.0f) {
                    sizeOfQueue = 0;
                }
                else {
                    if (retVal > 0.0f)
                        accumulatedPaceTime += retVal;
                    if (retVal != -1.0) {
                        /*
                        * Get RTP packet from the selected RTP queue
                        */
                        pthread_mutex_lock(&lock_rtp_queue);
                        bool isMark;
						uint32_t ssrc_unused;

                        rtpQueue->pop(&buf, size, ssrc_unused, seqNr, isMark);
                        pthread_mutex_unlock(&lock_rtp_queue);

                        /*
                        * Transmit RTP packet
                        */
                        sendPacket(buf, size);

                        /*
                        * Register transmitted RTP packet
                        */
                        pthread_mutex_lock(&lock_scream);
                        retVal = screamTx->addTransmitted(getTimeInNtp(), ssrc, size, seqNr, false);
                        pthread_mutex_unlock(&lock_scream);
                    }

                    /*
                    * Check if send window allows transmission and there is atleast one stream
                    *  with RTP packets in queue

                    */
                    retVal = screamTx->isOkToTransmit(getTimeInNtp(), ssrc);
                    if (retVal == -1.0f) {
                        /*
                        * Send window full or no packets in any RTP queue
                        */
                        sizeOfQueue = 0;
                    }
                    else {
                        /*
                        * Send window allows transmission and atleast one stream has packets in RTP queue
                        * Get RTP queue for selected stream (ssrc)
                        */
                        rtpQueue = (RtpQueue*)screamTx->getStreamQueue(ssrc);
                        pthread_mutex_lock(&lock_rtp_queue);
                        sizeOfQueue = rtpQueue->sizeOfQueue();
                        pthread_mutex_unlock(&lock_rtp_queue);
                    }
                }

            } while (accumulatedPaceTime <= minPaceInterval &&
                retVal != -1.0f &&
                sizeOfQueue > 0);

            if (accumulatedPaceTime > 0) {
                /*
                * Sleep for a while, this paces out packets
                */
                sleepTime_us = int(std::max(minPaceInterval,accumulatedPaceTime)*1e6f);
                accumulatedPaceTime = 0.0f;
            }
        }
        usleep(sleepTime_us);
    }
    return NULL;
}


int recvRtp(unsigned char *buf_rtp, int ix) {
    /*
    * Wait for RTP packets from the coder
    */
    int recvlen = recvfrom(fd_in_rtp[ix],
        buf_rtp,
        BUFSIZE,
        0,
        (struct sockaddr *)&in_rtp_addr[ix], &addrlen_in_rtp[ix]);
    if (stopThread)
        return 0;

    return recvlen;
}

void processRtp(unsigned char *buf_rtp, int recvlen, int ix) {
    uint16_t seqNr;
    uint32_t ts;
    unsigned char pt;

    parseRtp(buf_rtp, &seqNr, &ts, &pt);
    uint32_t pt_ = pt & 0x7F;
    bool isMark = (pt & 0x80) != 0;
    if ((pt & 0x7F) == PT) {
        /*
        * Overwrite SSRC with new value, easier to sort out media sources
        * on receiver side
        */
        memcpy(&buf_rtp[8], &in_ssrc_network[ix], 4);
        pthread_mutex_lock(&lock_rtp_queue);
        uint32_t ssrc_unused = 0;
        rtpQueue[ix]->push(buf_rtp, recvlen, ssrc_unused, seqNr, isMark, (getTimeInNtp())/65536.0f);
        pthread_mutex_unlock(&lock_rtp_queue);

        pthread_mutex_lock(&lock_scream);
        screamTx->newMediaFrame(getTimeInNtp(), in_ssrc[ix], recvlen, isMark);
        pthread_mutex_unlock(&lock_scream);
    }
}

/*
* One thread for each media source (camera)
*/
void *rxRtpThread0(void *arg) {
	for (int n=0; n < kRtpQueueSize; n++) {
	  rtpBufs[0][n] = new unsigned char[BUFSIZE];
    }
    int rtpBufIx = 0;
    //unsigned char buf_rtp[BUFSIZE];
    for (;;) {
		unsigned char *buf_rtp = rtpBufs[0][rtpBufIx];
        int len = recvRtp(buf_rtp, 0);
        if (len > 0) {
            processRtp(buf_rtp, len, 0);
        }
        rtpBufIx = (rtpBufIx+1) % kRtpQueueSize;
        usleep(10);
    }
    /*
     * ToDo delete rtpBufs ?
     */
    return NULL;
}
void *rxRtpThread1(void *arg) {
	for (int n=0; n < kRtpQueueSize; n++) {
	  rtpBufs[1][n] = new unsigned char[BUFSIZE];
    }
    int rtpBufIx = 0;
    for (;;) {
		unsigned char *buf_rtp = rtpBufs[1][rtpBufIx];
        int len = recvRtp(buf_rtp, 1);
        if (len > 0) {
            processRtp(buf_rtp, len, 1);
        }
        rtpBufIx = (rtpBufIx+1) % kRtpQueueSize;
        usleep(10);
    }
    return NULL;
}
void *rxRtpThread2(void *arg) {
	for (int n=0; n < kRtpQueueSize; n++) {
	  rtpBufs[2][n] = new unsigned char[BUFSIZE];
    }
    int rtpBufIx = 0;
    for (;;) {
		unsigned char *buf_rtp = rtpBufs[2][rtpBufIx];
        int len = recvRtp(buf_rtp, 2);
        if (len > 0) {
            processRtp(buf_rtp, len, 2);
        }
        rtpBufIx = (rtpBufIx+1) % kRtpQueueSize;
        usleep(10);
    }
    return NULL;
}
void *rxRtpThread3(void *arg) {
	for (int n=0; n < kRtpQueueSize; n++) {
	  rtpBufs[3][n] = new unsigned char[BUFSIZE];
    }
    int rtpBufIx = 0;
    for (;;) {
		unsigned char *buf_rtp = rtpBufs[3][rtpBufIx];
        int len = recvRtp(buf_rtp, 3);
        if (len > 0) {
            processRtp(buf_rtp, len, 3);
        }
        rtpBufIx = (rtpBufIx+1) % kRtpQueueSize;
        usleep(10);
    }
    return NULL;
}

void *rxRtcpThread(void *arg) {
    /*
    * Wait for RTCP packets from receiver
    */
    unsigned char buf_rtcp[BUFSIZE];
    char s[100];
    for (;;) {
        int recvlen = recvfrom(fd_out_rtp, buf_rtcp, BUFSIZE, 0, (struct sockaddr *)&in_rtcp_addr, &addrlen_in_rtcp);
        if (stopThread)
            return;
        if (recvlen > KEEP_ALIVE_PKT_SIZE) {
			sprintf(s, "%1.4f", getTimeInNtp() / 65536.0f);
			screamTx->setTimeString(s);
            pthread_mutex_lock(&lock_scream);
            screamTx->incomingStandardizedFeedback(getTimeInNtp(), buf_rtcp, recvlen);
            lastRtcpT_ntp = getTimeInNtp();
            pthread_mutex_unlock(&lock_scream);
        } else {
            printf("%s %u %d\n", __FUNCTION__, __LINE__, recvlen);
        }
    }
    return NULL;
}

int setup() {
    for (int n = 0; n < nSources; n++) {
        in_ssrc_network[n] = htonl(in_ssrc[n]);
        in_rtp_addr[n].sin_family = AF_INET;
        in_rtp_addr[n].sin_addr.s_addr = htonl(INADDR_ANY);
        in_rtp_addr[n].sin_port = htons(in_port[n]);

        if ((fd_in_rtp[n] = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            char s[100];
            sprintf(s, "cannot create socket for incoming RTP media %d", n + 1);
            perror(s);
            return 0;
        }

        if (bind(fd_in_rtp[n], (struct sockaddr *)&in_rtp_addr[n], sizeof(in_rtp_addr[n])) < 0) {
            char s[100];
            sprintf(s, "bind incoming_rtp_addr %d failed", n + 1);
            perror(s);
            return 0;
        }
        else{
            cerr << "Listen on port " << in_port[n] << " to receive RTP media " << (n + 1) << endl;
        }

        out_rtcp_addr[n].sin_family = AF_INET;
        inet_aton(in_ip, (in_addr*)&out_rtcp_addr[n].sin_addr.s_addr);
        //inet_aton("192.168.1.167", (in_addr*)&out_rtcp_addr[n].sin_addr.s_addr);
        //out_rtcp_addr[n].sin_addr.s_addr = htonl(INADDR_ANY);
        out_rtcp_addr[n].sin_port = htons(in_port[n]+1);
        if ((fd_out_rtcp[n] = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
          perror("cannot create socket for outgoing RTCP control");
          return 0;
        }
    }

    out_ctrl_addr.sin_family = AF_INET;
    inet_aton(in_ip, (in_addr*)&out_ctrl_addr.sin_addr.s_addr);
    out_ctrl_addr.sin_port = htons(33000);
    if ((fd_out_ctrl = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
       perror("cannot create socket for car remote control");
      return 0;
    }


    memset(&out_rtp_addr, 0, sizeof(struct sockaddr_in));
    out_rtp_addr.sin_family = AF_INET;
    inet_aton(out_ip, (in_addr*)&out_rtp_addr.sin_addr.s_addr);
    out_rtp_addr.sin_port = htons(out_port);
    addrlen_out_rtp = sizeof(out_rtp_addr);

    if ((fd_out_rtp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("cannot create socket for outgoing RTP media");
        return 0;
    }

    /*
    * Set send buf reasonably high to avoid socket blocking
    */
    int sendBuff = 1000000;
    int res = setsockopt(fd_out_rtp, SOL_SOCKET, SO_SNDBUF, &sendBuff, sizeof(sendBuff));

    /*
    * Set ECN capability for outgoing socket using IP_TOS
    */
#ifdef ECN_CAPABLE
	int iptos = 0;
	if (ect == 0 || ect == 1)
		iptos = 2 - ect;
    res = setsockopt(fd_out_rtp, IPPROTO_IP, IP_TOS, &iptos, sizeof(iptos));
    if (res < 0) {
        cerr << "Not possible to set ECN bits" << endl;
    }
    int tmp = 0;
    res = getsockopt(fd_out_rtp, IPPROTO_IP, IP_TOS, &tmp, sizeof(tmp));
    if (iptos == tmp) {
        cerr << "ECN set successfully" << endl;
    }
    else {
        cerr << "ECN bits _not_ set successfully ? " << iptos << " " << tmp << endl;
    }
#endif
    /*
    * Socket for incoming RTP media
    */
    in_rtcp_addr.sin_family = AF_INET;
    in_rtcp_addr.sin_port = htons(out_port);
    if (strlen(in_ip_rtcp) == 0) {
        in_rtcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        printf("in_ip_rtcp=%s\n", in_ip_rtcp);
        inet_aton(in_ip_rtcp, (in_addr*)&in_rtcp_addr.sin_addr.s_addr);
    }

    if (bind(fd_out_rtp, (struct sockaddr *)&in_rtcp_addr, sizeof(in_rtcp_addr)) < 0) {
        perror("bind outgoing_rtp_addr failed");
        fprintf(stderr, "port = %u\n", out_port);
        return 0;
    }
    else {
        cerr << "Listen on port " << out_port << " to receive RTCP from encoder " << endl;
    }

#ifdef V2
    screamTx = new ScreamV2Tx(
        congestionScaleFactor,
        congestionScaleFactor,
        delayTarget,
        12500,
        pacingHeadroom,
        pacingHeadroom,
        2.0f,
        multiplicativeIncreaseFactor,
        ect == 1,
        false,
        false,
        false);
        screamTx->setPostCongestionDelay(4.0);
#else
    screamTx = new ScreamV1Tx(
        congestionScaleFactor,
        congestionScaleFactor,
        delayTarget,
        false,
        1.0f,
        10.0f,
        12500,
        pacingHeadroom,
        bytesInFlightHistSize,
        (ect == 1),
        false,
        false,
        2.0f,
        isNewCc);
        screamTx->setPostCongestionDelay(1.0);
        screamTx->setFastIncreaseFactor(fastIncreaseFactor);
#endif
    screamTx->setCwndMinLow(10000); // ~1.5Mbps at RTT = 50ms
    screamTx->setMaxTotalBitrate(maxTotalRate);

    for (int n = 0; n < nSources; n++) {
        rtpQueue[n] = new RtpQueue();

#ifdef V2
        screamTx->registerNewStream(rtpQueue[n],
            in_ssrc[n], priority[n],
            rateMin[n]*1000, rateInit[n]*1000, rateMax[n]*1000, 0.2f,false);
#else
        screamTx->registerNewStream(rtpQueue[n],
            in_ssrc[n], priority[n],
            rateMin[n]*1000, rateInit[n]*1000, rateMax[n]*1000, rateIncrease[n]*1000, 0.5f,
            0.1f, 0.1f, 0.2f,
            congestionScaleFactor, congestionScaleFactor, true);
#endif
    }
    return 1;
}

float lastLossEpochT[2] = {-1.0,-1.0};

int nn=0;
void *videoControlThread(void *arg) {

    char buf[5];
    while (!stopThread) {
		nn++;
        for (int n = 0; n < nSources; n++) {
            /*
            * Poll rate change for all media sources
            */
            float rate = screamTx->getTargetBitrate(in_ssrc[n]);
            if (rate > 0) {
              uint32_t rate_32 = (uint32_t)(rate*rateScale[n]);
              rate_32 = htonl(rate_32);
              memcpy(buf,&rate_32,4);
              buf[4] = 0;
              if (n==0 || n==1) {
				/*
				 * Force-IDR in case of loss
				 */
			    struct timeval tp;
                gettimeofday(&tp, NULL);
                double time = tp.tv_sec + tp.tv_usec*1e-6-t0;

				if (screamTx->isLossEpoch(in_ssrc[n])) {
				    lastLossEpochT[n] = time;
				}
				if (lastLossEpochT[n] > 0.0 && time-lastLossEpochT[n] > 0.1) {
					/*
					 * Wait till 100ms after the last loss until force
					 * new IDR
					 */
					 buf[4] = 1;
					 lastLossEpochT[n] = -1.0f;
				}
			  }

              sendto(fd_out_rtcp[n], buf, 5, 0, (struct sockaddr *)&out_rtcp_addr[n], sizeof(out_rtcp_addr[n]));
            }
        }
        usleep(200000);
    }
}
