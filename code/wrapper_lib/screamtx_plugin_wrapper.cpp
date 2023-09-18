// Scream sender side wrapper
#include "ScreamTx.h"
#include "RtpQueue.h"
#include "sys/types.h"
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>

extern const char *log_tag;

typedef void (* ScreamSenderPushCallBack)(uint8_t *, uint8_t *, uint8_t);

typedef struct  stream_s {
    uint32_t ssrc;
    RtpQueue *rtpQueue;
    pthread_mutex_t lock_rtp_queue;
    ScreamSenderPushCallBack cb;
    uint8_t *cb_data;
    uint64_t rtpqueue_full;
    uint32_t encoder_rate;
    float lastLossEpochT;

} stream_t;

stream_t streams[kMaxStreams];

/*
 * Get the stream that matches ssrc
 */
stream_t *getStream(uint32_t ssrc) {
    if (ssrc > 0) {
            for (int n = 0; n < kMaxStreams; n++) {
                if (streams[n].ssrc == ssrc) {
                    return &streams[n];
                }
            }
    }
    printf("%s %u no stream  ssrc %u \n", __FUNCTION__, __LINE__, ssrc);
 	return NULL;
}
static uint32_t cur_n_streams = 0;
stream_t *addStream(uint32_t ssrc) {
 	for (int n = 0; n < kMaxStreams; n++) {
 		if (streams[n].ssrc == 0) {
            streams[n].ssrc = ssrc;
            streams[n].lastLossEpochT = -1.0;
            cur_n_streams++;
            printf("%s %u added ssrc %u index %d cur_n_streams %u \n", __FUNCTION__, __LINE__, ssrc, n, cur_n_streams);
 			return &streams[n];
 		}
 	}
    printf("%s %u can't add  ssrc %u \n", __FUNCTION__, __LINE__, ssrc);
 	return NULL;
}

using namespace std;

#define BUFSIZE 2048

float minPaceInterval = 0.002f;
int minPaceIntervalUs = 1900;

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
bool disablePacing = false;
int initRate = 1000;
int minRate = 1000;
int maxRate = 200000;
int rateIncrease = 10000;
float priority = 1.0;
float rateScale = 0.5f;
float rateMultiply = 1.0;
float dscale = 10.0f;
bool enableClockDriftCompensation = false;
static bool newCc = false;
bool isBurst = false;
float burstStartTime = -1.0;
float burstSleepTime = -1.0;
float packetPacingHeadroom=1.25f;
float txQueueSizeFactor = 0.1f;
float queueDelayGuard = 0.05f;
float hysteresis = 0.0;

uint32_t lastKeyFrameT_ntp = 0;
float runTime = -1.0;
bool stopThread = false;
pthread_t transmit_rtp_thread = 0;
bool sierraLog;

pthread_t stats_print_thread = 0;

float scaleFactor = 0.9f;
float delayTarget = 0.06f;
float maxRtpQueueDelayArg = 0.2f;
bool forceidr = false;
bool printSummary = true;

static uint32_t first_ntp = 0;
ScreamV1Tx *screamTx = 0;

uint32_t lastLogT_ntp = 0;
uint32_t lastLogTv_ntp = 0;
uint32_t tD_ntp = 0;//(INT64_C(1) << 32)*1000 - 5000000;
pthread_mutex_t lock_scream;

FILE *fp_log = 0;

bool ntp = false;
bool append = false;
bool itemlist = false;
bool detailed = false;

double t0=0;

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
 * Transmit a packet if possible.
 * If not allowed due to packet pacing restrictions,
 * then start a timer.
 */
