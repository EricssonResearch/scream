export GST_PLUGIN_PATH=$GST_PLUGIN_PATH:../target/debug/
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:../../code/wrapper_lib

export SENDPIPELINE=" videotestsrc is-live=true pattern=snow ! x264enc name=video threads=4 speed-preset=ultrafast tune=fastdecode+zerolatency bitrate=10000 ! tee name=t t. ! queue ! rtph264pay ssrc=1 config-interval=-1 ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! screamtx name=screamtx params=\" -forceidr -ect 1 -initrate 2500 -minrate 500 -maxrate 10000 \" !  udpsink host=127.0.0.1 port=30112 sync=false t. ! queue ! fakesink silent=false sync=false rtpbin name=r udpsrc port=30112 address=127.0.0.2 ! queue ! screamtx.rtcp_sink screamtx.rtcp_src ! r.recv_rtcp_sink_0 "

killall -9 scream_sender
../target/debug/scream_sender
