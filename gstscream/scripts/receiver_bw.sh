export GST_PLUGIN_PATH=$GST_PLUGIN_PATH:../target/debug/
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:../../code/wrapper_lib

export RECVPIPELINE="rtpbin latency=10 name=rtpbin udpsrc port=30112 address=127.0.0.1 ! screamrx name=screamrx screamrx.src ! fakesink  rtpbin.send_rtcp_src_0 ! funnel name=f ! udpsink host=127.0.0.2 port=30112 sync=false async=false screamrx.rtcp_src ! f. "

killall -9 scream_receiver
../target/debug/scream_receiver
