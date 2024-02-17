#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/env.sh

QUEUE="queue max-size-buffers=2 max-size-bytes=0 max-size-time=0"

if (($USE_SCREAM == 1)); then
    SCREAMRX0="! screamrx name=screamrx0 screamrx0.src "
    SCREAMRX0_RTCP="screamrx0.rtcp_src !  f0."
#
    SCREAMRX1="! screamrx name=screamrx1 screamrx1.src "
    SCREAMRX1_RTCP="screamrx1.rtcp_src !  f1."
#
    SCREAMRX2="! screamrx name=screamrx2 screamrx2.src "
    SCREAMRX2_RTCP="screamrx2.rtcp_src !  f2."
else
    SCREAMRX0=""
    SCREAMRX0_RTCP=""
    SCREAMRX1=""
    SCREAMRX1_RTCP=""
    SCREAMRX2=""
    SCREAMRX2_RTCP=""
fi

DECODER=avdec_h${ENC_ID}
#DECODER=nvh${ENC_ID}dec
VIDEOSINK="videoconvert ! fpsdisplaysink video-sink=\"ximagesink\""
#VIDEOSINK="fakesink"

export RECVPIPELINE="rtpbin latency=10 name=r \
udpsrc port=$PORT0_RTP address=$RECEIVER_IP $RETRIEVE_ECN ! \
 queue $SCREAMRX0 ! application/x-rtp, media=video, encoding-name=H${ENC_ID}, clock-rate=90000 ! r.recv_rtp_sink_0 r. ! rtph${ENC_ID}depay ! h${ENC_ID}parse ! $DECODER name=videodecoder0 ! $QUEUE ! $VIDEOSINK \
 r.send_rtcp_src_0 ! funnel name=f0 ! queue ! udpsink host=$SENDER_IP port=$PORT0_RTCP sync=false async=false \
 $SCREAMRX0_RTCP udpsrc port=$PORT0_RTCP ! r.recv_rtcp_sink_0 \
\
udpsrc port=$PORT1_RTP address=$RECEIVER_IP $RETRIEVE_ECN ! \
 queue $SCREAMRX1 ! application/x-rtp, media=video, encoding-name=H${ENC_ID}, clock-rate=90000 ! r.recv_rtp_sink_1 r. ! rtph${ENC_ID}depay ! h${ENC_ID}parse ! $DECODER name=videodecoder1 ! $QUEUE ! $VIDEOSINK \
  r.send_rtcp_src_1 ! funnel name=f1 ! queue ! udpsink host=$SENDER_IP port=$PORT1_RTCP sync=false async=false \
  $SCREAMRX1_RTCP udpsrc port=$PORT1_RTCP ! r.recv_rtcp_sink_1 \
\
udpsrc port=$PORT2_RTP address=$RECEIVER_IP $RETRIEVE_ECN ! \
 queue $SCREAMRX2 ! application/x-rtp, media=video, encoding-name=H${ENC_ID}, clock-rate=90000 ! r.recv_rtp_sink_2 r. ! rtph${ENC_ID}depay ! h${ENC_ID}parse ! $DECODER name=videodecoder2 ! $QUEUE ! $VIDEOSINK \
 r.send_rtcp_src_2 ! funnel name=f2 ! queue ! udpsink host=$SENDER_IP port=$PORT2_RTCP sync=false async=false \
 $SCREAMRX2_RTCP udpsrc port=$PORT2_RTCP ! r.recv_rtcp_sink_2 \
"

export GST_DEBUG="screamrx:2"
killall -9 scream_receiver
$SCREAM_TARGET_DIR/scream_receiver
