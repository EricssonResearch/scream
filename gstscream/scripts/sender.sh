#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
export SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/env.sh

INIT_ENC_BITRATE=5000

if (($USE_SCREAM == 1)); then
    INIT_ENC_BITRATE=500
    #NOSUMMARY=" -nosummary"
#
    
    SCREAMTX0="queue ! screamtx name=\"screamtx0\" params=\"$NOSUMMARY -forceidr $SCREAMTX_PARAM_ECT -initrate $INIT_ENC_BITRATE  -minrate 200 -maxrate 8000\" ! queue !"
    SCREAMTX0_RTCP="screamtx0.rtcp_sink screamtx0.rtcp_src !"
else
    SCREAMTX0=""
    SCREAMTX0_RTCP=""
fi

# VIDEOSRC="videotestsrc is-live=true pattern=snow ! video/x-raw,format=I420,width=1280,height=720,framerate=50/1"
VIDEOSRC="v4l2src device=/dev/video4 ! image/jpeg, width=1280, height=720, framerate=30/1 ! jpegdec"

export SENDPIPELINE="rtpbin name=r \
$VIDEOSRC ! $ENCODER name=encoder0 bitrate=$INIT_ENC_BITRATE ! rtph${ENC_ID}pay config-interval=-1 ! $SCREAMTX0 r.send_rtp_sink_0 r.send_rtp_src_0 ! udpsink host=$RECEIVER_IP port=$PORT0_RTP sync=false $SET_ECN \
   udpsrc port=$PORT0_RTCP address=$SENDER_IP ! queue ! $SCREAMTX0_RTCP r.recv_rtcp_sink_0 \
   r.send_rtcp_src_0 ! udpsink host=$RECEIVER_IP port=$PORT0_RTCP sync=false async=false \
"
export GST_DEBUG="2,screamtx:2"
killall -9 scream_sender
$SCREAM_TARGET_DIR/scream_sender --verbose 
