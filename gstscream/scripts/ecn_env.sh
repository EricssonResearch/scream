export ECN_ENABLED=0
if (($ECN_ENABLED == 1)); then
export SET_ECN="set-ecn=1"
export SCREAMTX_PARAM_ECT="-ect 1"
export RETRIEVE_ECN="retrieve-ecn=true"
MY_GST_INSTALL=$(readlink -f $SCRIPT_DIR/../../../gstreamer/install)
ls $MY_GST_INSTALL
export GST_PLUGIN_PATH=$GST_PLUGIN_PATH:$MY_GST_INSTALL/lib/x86_64-linux-gnu
export GST_PLUGIN_PATH=$GST_PLUGIN_PATH:$MY_GST_INSTALL/lib/x86_64-linux-gnu/gstreamer-1.0
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$MY_GST_INSTALL/lib/x86_64-linux-gnu:$MY_GST_INSTALL/lib/x86_64-linux-gnu/gstreamer-1.0
export PATH=$MY_GST_INSTALL/bin:$PATH
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:$MY_GST_INSTALL/lib/x86_64-linux-gnu/pkgconfig
export PYTHONPATH=${PYTHONPATH}:$MY_GST_INSTALL/lib/python3.10/site-packages/
LP=$(pkg-config --libs-only-L  gstreamer-1.0 )
echo $LP
env
export RUSTFLAGS="$LP $RUSTFLAGS"
fi
