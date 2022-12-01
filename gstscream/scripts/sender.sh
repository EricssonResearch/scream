SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
SCREAM_TARGET_DIR=$SCRIPT_DIR/../target/debug/
export GST_PLUGIN_PATH=$SCREAM_TARGET_DIR:$GST_PLUGIN_PATH
export LD_LIBRARY_PATH=$SCREAMLIB_DIR:$LD_LIBRARY_PATH

DST_IP=127.0.0.1
LOCAL_IP=127.0.0.2

PORT0_RTP=30112
PORT0_RTCP=30113

INIT_ENC_BITRATE=5000

USE_SCREAM=1
if (($USE_SCREAM == 1)); then
    INIT_ENC_BITRATE=500
    #NOSUMMARY=" -nosummary"
#
    SCREAMTX0="queue ! screamtx name=\"screamtx0\" params=\"$NOSUMMARY -forceidr -ect 1 -initrate 500 -minrate 200 -maxrate 8000\" ! queue !"
    SCREAMTX0_RTCP="screamtx0.rtcp_sink screamtx0.rtcp_src !"
else
    SCREAMTX0=""
    SCREAMTX0_RTCP=""
fi

ENCODER="x264enc threads=4 speed-preset=ultrafast tune=fastdecode+zerolatency bitrate=$INIT_ENC_BITRATE"
#ENCODER="nvh264enc zerolatency=true preset=low-latency-hq rc-mode=cbr-ld-hq gop-size=-1 bitrate=$INIT_ENC_BITRATE"

VIDEOSRC="videotestsrc is-live=true pattern=snow ! video/x-raw,format=I420,width=1280,height=720,framerate=50/1"

export SENDPIPELINE="rtpbin name=r \
$VIDEOSRC ! $ENCODER name=encoder0 ! rtph264pay config-interval=-1 ! $SCREAMTX0 r.send_rtp_sink_0 r.send_rtp_src_0 ! udpsink host=$DST_IP port=$PORT0_RTP sync=false \
   udpsrc port=$PORT0_RTCP address=$LOCAL_IP ! queue ! $SCREAMTX0_RTCP r.recv_rtcp_sink_0 \
   r.send_rtcp_src_0 ! udpsink host=$DST_IP port=$PORT0_RTCP sync=false async=false \
"

export GST_DEBUG="2,screamtx:2"
killall -9 scream_sender
$SCREAM_TARGET_DIR/scream_sender --verbose
#$SCREAM_TARGET_DIR/scream_sender
