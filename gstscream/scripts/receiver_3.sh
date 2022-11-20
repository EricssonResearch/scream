SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
SCREAM_TARGET_DIR=$SCRIPT_DIR/../target/debug/
export GST_PLUGIN_PATH=$SCREAM_TARGET_DIR:$GST_PLUGIN_PATH
export LD_LIBRARY_PATH=$SCREAMLIB_DIR:$LD_LIBRARY_PATH

DST_IP=127.0.0.2
LOCAL_IP=127.0.0.1

PORT0_RTP=30112
PORT0_RTCP=30113

PORT1_RTP=30114
PORT1_RTCP=30115

PORT2_RTP=30116
PORT2_RTCP=30117

QUEUE="queue max-size-buffers=2 max-size-bytes=0 max-size-time=0"

USE_SCREAM=1
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

DECODER=avdec_h264
#DECODER=nvh264dec
VIDEOSINK="videoconvert ! fpsdisplaysink video-sink=\"ximagesink\""
#VIDEOSINK="fakesink"

export RECVPIPELINE="rtpbin latency=10 name=r \
udpsrc port=$PORT0_RTP address=$LOCAL_IP ! \
 queue $SCREAMRX0 ! application/x-rtp, media=video, encoding-name=H264, clock-rate=90000 ! r.recv_rtp_sink_0 r. ! rtph264depay ! h264parse ! $DECODER name=videodecoder0 ! $QUEUE ! $VIDEOSINK \
 r.send_rtcp_src_0 ! funnel name=f0 ! queue ! udpsink host=$DST_IP port=$PORT0_RTCP sync=false async=false \
 $SCREAMRX0_RTCP udpsrc port=$PORT0_RTCP ! r.recv_rtcp_sink_0 \
\
udpsrc port=$PORT1_RTP address=$LOCAL_IP ! \
 queue $SCREAMRX1 ! application/x-rtp, media=video, encoding-name=H264, clock-rate=90000 ! r.recv_rtp_sink_1 r. ! rtph264depay ! h264parse ! $DECODER name=videodecoder1 ! $QUEUE ! $VIDEOSINK \
  r.send_rtcp_src_1 ! funnel name=f1 ! queue ! udpsink host=$DST_IP port=$PORT1_RTCP sync=false async=false \
  $SCREAMRX1_RTCP udpsrc port=$PORT1_RTCP ! r.recv_rtcp_sink_1 \
\
udpsrc port=$PORT2_RTP address=$LOCAL_IP ! \
 queue $SCREAMRX2 ! application/x-rtp, media=video, encoding-name=H264, clock-rate=90000 ! r.recv_rtp_sink_2 r. ! rtph264depay ! h264parse ! $DECODER name=videodecoder2 ! $QUEUE ! $VIDEOSINK \
 r.send_rtcp_src_2 ! funnel name=f2 ! queue ! udpsink host=$DST_IP port=$PORT2_RTCP sync=false async=false \
 $SCREAMRX2_RTCP udpsrc port=$PORT2_RTCP ! r.recv_rtcp_sink_2 \
"

export GST_DEBUG="screamrx:2"
killall -9 scream_receiver
$SCREAM_TARGET_DIR/scream_receiver
