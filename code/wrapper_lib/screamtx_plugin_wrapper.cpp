// Scream sender side wrapper
#include "ScreamTx.h"
#include "RtpQueue.h"
#include "sys/types.h"
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>

const char* log_tag = "scream_lib";
typedef void (* ScreamSenderPushCallBack)(uint8_t *, uint8_t *, uint8_t);

typedef struct  stream_s {
    uint32_t ssrc;
    RtpQueue *rtpQueue;
    pthread_mutex_t lock_rtp_queue;
    ScreamSenderPushCallBack cb;
    uint8_t *cb_data;
    uint64_t rtpqueue_full;
    uint64_t force_idr;
    uint64_t rate_updates;
    uint32_t prev_encoder_rate;
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
float rateMultiply = 1.0;
bool enableClockDriftCompensation = false;
float priority = 1.0;
bool openWindow = false;
const char *scream_version = "V2";
float packetPacingHeadroom = 1.5f;
float scaleFactor = 0.7f;
ScreamV2Tx *screamTx = 0;
float bytesInFlightHeadroom = 2.0f;
float multiplicativeIncreaseFactor = 0.05f;
bool isEmulateCubic = false;

float hysteresis = 0.0f;
float postCongestionDelay = 4.0f;
float adaptivePaceHeadroom = 1.5f;

uint32_t lastKeyFrameT_ntp = 0;
float runTime = -1.0;
bool stopThread = false;
pthread_t transmit_rtp_thread = 0;
bool sierraLog;

pthread_t stats_print_thread = 0;

float delayTarget = 0.06f;
float maxRtpQueueDelayArg = 0.2f;
bool forceidr = false;
bool printSummary = true;

static uint32_t first_ntp = 0;
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
  uint32_t ts;
  bool isMark = false;
  void *buf;
  uint32_t time_ntp = getTimeInNtp();
  int sleepTime_us = 10;
  float retVal = 0.0f;
  int sizeOfQueue;
  uint32_t ssrc = 0;
  struct timeval start, end;
  useconds_t diff = 0;
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
           float rtpQueueDelay = 0.0f;
           rtpQueueDelay = stream->rtpQueue->getDelay((time_ntp) / 65536.0f);
           stream->rtpQueue->pop(&buf, size, ssrc, seqNr, isMark,ts);
           pthread_mutex_unlock(&stream->lock_rtp_queue);
           if (buf) {
               stream->cb(stream->cb_data, (uint8_t *)buf, 1);
           }
           if ((cur_n_streams > 1) && (sleeps++ < 120)) {
               usleep(500);
           }
           time_ntp = getTimeInNtp();
           pthread_mutex_lock(&lock_scream);
           retVal = screamTx->addTransmitted(time_ntp, ssrc, size, seqNr, isMark, rtpQueueDelay, ts);
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
    std::cout << " encoder_rate " ;
 	for (int n = 0; n < kMaxStreams; n++) {
 		if (streams[n].ssrc != 0) {
            std::cout  << " " << (streams[n].encoder_rate /1000) << " " ;
 		}
 	}
    std::cout  << "kbbps ";
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
                std::cerr << "No RTCP feedback received" << std::endl;
            } else {
                float time_s = time_ntp/65536.0f;
                char s[1000];
                screamTx->getStatistics(time_s, s);
                if (sierraLog)
                    std::cout << s << std::endl << "      CellId, RSRP, RSSI, SINR: {" << sierraLogString << "}" << std::endl << std::endl;
                else
                    std::cout << s ;
                EncoderRates();
                std::cout << std::endl;

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
                    std::cout << s1 << std::endl;
                    screamTx->getLog(time_s, s1, 0, false);
                    sprintf(s,"%8.3f,%s ", time_s, s1);
                    std::cout << s << " ";
                    EncoderRates();
                    std::cout << std::endl;
                    screamTx->getShortLog(time_s, s1);
                }
                sprintf(s,"%8.3f, %s ", time_s, s1);
                std::cout << s;
                EncoderRates();
                std::cout << std::endl;
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
    std::cerr << "SCReAM V2 BW test tool, sender. Ericsson AB. Version 2024-07-03 " << std::endl;
    std::cerr << "Usage : " << std::endl << " > scream_bw_test_tx <options> decoder_ip decoder_port " << std::endl;
    std::cerr << "     -if name                 bind to specific interface" << std::endl;
    std::cerr << "     -time value              run for time seconds (default infinite)" << std::endl;
    std::cerr << "     -burst val1 val2         burst media for a given time and then sleeps a given time" << std::endl;
    std::cerr << "         example -burst 1.0 0.2 burst media for 1s then sleeps for 0.2s " << std::endl;
    std::cerr << "     -nopace                  disable packet pacing" << std::endl;
    std::cerr << "     -fixedrate value         set a fixed 'coder' bitrate " << std::endl;
    std::cerr << "     -pushtraffic             just pushtraffic at a fixed bitrate, no feedback needed" << std::endl;
    std::cerr << "                                must be used with -fixedrate option" << std::endl;
    std::cerr << "     -key val1 val2           set a given key frame interval [s] and size multiplier " << std::endl;
    std::cerr << "                               example -key 2.0 5.0 " << std::endl;
    std::cerr << "     -rand value              framesizes vary randomly around the nominal " << std::endl;
    std::cerr << "                               example -rand 10 framesize vary +/- 10% " << std::endl;
    std::cerr << "     -initrate value          set a start bitrate [kbps]" << std::endl;
    std::cerr << "                               example -initrate 2000 " << std::endl;
    std::cerr << "     -minrate  value          set a min bitrate [kbps], default 1000kbps" << std::endl;
    std::cerr << "                               example -minrate 1000 " << std::endl;
    std::cerr << "     -maxrate value           set a max bitrate [kbps], default 200000kbps" << std::endl;
    std::cerr << "                               example -maxrate 10000 " << std::endl;
    std::cerr << "     -ect n                   ECN capable transport, n = 0 or 1 for ECT(0) or ECT(1)," << std::endl;
    std::cerr << "                               -1 for not-ECT (default)" << std::endl;
    std::cerr << "     -scale value             scale factor in case of loss or ECN event (default 0.9) " << std::endl;
    std::cerr << "     -delaytarget value       set a queue delay target (default = 0.06s) " << std::endl;
    std::cerr << "     -paceheadroom value      set a packet pacing headroom (default = 1.5) " << std::endl;
    std::cerr << "     -adaptivepaceheadroom value set adaptive packet pacing headroom (default = 1.5) " << std::endl;
    std::cerr << "     -inflightheadroom value  set a bytes in flight headroom (default = 2.0) " << std::endl;
    std::cerr << "     -mulincrease val         multiplicative increase factor for (default 0.05)" << std::endl;
    std::cerr << "     -postcongestiondelay val post congestion delay (default 4.0s)" << std::endl;
    std::cerr << "     -mtu value               set the max RTP payload size (default 1200 byte)" << std::endl;
    std::cerr << "     -fps value               set the frame rate (default 50)" << std::endl;
    std::cerr << "     -clockdrift              enable clock drift compensation for the case that the" << std::endl;
    std::cerr << "                               receiver end clock is faster" << std::endl;
    std::cerr << "     -emulatecubic            make adaptation more cautious around the last known higher rate" << std::endl;
    std::cerr << "     -verbose                 print a more extensive log" << std::endl;
    std::cerr << "     -nosummary               don't print summary" << std::endl;
    std::cerr << "     -log logfile             save detailed per-ACK log to file" << std::endl;
    std::cerr << "     -ntp                     use NTP timestamp in logfile" << std::endl;
    std::cerr << "     -append                  append logfile" << std::endl;
    std::cerr << "     -itemlist                add item list in beginning of log file" << std::endl;
    std::cerr << "     -detailed                detailed log, per ACKed RTP" << std::endl;
    std::cerr << "     -periodicdropinterval    interval [s] between periodic drops in rate (default 60s)" << std::endl;
    std::cerr << "     -microburstinterval      microburst interval [ms] for packet pacing (default 1ms)" << std::endl;
    std::cerr << "     -hysteresis              inhibit updated target rate to encoder if the rate change is small" << std::endl;
    std::cerr << "                               a value of 0.1 means a hysteresis of +10%/-2.5%" << std::endl;
    exit(-1);
  }
  int ix = 1;
  char *logFile = 0;
  /* First find options */
  while (ix < argc) {
      if (!strstr(argv[ix],"-")) {
          std::cerr << "wrong arg " << argv[ix] << " index " <<  ix  << std::endl;
        exit(0);
      }
    if (strstr(argv[ix],"-ect")) {
      ect = atoi(argv[ix+1]);
      ix+=2;
      if (!(ect == -1 || ect == 0 || ect == 1 || ect == 3)) {
        std::cerr << "ect must be -1, 0, 1 or 3 " << std::endl;
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
    if (strstr(argv[ix], "-adaptivepaceheadroom")) {
        adaptivePaceHeadroom = atof(argv[ix + 1]);
        ix += 2;
        continue;
    }

    if (strstr(argv[ix], "-postcongestiondelay")) {
        postCongestionDelay = atof(argv[ix + 1]);
        ix += 2;
        continue;
    }

    if (strstr(argv[ix], "-infligtheadroom")) {
        bytesInFlightHeadroom = atof(argv[ix + 1]);
        ix += 2;
        continue;
    }

    if (strstr(argv[ix], "-mulincrease")) {
        multiplicativeIncreaseFactor = atof(argv[ix + 1]);
        ix += 2;
        continue;
    }
    if (strstr(argv[ix],"-priority")) {
      priority = atof(argv[ix+1]);
      ix+=2;
			continue;
    }
    if (strstr(argv[ix], "-openwindow")) {
        openWindow = true;
        ix++;
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
    if (strstr(argv[ix], "-emulatecubic")) {
        isEmulateCubic = true;
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
            std::cerr << "microburstinterval must be in range 2..20ms" << std::endl;
            exit(0);
        }
        continue;
    }
    if (strstr(argv[ix], "-hysteresis")) {
        hysteresis = atof(argv[ix + 1]);
        ix += 2;
        if (hysteresis < 0.0f || hysteresis > 0.2f) {
            std::cerr << "hysteresis must be in range 0.0...0.2" << std::endl;
            exit(0);
        }
        continue;
    }

    std::cerr << "unexpected arg " << argv[ix] << std::endl;
    exit(0);
  }

  if (minRate > initRate)
    initRate = minRate;

  if (screamTx == NULL) {
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
          openWindow,
          false,
          enableClockDriftCompensation);
      screamTx->setIsEmulateCubic(isEmulateCubic);
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
                                0.2f,
                                false,
                                hysteresis);
    std::cerr << "Scream sender " << scream_version << " started! ssrc "<< ssrc << std::endl;
  if (stats_print_thread == 0) {
      pthread_create(&stats_print_thread,NULL,statsPrintThread, NULL);
  }
  return (0);
}

