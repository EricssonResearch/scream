#include "ScreamTx.h"
#include "RtpQueue.h"
#include "sys/types.h"
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
static struct itimerval timer;

typedef void (*ScreamTxBwPluginPushCallBack)(uint8_t *cb_data, uint32_t recvlen, uint16_t seqNr, uint32_t ts, uint8_t pt, uint8_t isMark, uint32_t ssrc);
static ScreamTxBwPluginPushCallBack cb;
static uint8_t *cb_data;

struct periodicInfo
{
	int timer_fd;
	unsigned long long wakeupsMissed;
};

using namespace std;

static bool stopThread = false;
static pthread_t create_rtp_thread = 0;
static bool isKeyFrame = false;
static float keyFrameSize = 1.0;
static float keyFrameInterval = 0.0;
static float FPS = 50.0f; // Frames per second
static float randRate = 0.0f;
static float burstTime = -1.0;
static float burstSleep = -1.0;
static bool isBurst = false;
static uint16_t seqNr = 0;
static uint32_t lastKeyFrameT_ntp = 0;
static int mtu = 1200;
static float runTime = -1.0;
static int periodicRateDropInterval = 600; // seconds*10
static float burstStartTime = -1.0;
static float burstSleepTime = -1.0;
static float rateScale = 0.5f;
static uint32_t SSRC = 100;
static uint64_t numframes = UINT64_MAX;
static double t0=0;

static void waitPeriod(struct periodicInfo *info)
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

extern uint32_t getTimeInNtp(void);

static float TargetBitrate;
static bool ForceKeyUnit = false;
static float getTargetBitrate(void)
{
    return TargetBitrate;
}
void *createRtpThread(void *arg) {
	uint32_t keyFrameInterval_ntp = (uint32_t)(keyFrameInterval * 65536.0f);
	float rateScale = 1.0f;
	if (isKeyFrame) {
		rateScale = 1.0f + 0.0*keyFrameSize / (FPS*keyFrameInterval) / 2.0;
		rateScale = (1.0f / rateScale);
	}
	uint32_t dT_us = (uint32_t)(1e6 / FPS);
	unsigned char PT = 98;
	struct periodicInfo info;

	/*
	* Infinite loop that generates RTP packets
	*/
	for (;;) {
		if (stopThread) {
			return NULL;
		}
		uint32_t time_ntp = getTimeInNtp();

		uint32_t ts = (uint32_t)(time_ntp / 65536.0 * 90000);
		float randVal = float(rand()) / RAND_MAX - 0.5;
        float rateTx = getTargetBitrate()*rateScale;
		int bytes = (int)(rateTx / FPS / 8 * (1.0 + randVal * randRate));

		if (isKeyFrame && time_ntp - lastKeyFrameT_ntp >= keyFrameInterval_ntp) {
			/*
			* Fake a key frame
			*/
			bytes = (int)(bytes*keyFrameSize);
			lastKeyFrameT_ntp = time_ntp;
		}

		if (burstTime < 0) {
			int tmp = time_ntp / 6554;
			int tmp_mod = tmp % periodicRateDropInterval;
			if (tmp_mod > (periodicRateDropInterval - 2)) {
				/*
				* Drop bitrate for 100ms every periodicRateDropInterval/10
				*  this ensures that the queue delay is estimated correctly
				* A normal video encoder use does not necessitate this as the
				*  video stream typically drops in bitrate every once in a while
				*/
				bytes = 10;
			}
			if (tmp > 2 && (tmp_mod > (periodicRateDropInterval - 2) || tmp_mod <= 2)) {
				/*
				* Make sure that the rate drop does not impact on the rate control
				*  this extra code locks the rate update to avoid reaction to false
				*  rate estimates
				*/
				// screamTx->setEnableRateUpdate(false);
			}
			else {
				// screamTx->setEnableRateUpdate(true);
			}
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
				// pt |= 0x80;
                isMark = true;
                numframes--;
			} else {
                isMark = false;
            }
            cb(cb_data, recvlen, seqNr, ts, pt, isMark, SSRC);
			seqNr++;
		}
        if (numframes <= 1) {
            printf("EOS\n");
            exit(0);
        }
		waitPeriod(&info);
	}

	return NULL;
}

static uint32_t lastT_ntp;

