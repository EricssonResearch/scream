export GST_PLUGIN_PATH=$GST_PLUGIN_PATH:../target/debug/
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:../../code/wrapper_lib

export RECVPIPELINE="rtpbin latency=10 name=rtpbin udpsrc port=30112 address=127.0.0.1 ! screamrx name=screamrx screamrx.src ! application/x-rtp, media=video, encoding-name=H264, clock-rate=90000 ! rtpbin.recv_rtp_sink_0 rtpbin. ! rtph264depay ! h264parse ! avdec_h264 name=videodecoder ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! glupload ! glcolorconvert ! fpsdisplaysink video-sink=\"glimagesinkelement\" rtpbin.send_rtcp_src_0 ! funnel name=f ! udpsink host=127.0.0.2 port=30112 sync=false async=false screamrx.rtcp_src ! f. "

killall -9 scream_receiver
../target/debug/scream_receiver