void packet_free(void *buf, uint32_t ssrc)
{
    stream_t *stream = getStream(ssrc);
    if (buf) {
        stream->cb(stream->cb_data, (uint8_t *)buf, 0);
    }
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
ScreamSenderPluginUpdate (uint32_t ssrc, const char *arg_string)
{
    stream_t *stream = getStream(ssrc);
    if (stream == NULL) {
        printf("%s %u can't get scream ssrc %u\n", __FUNCTION__, __LINE__, ssrc);
        return;
    }

    char *s = strdup(arg_string);
    int argc;
    char *argv[25];
    parseCommandLine(s, &argc, argv, 24);
    int ix = 1;
    /* First find options */
    while (ix < argc) {
        if (!strstr(argv[ix],"-")) {
            std::cerr << "wrong arg " << argv[ix] << " index " <<  ix  << std::endl;
            exit(0);
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
        printf("\nunsupported params %s\n", argv[ix]);
        exit (255);
    }
    pthread_mutex_lock(&lock_scream);
    screamTx->updateBitrateStream(ssrc, minRate * 1000, maxRate * 1000);
    pthread_mutex_unlock(&lock_scream);
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
    rc = stream->rtpQueue->push(buf_rtp, recvlen, ssrc, seq, (marker != 0), (ntp)/65536.0f,timestamp);
    pthread_mutex_unlock(&stream->lock_rtp_queue);
    if (!rc) {
        stream->rtpqueue_full++;
        stream->cb(stream->cb_data, (uint8_t *)buf_rtp, 0);
        std::cerr << log_tag << " RtpQueue is full " << std::endl;
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
    *rate_p = 0;
    *force_idr_p = 0;
    stream_t *stream = NULL;
    /*
     * Poll rate change for all media sources
     */
    uint32_t time_ntp = getTimeInNtp();
    float rate = screamTx->getTargetBitrate(time_ntp, ssrc);
    if (rate <= 0) {
        // printf("rate 0 %s %d ssrc %u \n", __FUNCTION__, __LINE__, ssrc);
        return;
    }
    stream = getStream(ssrc);
    //rate = 1e6*(5+15*(nn/10 % 2));
    if (0) {
        uint32_t rate_32 = (uint32_t)(rate);
        std::cerr << " videoControlThread: rate " << rate_32 << std::endl;
    }
    *rate_p = (uint32_t)(rate * rateMultiply);
    if (*rate_p)  {
        stream_t *stream = getStream(ssrc);
        stream->prev_encoder_rate = stream->encoder_rate;
        stream->encoder_rate = *rate_p;
        if (stream->prev_encoder_rate != stream->encoder_rate) {
            stream->rate_updates++;
        }
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
            stream->force_idr++;
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
    snprintf(buffer, 50, ",%lu,%lu,%lu", stream->rtpqueue_full, stream->force_idr, stream->rate_updates);
    strcat(s, buffer);
    *len = strlen(s);
    if (clear) {
        stream->rtpqueue_full = 0;
        stream->force_idr = 0;
        stream->rate_updates = 0;
    }
}
void ScreamSenderStatsHeader(char     *s,
                             uint32_t *len)
{
    const char *extra = ",rtpqueue_full,force_idr,rate_updates";
    screamTx->getLogHeader(s);
    strcat(s, extra);
    *len = strlen(s);
}
#ifdef __cplusplus
}
#endif
