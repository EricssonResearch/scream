#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/ecn_env.sh
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
SCREAM_TARGET_DIR=$SCRIPT_DIR/../target/debug/
export GST_PLUGIN_PATH=$SCREAM_TARGET_DIR:$GST_PLUGIN_PATH
export LD_LIBRARY_PATH=$SCREAMLIB_DIR:$LD_LIBRARY_PATH

export SENDPIPELINE=" screamtxbw  name=encoder0 params=\"-numframes 10000 -initrate 1000 -ssrc 1\" !  queue ! tee name=t t. ! queue ! screamtx name=screamtx0 params=\" -forceidr -ect 1 -initrate 2500 -minrate 500 -maxrate 10000 \" !  udpsink host=127.0.0.1 port=30112 sync=false t. ! queue ! fakesink silent=false sync=false rtpbin name=r udpsrc port=30112 address=127.0.0.2 ! queue ! screamtx0.rtcp_sink screamtx0.rtcp_src ! r.recv_rtcp_sink_0 "

killall -9 scream_sender
export RUST_BACKTRACE=1
$SCREAM_TARGET_DIR/scream_sender
