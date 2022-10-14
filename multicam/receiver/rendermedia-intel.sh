#!/bin/sh
#
# The receiver side processing is illustrated below
# The SCReAM receiver application receives multiplexed RTP media on port $2
#  and transits RTCP feedback over the same port
# The received RTP media is demultiplexed and forwarded on local
#  ports 30112, 30114, 31016 and 30118
# The video decoding assumes an NVIDIA Jetson Nano or Xavier NX platform, change to applicable
#  HW decoding, depending on platform
#
#                     +----------------------+               +--------------------+
#                     |                      |  Lo:30112     |                    |
#                     |                      +-------------->+  Front camera      |
#                     |                      |               |  decode/render     |
# +------------------>+                      |               +--------------------+
#    $1:$2            |  SCReAM receiver     |
# <-------------------+                      |               +--------------------+
#                     |                      |  Lo:30114     |                    |
#                     |                      +-------------->+  Rear  camera      |
#                     |                      |               |  decode/render     |
#                     +----------------------+               +--------------------+


# Start SCReAM receiver side
./scream/bin/scream_receiver $1 $2 $3 | tee ./Data/scream_$4.txt &


## /dev/video0
gst-launch-1.0 udpsrc port=30112 ! application/x-rtp,media=video,clock-rate=90000,encoding-name=H264 ! rtpjitterbuffer latency=50 ! rtph264depay ! h264parse ! vaapih264dec low-latency=true ! videoconvert ! waylandsink sync=true &

## /dev/video1
gst-launch-1.0 udpsrc port=30114 ! application/x-rtp,media=video,clock-rate=90000,encoding-name=H264 ! rtpjitterbuffer latency=50 ! rtph264depay ! h264parse ! vaapih264dec low-latency=true ! videoconvert ! ximagesink  sync=true &
