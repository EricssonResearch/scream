#!/bin/bash
ECN_ENABLED=1
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/ecn_env.sh
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
cd $SCREAMLIB_DIR; cmake .; make
cd $SCRIPT_DIR
export RUSTFLAGS="$RUSTFLAGS -L$SCREAMLIB_DIR"
if (($ECN_ENABLED == 1)); then
    cargo build --features ecn-enabled
    cargo clippy --features ecn-enabled
else
    cargo build
    cargo clippy
fi    

