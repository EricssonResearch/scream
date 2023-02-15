SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/ecn_env.sh
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
SCREAM_TARGET_DIR=$SCRIPT_DIR/../target/debug/
export GST_PLUGIN_PATH=$SCREAM_TARGET_DIR:$GST_PLUGIN_PATH
export LD_LIBRARY_PATH=$SCREAMLIB_DIR:$LD_LIBRARY_PATH

DST_IP=127.0.0.1
LOCAL_IP=127.0.0.2

PORT0_RTP=30112
PORT0_RTCP=30113

PORT1_RTP=30114
PORT1_RTCP=30115

PORT2_RTP=30116
PORT2_RTCP=30117

INIT_ENC_BITRATE=5000

#needed for lo and other low latency links
NETSIM="netsim min-delay=10 !"

USE_SCREAM=1
if (($USE_SCREAM == 1)); then
    INIT_ENC_BITRATE=500
    #NOSUMMARY=" -nosummary"
#
    SCREAMTX0="queue ! screamtx name=\"screamtx0\" params=\"$NOSUMMARY -forceidr $SCREAMTX_PARAM_ECT -initrate 500 -minrate 200 -maxrate 8000\" ! queue !"
    SCREAMTX0_RTCP="screamtx0.rtcp_sink screamtx0.rtcp_src !"
#
    SCREAMTX1="queue ! screamtx name=\"screamtx1\" params=\"$NOSUMMARY -forceidr $SCREAMTX_PARAM_ECT -initrate 500 -minrate 200 -maxrate 15000\" ! queue !"
    SCREAMTX1_RTCP="screamtx1.rtcp_sink screamtx1.rtcp_src !"
#
    SCREAMTX2="queue ! screamtx name=\"screamtx2\" params=\"$NOSUMMARY -forceidr $SCREAMTX_PARAM_ECT -initrate 500 -minrate 200 -maxrate 1000\" ! queue !"
    SCREAMTX2_RTCP="screamtx2.rtcp_sink screamtx2.rtcp_src !"
else
    SCREAMTX0=""
    SCREAMTX0_RTCP=""
    SCREAMTX1=""
    SCREAMTX1_RTCP=""
    SCREAMTX2=""
    SCREAMTX2_RTCP=""
fi

ENCODER="x264enc threads=4 speed-preset=ultrafast tune=fastdecode+zerolatency bitrate=$INIT_ENC_BITRATE"
#ENCODER="nvh264enc zerolatency=true preset=low-latency-hq rc-mode=cbr-ld-hq gop-size=-1 bitrate=$INIT_ENC_BITRATE"

VIDEOSRC="videotestsrc is-live=true pattern=snow ! video/x-raw,format=I420,width=1280,height=720,framerate=50/1"

export SENDPIPELINE="rtpbin name=r \
$VIDEOSRC ! $ENCODER name=encoder0 ! rtph264pay config-interval=-1 ! $SCREAMTX0 r.send_rtp_sink_0 r.send_rtp_src_0 ! $NETSIM udpsink host=$DST_IP port=$PORT0_RTP sync=false $SET_ECN \
   udpsrc port=$PORT0_RTCP address=$LOCAL_IP ! queue ! $SCREAMTX0_RTCP r.recv_rtcp_sink_0 \
   r.send_rtcp_src_0 ! udpsink host=$DST_IP port=$PORT0_RTCP sync=false async=false \
\
$VIDEOSRC ! $ENCODER name=encoder1 ! rtph264pay config-interval=-1 ! $SCREAMTX1 r.send_rtp_sink_1 r.send_rtp_src_1 ! $NETSIM udpsink host=$DST_IP port=$PORT1_RTP sync=false $SET_ECN \
   udpsrc port=$PORT1_RTCP address=$LOCAL_IP ! queue ! $SCREAMTX1_RTCP r.recv_rtcp_sink_1 \
   r.send_rtcp_src_1 ! udpsink host=$DST_IP port=$PORT1_RTCP sync=false async=false \
\
$VIDEOSRC ! $ENCODER name=encoder2 ! rtph264pay config-interval=-1 ! $SCREAMTX2 r.send_rtp_sink_2 r.send_rtp_src_2 ! $NETSIM udpsink host=$DST_IP port=$PORT2_RTP sync=false $SET_ECN \
  udpsrc port=$PORT2_RTCP address=$LOCAL_IP ! queue ! $SCREAMTX2_RTCP r.recv_rtcp_sink_2 \
  r.send_rtcp_src_1 ! udpsink host=$DST_IP port=$PORT2_RTCP sync=false async=false \
"

export GST_DEBUG="2,screamtx:2"
killall -9 scream_sender
$SCREAM_TARGET_DIR/scream_sender --verbose
#$SCREAM_TARGET_DIR/scream_sender
