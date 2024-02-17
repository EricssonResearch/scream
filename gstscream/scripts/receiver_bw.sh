#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/env.sh

export RECVPIPELINE="rtpbin latency=10 name=r udpsrc port=$PORT0_RTP address=$RECEIVER_IP ! screamrx name=screamrx screamrx.src ! fakesink  r.send_rtcp_src_0 ! funnel name=f ! udpsink host=$SENDER_IP port=$PORT0_RTCP sync=false async=false screamrx.rtcp_src ! f. "

killall -9 scream_receiver
$SCREAM_TARGET_DIR/scream_receiver
