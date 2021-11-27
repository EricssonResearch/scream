SCRIPT_DIR=`pwd`
SCREAMLIB_DIR=$SCRIPT_DIR/../../code/wrapper_lib
cd $SCREAMLIB_DIR; cmake .; make
cd $SCRIPT_DIR
export RUSTFLAGS="$RUSTFLAGS -L$SCREAMLIB_DIR"
cargo build

