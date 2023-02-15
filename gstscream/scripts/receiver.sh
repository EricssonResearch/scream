#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/ecn_env.sh
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
SCREAM_TARGET_DIR=$SCRIPT_DIR/../target/debug/
export GST_PLUGIN_PATH=$SCREAM_TARGET_DIR:$GST_PLUGIN_PATH
export LD_LIBRARY_PATH=$SCREAMLIB_DIR:$LD_LIBRARY_PATH

DST_IP=127.0.0.2
LOCAL_IP=127.0.0.1

PORT0_RTP=30112
PORT0_RTCP=30113

QUEUE="queue max-size-buffers=2 max-size-bytes=0 max-size-time=0"

USE_SCREAM=1
if (($USE_SCREAM == 1)); then
    SCREAMRX0="! screamrx name=screamrx0 screamrx0.src "
    SCREAMRX0_RTCP="screamrx0.rtcp_src !  f0."
else
    SCREAMRX0=""
    SCREAMRX0_RTCP=""
fi

DECODER=avdec_h264
#DECODER=nvh264dec
#
VIDEOSINK="videoconvert ! fpsdisplaysink video-sink=\"ximagesink\""
#VIDEOSINK="glupload ! glcolorconvert ! fpsdisplaysink video-sink=\"glimagesinkelement\""
#VIDEOSINK="fakesink"

export RECVPIPELINE="rtpbin latency=10 name=r \
udpsrc port=$PORT0_RTP address=$LOCAL_IP $RETRIEVE_ECN ! \
 queue $SCREAMRX0 ! application/x-rtp, media=video, encoding-name=H264, clock-rate=90000 ! r.recv_rtp_sink_0 r. ! rtph264depay ! h264parse ! $DECODER name=videodecoder0 ! $QUEUE ! $VIDEOSINK \
 r.send_rtcp_src_0 ! funnel name=f0 ! queue ! udpsink host=$DST_IP port=$PORT0_RTCP sync=false async=false \
 $SCREAMRX0_RTCP udpsrc port=$PORT0_RTCP ! r.recv_rtcp_sink_0 \
"

export GST_DEBUG="screamrx:2"
killall -9 scream_receiver
$SCREAM_TARGET_DIR/scream_receiver
