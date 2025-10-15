#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/env.sh
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
cd $SCREAMLIB_DIR; cmake .; make
cd $SCRIPT_DIR
export RUSTFLAGS="$RUSTFLAGS -L$SCREAMLIB_DIR -C link-args=-Wl,-rpath,$SCREAMLIB_DIR"
if (($ECN_ENABLED == 1)); then
    cargo build --features ecn-enabled,screamtxbw-enabled
    cargo clippy --features ecn-enabled,screamtxbw-enabled
else
    cargo build --features screamtxbw-enabled
    cargo clippy --features screamtxbw-enabled
fi    

