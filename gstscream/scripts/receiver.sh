#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/env.sh

QUEUE="queue max-size-buffers=2 max-size-bytes=0 max-size-time=0"

if (($USE_SCREAM == 1)); then
    SCREAMRX0="! screamrx name=screamrx0 screamrx0.src "
    SCREAMRX0_RTCP="screamrx0.rtcp_src !  f0."
else
    SCREAMRX0=""
    SCREAMRX0_RTCP=""
fi

VIDEOSINK="videoconvert ! fpsdisplaysink video-sink=\"ximagesink\""
#VIDEOSINK="glupload ! glcolorconvert ! fpsdisplaysink video-sink=\"glimagesinkelement\""
#VIDEOSINK="fakesink"

export RECVPIPELINE="rtpbin latency=10 name=r \
udpsrc port=$PORT0_RTP address=$RECEIVER_IP $RETRIEVE_ECN ! \
 queue $SCREAMRX0 ! application/x-rtp, media=video, encoding-name=H${ENC_ID}, clock-rate=90000 ! r.recv_rtp_sink_0 r. ! rtph${ENC_ID}depay ! h${ENC_ID}parse ! $DECODER name=videodecoder0 ! $QUEUE ! $VIDEOSINK \
 r.send_rtcp_src_0 ! funnel name=f0 ! queue ! udpsink host=$SENDER_IP port=$PORT0_RTCP sync=false async=false \
 $SCREAMRX0_RTCP udpsrc port=$PORT0_RTCP ! r.recv_rtcp_sink_0 \
"

export GST_DEBUG="2,screamrx:2"
killall -9 scream_receiver
$SCREAM_TARGET_DIR/scream_receiver