void *transmitRtpThread(void *arg) {
  int size;
  uint16_t seqNr;
  bool isMark = false;
  void *buf;
  uint32_t time_ntp = getTimeInNtp();
  int sleepTime_us = 10;
  float retVal = 0.0f;
  int sizeOfQueue;
  uint32_t ssrc = 0;
  struct timeval start, end;
  useconds_t diff = 0;
  static bool first = false;
  stream_t *stream = NULL;
  printf("%s %u \n", __FUNCTION__, __LINE__);
  for (;;) {
    if (stopThread) {
      return NULL;
    }

    sleepTime_us = 1;
    retVal = 0.0f;
    time_ntp = getTimeInNtp();
    pthread_mutex_lock(&lock_scream);
    retVal = screamTx->isOkToTransmit(time_ntp, ssrc);
    pthread_mutex_unlock(&lock_scream);
    stream = getStream(ssrc);
    if (retVal != -1.0f) {
      pthread_mutex_lock(&stream->lock_rtp_queue);
      sizeOfQueue = stream->rtpQueue->sizeOfQueue();
      pthread_mutex_unlock(&stream->lock_rtp_queue);
      do {
         gettimeofday(&start, 0);
         time_ntp = getTimeInNtp();

         retVal = screamTx->isOkToTransmit(time_ntp, ssrc);
         stream = getStream(ssrc);
         /*
          * The pacing can cause packets to be discarded initially.
          * Disabling  packet pacing for the first few seconds
          */

         if ((disablePacing || (time_ntp - first_ntp < 5 * 65536)) // 5 s in Q16))
             && sizeOfQueue > 0 && retVal > 0.0f) {
            retVal = 0.0f;
         }
         if (retVal > 0.0f)
            accumulatedPaceTime += retVal;
         if (retVal != -1.0) {
           static int sleeps = 0;
           pthread_mutex_lock(&stream->lock_rtp_queue);
           stream->rtpQueue->pop(&buf, size, ssrc, seqNr, isMark);
           pthread_mutex_unlock(&stream->lock_rtp_queue);
           stream->cb(stream->cb_data, (uint8_t *)buf, 1);
           if ((cur_n_streams > 1) && (sleeps++ < 120)) {
               usleep(500);
           }
           time_ntp = getTimeInNtp();
           pthread_mutex_lock(&lock_scream);
           retVal = screamTx->addTransmitted(time_ntp, ssrc, size, seqNr, isMark);
           pthread_mutex_unlock(&lock_scream);
         }
         pthread_mutex_lock(&stream->lock_rtp_queue);
         sizeOfQueue = stream->rtpQueue->sizeOfQueue();
         pthread_mutex_unlock(&stream->lock_rtp_queue);
         gettimeofday(&end, 0);
         diff = end.tv_usec-start.tv_usec;
         accumulatedPaceTime = std::max(0.0f, accumulatedPaceTime-diff*1e-6f);
      } while (accumulatedPaceTime <= minPaceInterval &&
           retVal != -1.0f &&
           sizeOfQueue > 0);
      if (accumulatedPaceTime > 0) {
          sleepTime_us = std::min((int)(accumulatedPaceTime*1e6f), minPaceIntervalUs);
          accumulatedPaceTime = 0.0f;
      }
    }
    usleep(sleepTime_us);
    sleepTime_us = 0;
  }
  return NULL;
}

uint32_t lastT_ntp;

uint32_t rtcp_rx_time_ntp = 0;
#define KEEP_ALIVE_PKT_SIZE 1
int verbose = 0;

/*
 * Print current encoder rates
 */
