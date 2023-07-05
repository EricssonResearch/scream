#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/ecn_env.sh
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
SCREAM_TARGET_DIR=$SCRIPT_DIR/../target/debug/
export GST_PLUGIN_PATH=$SCREAM_TARGET_DIR:$GST_PLUGIN_PATH
export LD_LIBRARY_PATH=$SCREAMLIB_DIR:$LD_LIBRARY_PATH

export RECVPIPELINE="rtpbin latency=10 name=r udpsrc port=30112 address=127.0.0.1 ! screamrx name=screamrx screamrx.src ! fakesink  r.send_rtcp_src_0 ! funnel name=f ! udpsink host=127.0.0.2 port=30112 sync=false async=false screamrx.rtcp_src ! f. "

killall -9 scream_receiver
$SCREAM_TARGET_DIR/scream_receiver