int txbw_plugin_main(int argc, char* argv[])
{
  struct timeval tp;
  gettimeofday(&tp, NULL);
  t0 = tp.tv_sec + tp.tv_usec*1e-6 - 1e-3;
  lastT_ntp = getTimeInNtp();

  /*
  * Parse command line
  */
  if (argc <= 1) {
    cerr << "SCReAM sender. Ericsson AB. Version 2024-07-03" << endl;
    cerr << "Usage : " << endl << " > scream_bw_test_tx <options>  " << endl;
    cerr << "     -initrate value       set a start bitrate [kbps]" << endl;
    cerr << "                            example -initrate 2000 " << endl;
    cerr << "     -ratescale value      set a max allowed rate increase speed as a fraction of the " << endl;
    cerr << "                            current rate, default 0.5" << endl;
    cerr << "                            example -ratescale 1.0 " << endl;
    cerr << "     -ssrc value          set ssrc" << endl;
    cerr << "     -numframes value     exit after sending numframes frames" << endl;
    cerr << "     -periodicdropinterval interval [s] between periodic drops in rate (default 60s)" << endl;
    exit(-1);
  }
  int ix = 1;
  /* First find options */
  while (ix < argc) {
           if (!strstr(argv[ix],"-")) {
               cerr << "wrong arg " << argv[ix] << " index " <<  ix  << endl;
               exit(0);
           }
    		if (strstr(argv[ix], "-time")) {
			runTime = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}

		if (strstr(argv[ix], "-mtu")) {
			mtu = atoi(argv[ix + 1]);
			ix += 2;
			continue;
		}
        if (strstr(argv[ix], "-numframes")) {
			numframes = atoll(argv[ix + 1]);
			ix += 2;
			continue;
		}


        if (strstr(argv[ix], "-initrate")) {
            TargetBitrate = (atoi(argv[ix + 1])) * 1000.0f;
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
		if (strstr(argv[ix], "-ratescale")) {
			rateScale = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}
		if (strstr(argv[ix], "-ssrc")) {
			SSRC = atof(argv[ix + 1]);
			ix += 2;
			continue;
		}

		if (strstr(argv[ix], "-periodicdropinterval")) {
			periodicRateDropInterval = (int)(atof(argv[ix + 1])*10.0f);
			ix += 2;
			continue;
		}
        cerr << "unexpected arg " << argv[ix] << endl;
        exit(0);
  }

  {
      cerr << "Scream sender started! "<<endl;

	/* Create RTP thread */
	pthread_create(&create_rtp_thread, NULL, createRtpThread, (void*)"Create RTP thread...");
    while (!stopThread && (runTime < 0 || getTimeInNtp() < runTime*65536.0f)) {
        usleep(500000);
    }
    stopThread = true;
	usleep(500000);
}
  exit (0);
}

extern int parseCommandLine(
    char     *buffer,     /* In/Out : Modifiable String Buffer To Tokenise */
    int      *argc,       /* Out    : Argument Count */
    char     *argv[],     /* Out    : Argument String Vector Array */
    const int argv_length /* In     : Maximum Count For `*argv[] */
                            );
void *
ScreamTxBwPluginInitThread(void *arg)
{
    char *s = (char *)arg;
    int n_argc;
    char *n_argv[21];
    parseCommandLine(s, &n_argc, n_argv, 20);
    txbw_plugin_main(n_argc, n_argv);
    return (NULL);
}

#ifdef __cplusplus
extern "C" {
#endif

void ScreamTxBwPluginSetTargetRate(uint32_t rate)
{
    TargetBitrate = rate * 1000.0f;
}
void ScreamTxBwPluginSetForceKeyUnit(void)
{
    // TBD not used yet
    ForceKeyUnit = true;
}

void
ScreamTxBwPluginInit (const char *arg_string, uint8_t *cb_data_arg,  ScreamTxBwPluginPushCallBack callback)
{
    pthread_t scream_sender_plugin_init_thread;
    printf("%s %s\n",  __FUNCTION__, arg_string);
    cb = callback;
    cb_data = cb_data_arg;
    char *s = strdup(arg_string);
    pthread_create(&scream_sender_plugin_init_thread, NULL, ScreamTxBwPluginInitThread, s);
}

#ifdef __cplusplus
}
#endif