void EncoderRates(void) {
    cout << " encoder_rate " ;
 	for (int n = 0; n < kMaxStreams; n++) {
 		if (streams[n].ssrc != 0) {
            cout  << " " << (streams[n].encoder_rate /1000) << " " ;
 		}
 	}
    cout  << "kbbps ";
}
void *
statsPrintThread(void *arg)
{
    printf("%s %u stopThread %d, runTime %f \n", __FUNCTION__, __LINE__, stopThread, runTime);
    while(!stopThread && (runTime < 0 || getTimeInNtp() < runTime*65536.0f)) {
        uint32_t time_ntp = getTimeInNtp();
        bool isFeedback = time_ntp - rtcp_rx_time_ntp < 65536; // 1s in Q16
        if ((printSummary || !isFeedback) && time_ntp - lastLogT_ntp > 2*65536) { // 2s in Q16
            if (!isFeedback) {
                cerr << "No RTCP feedback received" << endl;
            } else {
                float time_s = time_ntp/65536.0f;
                char s[1000];
                screamTx->getStatistics(time_s, s);
                if (sierraLog)
                    cout << s << endl << "      CellId, RSRP, RSSI, SINR: {" << sierraLogString << "}" << endl << endl;
                else
                    cout << s ;
                EncoderRates();
                cout << endl;

            }
            lastLogT_ntp = time_ntp;
        }
        if ((verbose > 0) && time_ntp-lastLogTv_ntp > 13107) { // 0.2s in Q16
            if (isFeedback) {
                float time_s = time_ntp/65536.0f;
                char s[1000];
                char s1[500];
                if (verbose == 1) {
                    screamTx->getVeryShortLog(time_s, s1);
                } else {
                    screamTx->getLogHeader(s1);
                    cout << s1 << endl;
                    screamTx->getLog(time_s, s1, 0, false);
                    sprintf(s,"%8.3f,%s ", time_s, s1);
                    cout << s << " ";
                    EncoderRates();
                    cout << endl;
                    screamTx->getShortLog(time_s, s1);
                }
                sprintf(s,"%8.3f, %s ", time_s, s1);
                cout << s;
                EncoderRates();
                cout << endl;
                /*
                 * Send statistics to receiver this can be used to
                 * verify reliability of remote control
                 */
#if 0
                s1[0] = 0x80;
                s1[1] = 0x7F; // Set PT = 0x7F for statistics packet
                memcpy(&s1[2],s,strlen(s));
                sendPacket(s1, strlen(s)+2);
#endif
            }
            lastLogTv_ntp = time_ntp;
        }
        usleep(50000);
    };
    stopThread = true;
    usleep(500000);
    if (fp_log)
      fclose(fp_log);
    return (NULL);
}

