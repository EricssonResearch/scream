export GST_PLUGIN_PATH=$GST_PLUGIN_PATH:../target/debug/
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:../../code/wrapper_lib

export SENDPIPELINE=" screamtxbw  name=video params=\"-numframes 10000 -initrate 1000 -ssrc 1\" !  queue ! tee name=t t. ! queue ! screamtx name=screamtx params=\" -forceidr -ect 1 -initrate 2500 -minrate 500 -maxrate 10000 \" !  udpsink host=127.0.0.1 port=30112 sync=false t. ! queue ! fakesink silent=false sync=false rtpbin name=r udpsrc port=30112 address=127.0.0.2 ! queue ! screamtx.rtcp_sink screamtx.rtcp_src ! r.recv_rtcp_sink_0 "

killall -9 scream_sender
../target/debug/scream_sender
