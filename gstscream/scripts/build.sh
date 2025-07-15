#!/bin/bash
SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
source $SCRIPT_DIR/env.sh
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
cd $SCREAMLIB_DIR; cmake .; make
export RUSTFLAGS="$RUSTFLAGS -L$SCREAMLIB_DIR"
CARGO_MANIFEST="$SCRIPT_DIR/../Cargo.toml"
if (($ECN_ENABLED == 1)); then
    cargo build --manifest-path "$CARGO_MANIFEST" --features ecn-enabled,screamtxbw-enabled
    cargo clippy --manifest-path "$CARGO_MANIFEST" --features ecn-enabled,screamtxbw-enabled
else
    cargo build --manifest-path "$CARGO_MANIFEST" --features screamtxbw-enabled
    cargo clippy --manifest-path "$CARGO_MANIFEST" --features screamtxbw-enabled
fi