int tx_plugin_main(int argc, char* argv[], uint32_t ssrc)
{
  stream_t *stream = getStream(ssrc);
  struct timeval tp;
  gettimeofday(&tp, NULL);
  t0 = tp.tv_sec + tp.tv_usec*1e-6 - 1e-3;
  lastT_ntp = getTimeInNtp();

  /*
  * Parse command line
  */
  if (argc <= 1) {
    cerr << "SCReAM sender. Ericsson AB. Version 2020-12-17" << endl;
    cerr << "Usage : " << endl << " > scream_tx <options>  " << endl;
    cerr << "     -initrate value       set a start bitrate [kbps]" << endl;
    cerr << "                            example -initrate 2000 " << endl;
    cerr << "     -minrate  value       set a min bitrate [kbps], default 1000kbps" << endl;
    cerr << "                            example -minrate 1000 " << endl;
    cerr << "     -maxrate value        set a max bitrate [kbps], default 200000kbps" << endl;
    cerr << "                            example -maxrate 10000 " << endl;
    cerr << "     -rateincrease value   set a max allowed rate increase speed [kbps/s]," << endl;
    cerr << "                            default 10000kbps/s" << endl;
    cerr << "                            example -rateincrease 1000 " << endl;
    cerr << "     -priority  value      set stream priorities" << endl;
    cerr << "     -ratescale value      set a max allowed rate increase speed as a fraction of the " << endl;
    cerr << "                            current rate, default 0.5" << endl;
    cerr << "                            example -ratescale 1.0 " << endl;
    cerr << "     -ect n                ECN capable transport, n = 0 or 1 for ECT(0) or ECT(1)," << endl;
    cerr << "                            -1 for not-ECT (default)" << endl;
    cerr << "     -scale value          scale factor in case of loss or ECN event (default 0.9) " << endl;
    cerr << "     -dscale value         scale factor in case of increased delay (default 10.0) " << endl;
    cerr << "     -delaytarget value    set a queue delay target (default = 0.06s) " << endl;
    cerr << "     -paceheadroom value   set a packet pacing headroom (default = 1.25s) " << endl;
    cerr << "     -txqueuesizefactor value reaction to increased RTP queue delay (default 0.1) " << endl;
    cerr << "     -queuedelayguard value   reaction to increased network queue delay (default 0.05)" << endl;
    cerr << "     -forceidr             enable Force-IDR in case of loss"  << endl;
    cerr << "     -clockdrift           enable clock drift compensation for the case that the"  << endl;
    cerr << "                            receiver end clock is faster" << endl;
    cerr << "     -verbose value        print a more extensive log" << endl;
    cerr << "     -nosummary            don't print summary" << endl;
    cerr << "     -log logfile          save detailed per-ACK log to file" << endl;
    cerr << "     -ntp                  use NTP timestamp in logfile" << endl;
    cerr << "     -append               append logfile" << endl;
    cerr << "     -itemlist             add item list in beginning of log file" << endl;
    cerr << "     -detailed             detailed log, per ACKed RTP" << endl;
    cerr << "     -periodicdropinterval interval [s] between periodic drops in rate (default 60s)" << endl;
    cerr << "     -microburstinterval   microburst interval [ms] for packet pacing (default 2ms)" << endl;
    cerr << "     -hysteresis              inhibit updated target rate to encoder if the rate change is small" << endl;
    cerr << "                               a value of 0.1 means a hysteresis of +10%/-2.5%" << endl;

    //cerr << "     -sierralog          get logs from python script that logs a sierra modem" << endl;
    exit(-1);
  }
  int ix = 1;
  char *logFile = 0;
  /* First find options */
  while (ix < argc) {
      if (!strstr(argv[ix],"-")) {
          cerr << "wrong arg " << argv[ix] << " index " <<  ix  << endl;
        exit(0);
      }
    if (strstr(argv[ix],"-ect")) {
      ect = atoi(argv[ix+1]);
      ix+=2;
      if (!(ect == -1 || ect == 0 || ect == 1 || ect == 3)) {
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
    if (strstr(argv[ix],"-maxRtpQueueDelay")) {
      maxRtpQueueDelayArg = atof(argv[ix+1]);
        ix+=2;
			continue;
    }


    if (strstr(argv[ix],"-paceheadroom")) {
      packetPacingHeadroom = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix], "-txqueuesizefactor")) {
        txQueueSizeFactor = atoi(argv[ix + 1]);
        ix += 2;
        continue;
    }
    if (strstr(argv[ix], "-queuedelayguard")) {
        queueDelayGuard = atoi(argv[ix + 1]);
        ix += 2;
        continue;
    }

    if (strstr(argv[ix],"-nopace")) {
      disablePacing = true;
      ix++;
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
    if (strstr(argv[ix],"-priority")) {
      priority = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-ratescale")) {
      rateScale = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-ratemultiply")) {
      rateMultiply = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix],"-verbose")) {
      verbose = atoi(argv[ix+1]);
      ix+=2;
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
    if (strstr(argv[ix],"-clockdrift")) {
      enableClockDriftCompensation = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-newcc")) {
      newCc = true;
      ix++;
			continue;
    }
   if (strstr(argv[ix],"-forceidr")) {
      forceidr = true;
      ix++;
			continue;
    }
    if (strstr(argv[ix],"-microburstinterval")) {
        minPaceInterval = 0.001*(atof(argv[ix+1]));
        minPaceIntervalUs = (int) (minPaceInterval*1e6f);
        ix+=2;
        if (minPaceInterval < 0.002f || minPaceInterval > 0.020f) {
            cerr << "microburstinterval must be in range 2..20ms" << endl;
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

    cerr << "unexpected arg " << argv[ix] << endl;
    exit(0);
  }

  if (minRate > initRate)
    initRate = minRate;

  if (screamTx == NULL) {
            screamTx = new ScreamV1Tx(scaleFactor, scaleFactor,
                              delayTarget,
                              false,
                              1.0f,dscale,
                              (initRate*100)/8,
                              packetPacingHeadroom,
                              20,
                              ect==1,
                              false,
                              enableClockDriftCompensation,
			      1.0,
			      newCc);
      screamTx->setCwndMinLow(5000);
      if (logFile) {
          if (append)
              fp_log = fopen(logFile,"a");
          else
              fp_log = fopen(logFile,"w");
      }

      if (logFile && !append && itemlist) {
          fprintf(fp_log,"%s\n", screamTx->getDetailedLogItemList());
      }
      screamTx->setDetailedLogFp(fp_log);
      screamTx->useExtraDetailedLog(detailed);

      pthread_mutex_init(&lock_scream, NULL);
      pthread_mutex_init(&stream->lock_rtp_queue, NULL);
  }
    stream->rtpQueue = new RtpQueue();

  screamTx->registerNewStream(stream->rtpQueue,
                              ssrc,
                              priority,
                              minRate * 1000,
                              initRate * 1000,
                              maxRate * 1000,
                              rateIncrease * 1000,
                              rateScale,
                              maxRtpQueueDelayArg,
                              txQueueSizeFactor,
                              queueDelayGuard,
                              scaleFactor,
                              scaleFactor,
                              false,
                              hysteresis);

  cerr << "Scream sender started! ssrc "<< ssrc <<endl;
  if (stats_print_thread == 0) {
      pthread_create(&stats_print_thread,NULL,statsPrintThread, NULL);
  }
  return (0);
}

void packet_free(void *buf, uint32_t ssrc)
{
    stream_t *stream = getStream(ssrc);
    stream->cb(stream->cb_data, (uint8_t *)buf, 0);
}
int nn=0;

int parseCommandLine(
    char     *buffer,     /* In/Out : Modifiable String Buffer To Tokenise */
    int      *argc,       /* Out    : Argument Count */
    char     *argv[],     /* Out    : Argument String Vector Array */
    const int argv_length /* In     : Maximum Count For `*argv[] */
                     )
{
/* Tokenise string buffer into argc and argv format (req: string.h) */
    int i = 0;
    argv[0] = NULL;
    for (i = 1 ; i < argv_length ; i++)
    { /* Fill argv via strtok_r() */
        if ( NULL == (argv[i] = strtok_r( NULL, " ", &buffer)) ) {
            break;
        }
        // printf("%s %d, %d %s \n", __FUNCTION__, __LINE__, i, argv[i]);
    }
    printf("%s %d, %d \n", __FUNCTION__, __LINE__, i);
    *argc = i;
    return i;
}

#ifdef __cplusplus
extern "C" {
#endif

void
ScreamSenderPluginInit (uint32_t ssrc, const char *arg_string, uint8_t *cb_data_arg,
                        ScreamSenderPushCallBack callback)
{
    printf("%s %d %s ssrc %u\n",  __FUNCTION__, __LINE__, arg_string, ssrc);
    stream_t *stream = getStream(ssrc);
    if (stream != NULL) {
        printf("%s %u ssrc duplicate %u\n", __FUNCTION__, __LINE__, ssrc);
        return;
    }
    stream = addStream(ssrc);
    if (stream == NULL) {
        printf("%s %u can't create ssrc %u\n", __FUNCTION__, __LINE__, ssrc);
        exit(0);
    }

    stream->cb = callback;
    stream->cb_data = cb_data_arg;
    char *s = strdup(arg_string);
    int n_argc;
    char *n_argv[25];
    parseCommandLine(s, &n_argc, n_argv, 24);
    tx_plugin_main(n_argc, n_argv, ssrc);
}

void
ScreamSenderGlobalPluginInit (uint32_t ssrc, const char *arg_string, uint8_t *cb_data_arg,  ScreamSenderPushCallBack callback)
{
    printf("%s %u \n", __FUNCTION__, __LINE__);
    ScreamSenderPluginInit(ssrc, arg_string, cb_data_arg,  callback);
}

void ScreamSenderPush (uint8_t *buf_rtp, uint32_t recvlen, uint16_t seq,
                       uint8_t payload_type, uint32_t timestamp, uint32_t ssrc, uint8_t marker)
{
    bool rc;
    stream_t *stream = getStream(ssrc);
    static bool first = false;
    uint32_t ntp = getTimeInNtp();
    if (!first) {
        pthread_create(&transmit_rtp_thread,NULL,transmitRtpThread,(void*)"Transmit RTP thread...");
        first = true;
        first_ntp = ntp;
        printf("%s %u time %u  first %u\n", __FUNCTION__, __LINE__, ntp, seq);
    }
    pthread_mutex_lock(&stream->lock_rtp_queue);
    rc = stream->rtpQueue->push(buf_rtp, recvlen, ssrc, seq, (marker != 0), (ntp)/65536.0f);
    pthread_mutex_unlock(&stream->lock_rtp_queue);
    if (!rc) {
        stream->rtpqueue_full++;
        stream->cb(stream->cb_data, (uint8_t *)buf_rtp, 0);
        cerr << log_tag << " RtpQueue is full " << endl;
    }
    if (rc) {
        pthread_mutex_lock(&lock_scream);
        screamTx->newMediaFrame(ntp, ssrc, recvlen, (marker != 0));
        pthread_mutex_unlock(&lock_scream);
    }
}

uint8_t ScreamSenderRtcpPush(uint8_t*buf_rtcp, uint32_t recvlen) {
    if (recvlen <= KEEP_ALIVE_PKT_SIZE) {
        return 0;
    }
    uint8_t pt = buf_rtcp[1];
    uint8_t fmt = buf_rtcp[0];
    if ((pt != 205) || (fmt != 0x80)) {
        return 0;
    }
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
    pthread_mutex_lock(&lock_scream);
    screamTx->setTimeString(s);

    screamTx->incomingStandardizedFeedback(time_ntp, buf_rtcp, recvlen);

    pthread_mutex_unlock(&lock_scream);
    rtcp_rx_time_ntp = time_ntp;
    return (1);
}

void ScreamSenderGetTargetRate (uint32_t ssrc, uint32_t *rate_p, uint32_t *force_idr_p) {
    int n = 0;
    *rate_p = 0;
    *force_idr_p = 0;
    stream_t *stream = NULL;
    /*
     * Poll rate change for all media sources
     */
    float rate = screamTx->getTargetBitrate(ssrc);
    if (rate <= 0) {
        // printf("rate 0 %s %d ssrc %u \n", __FUNCTION__, __LINE__, ssrc);
        return;
    }
    stream = getStream(ssrc);
    //rate = 1e6*(5+15*(nn/10 % 2));
    if (0) {
        uint32_t rate_32 = (uint32_t)(rate);
        cerr << " videoControlThread: rate " << rate_32 << endl;
    }
    *rate_p = (uint32_t)(rate * rateMultiply);
    if (*rate_p)  {
        stream_t *stream = getStream(ssrc);
        stream->encoder_rate = *rate_p;
    }
    // cerr << " rate " << *rate_p << endl;
    if (forceidr) {
        /*
         * Force-IDR in case of loss
         */
        struct timeval tp;
        gettimeofday(&tp, NULL);
        double time = tp.tv_sec + tp.tv_usec*1e-6-t0;

        if (screamTx->isLossEpoch(ssrc)) {
            stream->lastLossEpochT = time;
        }
        if (stream->lastLossEpochT > 0.0 && time-stream->lastLossEpochT > 0.1) {
            /*
             * Wait till 100ms after the last loss until force
             * new IDR
             */
            *force_idr_p = 1;
            stream->lastLossEpochT = -1;
        } else {
        }
    }
}

void ScreamSenderStats(char     *s,
                       uint32_t *len, uint32_t ssrc, uint8_t clear)
{
    char buffer[50];
    uint32_t time_ntp = getTimeInNtp();
    float time_s = time_ntp/65536.0f;
    stream_t *stream = getStream(ssrc);
    if (!stream) {
        *len = 0;
        return;
    }
    screamTx->getLog(time_s, s, ssrc, clear != 0);
    snprintf(buffer, 50, ",%lu", stream->rtpqueue_full);
    strcat(s, buffer);
    *len = strlen(s);
    if (clear) {
        stream->rtpqueue_full = 0;
    }
}
void ScreamSenderStatsHeader(char     *s,
                             uint32_t *len)
{
    const char *extra = ",rtpqueue_full";
    screamTx->getLogHeader(s);
    strcat(s, extra);
    *len = strlen(s);
}
#ifdef __cplusplus
}
#endif
