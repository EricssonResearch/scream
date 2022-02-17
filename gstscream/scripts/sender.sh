export GST_PLUGIN_PATH=$GST_PLUGIN_PATH:../target/debug/
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:../../code/wrapper_lib

DST_IP=127.0.0.1
LOCAL_IP=127.0.0.2
PORT=30112

QUEUE="queue max-size-buffers=2 max-size-bytes=0 max-size-time=0"
INIT_ENC_BITRATE=10000
NOSUMMARY=" -nosummary"

USE_SCREAM=1
if (($USE_SCREAM == 1)); then
    SCREAMTX="$QUEUE !  screamtx name=\"screamtx\" params=\" $NOSUMMARY -forceidr -ect 1  -initrate 2500 -minrate 500  -maxrate 10000 \" ! "
    SCREAMTX_RTCP="rtpbin name=r udpsrc port=$PORT address=$LOCAL_IP ! queue ! screamtx.rtcp_sink screamtx.rtcp_src !  r.recv_rtcp_sink_0 "
else
    SCREAMTX=""
    SCREAMTX_RTCP=""
fi

SENDPIPELINE1=" videotestsrc is-live=true pattern=snow ! x264enc name=video threads=4 speed-preset=ultrafast tune=fastdecode+zerolatency bitrate=$INIT_ENC_BITRATE ! $QUEUE ! rtph264pay ssrc=1 config-interval=-1 ! $SCREAMTX   udpsink host=$DST_IP port=$PORT sync=false $SCREAMTX_RTCP"

SENDPIPELINE2=" videotestsrc is-live=true ! video/x-raw,format=I420,width=1280,height=720,framerate=25/1 ! x264enc name=video threads=4 speed-preset=ultrafast tune=fastdecode+zerolatency bitrate=$INIT_ENC_BITRATE ! $QUEUE ! rtph264pay ssrc=1 config-interval=-1 ! $SCREAMTX   udpsink host=$DST_IP port=$PORT sync=false $SCREAMTX_RTCP"

export SENDPIPELINE=$SENDPIPELINE1
killall -9 scream_sender
#../target/debug/scream_sender --verbose
../target/debug/scream_sender
