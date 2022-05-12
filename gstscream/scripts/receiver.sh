SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
SCREAM_TARGET_DIR=$SCRIPT_DIR/../target/debug/
export GST_PLUGIN_PATH=$GST_PLUGIN_PATH:$SCREAM_TARGET_DIR
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$SCREAMLIB_DIR

DST_IP=127.0.0.2
LOCAL_IP=127.0.0.1
PORT=30112

QUEUE="queue max-size-buffers=2 max-size-bytes=0 max-size-time=0"

USE_SCREAM=1
if (($USE_SCREAM == 1)); then
    SCREAMRX="! screamrx name=screamrx screamrx.src "
    SCREAMRX_RTCP=" screamrx.rtcp_src !  f."
else
    SCREAMTX=""
    SCREAMTX_RTCP=""
fi

export RECVPIPELINE="rtpbin latency=10 name=rtpbin udpsrc port=$PORT address=$LOCAL_IP ! $QUEUE $SCREAMRX ! application/x-rtp, media=video, encoding-name=H264, clock-rate=90000 ! rtpbin.recv_rtp_sink_0 rtpbin. ! rtph264depay ! h264parse ! avdec_h264 name=videodecoder ! $QUEUE ! glupload ! glcolorconvert ! fpsdisplaysink video-sink=\"glimagesinkelement\" rtpbin.send_rtcp_src_0 ! funnel name=f ! $QUEUE ! udpsink host=$DST_IP port=$PORT sync=false async=false $SCREAMRX_RTCP "

#export GST_DEBUG="screamrx:5"
killall -9 scream_receiver
$SCREAM_TARGET_DIR/scream_receiver
