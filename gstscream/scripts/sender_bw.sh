#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/env.sh

export SENDPIPELINE=" screamtxbw  name=encoder0 params=\"-numframes 10000 -initrate 1000 -ssrc 1\" !  queue ! tee name=t t. ! queue ! screamtx name=screamtx0 params=\" -forceidr -ect 1 -initrate 2500 -minrate 500 -maxrate 10000 \" !  udpsink host=$RECEIVER_IP port=$PORT0_RTP sync=false t. ! queue ! fakesink silent=false sync=false rtpbin name=r udpsrc port=$PORT0_RTCP address=$SENDER_IP ! queue ! screamtx0.rtcp_sink screamtx0.rtcp_src ! r.recv_rtcp_sink_0 "

killall -9 scream_sender
export RUST_BACKTRACE=1
$SCREAM_TARGET_DIR/scream_sender
