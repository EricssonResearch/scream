SCRIPT_PATH=$(realpath  $0)
SCRIPT_DIR=$(dirname  $SCRIPT_PATH)
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
cd $SCREAMLIB_DIR; cmake .; make
cd $SCRIPT_DIR
export RUSTFLAGS="$RUSTFLAGS -L$SCREAMLIB_DIR"
cargo build

