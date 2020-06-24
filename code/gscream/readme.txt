Build SCReAM gstreamer plugins and send and receive applications in Linux (Ubuntu)

1) Install applicable gstreamer thingys

You need these
https://developer.ridgerun.com/wiki/index.php?title=Setting_a_GStreamer_Alternative_Environment
sudo apt-get install \
pkg-config bison flex git \
libglib2.0-dev liborc-0.4-dev \
libtool autopoint autoconf \
gettext yasm

From https://gstreamer.freedesktop.org/documentation/installing/on-linux.html
apt-get install libgstreamer1.0-0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-doc gstreamer1.0-tools gstreamer1.0-x

In addition you need
sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

===== Build =====
It is of course assumed that you have two Linux laptops, Ubuntu 18.04 LTE seems to work OK

2) Set path
export GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0/

3) Make plugins

3a) Make gscreamtx plugin
cd ./gst-gscreamtx/gst-plugin
./autogen.sh

3b) Make gscreamrx plugin
cd ./gst-gscreamrx/gst-plugin
./autogen.sh

4) Run sender and receiver pipelines

4a) Start receiver side
export SENDER_IP=X.X.X.X #The IP address of the sender side
gst-launch-1.0 rtpbin name=rtpbin udpsrc port=5000 ! gscreamrx ! application/x-rtp,media=video,clock-rate=90000,encoding-name=H264 ! rtpbin.recv_rtp_sink_0 rtpbin. ! rtph264depay ! avdec_h264 ! videoconvert ! xvimagesink sync=false async=false udpsrc port=5001 ! rtpbin.recv_rtcp_sink_0  rtpbin.send_rtcp_src_0 ! udpsink port=5001 bind-port=5001 host=$SENDER_IP sync=false async=false

4b) Start sender side
export RECEIVER_IP=Y.Y.Y.Y

# rpicamsrc, quite decent performance, given the low total cost. The only problem is that the rate control loop is a bit slow, for this reason the rata adaptation in SCReAM is tuned to be a bit slower than normal
gst-launch-1.0 rtpbin name=rtpbin ! rpicamsrc name=video intra-refresh-type=cyclic-rows preview=false annotation-mode=1 inline-headers=true ! video/x-h264,width=1920,height=1080,framerate=30/1,profile=high ! rtph264pay ! gscreamtx media-src=1 ! rtpbin.send_rtp_sink_0 rtpbin.send_rtp_src_0 ! udpsink host=$RECEIVER_IP port=5000 bind-port=5000 rtpbin.send_rtcp_src_0 ! udpsink host=$RECEIVER_IP bind-port=5001 port=5001 udpsrc port=5001 ! rtpbin.recv_rtcp_sink_0

# uvch264src, this alternative behaves wery well with a reasonably fast video encoder rate control loop, a rate change converges within 300-400ms
gst-launch-1.0 rtpbin name=rtpbin ! uvch264src device=/dev/video0 name=video auto-start=true leaky-bucket-size=1000 ltr-buffer-size=0 ltr-encoder-control=0 iframe-period=1000 min-iframe-qp=20 video.vidsrc ! queue ! video/x-h264,width=1920,height=1080,framerate=30/1,profile=high ! rtph264pay ! gscreamtx media-src=2 ! rtpbin.send_rtp_sink_0 rtpbin.send_rtp_src_0 ! udpsink host=$RECEIVER_IP port=5000 bind-port=5000 rtpbin.send_rtcp_src_0 ! udpsink host=$RECEIVER_IP bind-port=5001 port=5001 udpsrc port=5001 ! rtpbin.recv_rtcp_sink_0

# omxh264enc jpeg from Logitech C920 USB cam (seems to be some problems getting this to run on an RPI)
gst-launch-1.0 rtpbin name=rtpbin ! v4l2src device=/dev/video0 ! image/jpeg,width=640,height=480,framerate=10/1 ! avdec_mjpeg ! videoconvert ! omxh264enc name=video ! rtph264pay ! gscreamtx media-src=4 ! rtpbin.send_rtp_sink_0 rtpbin.send_rtp_src_0 ! udpsink host=$RECEIVER_IP port=5000 bind-port=5000 rtpbin.send_rtcp_src_0 ! udpsink host=$RECEIVER_IP bind-port=5001 port=5001 udpsrc port=5001 ! rtpbin.recv_rtcp_sink_0

# x264enc
gst-launch-1.0 rtpbin name=rtpbin ! v4l2src device=/dev/video0 ! videoconvert ! video/x-raw,width=1920,height=1080,framerate=30/1 ! x264enc name=video tune=zerolatency ! rtph264pay ! gscreamtx media-src=0 ! rtpbin.send_rtp_sink_0 rtpbin.send_rtp_src_0 ! udpsink host=$RECEIVER_IP port=5000 bind-port=5000 rtpbin.send_rtcp_src_0 ! udpsink host=$RECEIVER_IP bind-port=5001 port=5001 udpsrc port=5001 ! rtpbin.recv_rtcp_sink_0

# x264enc jpeg from Logitech C920 USB cam
gst-launch-1.0 rtpbin name=rtpbin ! v4l2src device=/dev/video0 ! image/jpeg,width=1920,height=1080,framerate=30/1 ! avdec_mjpeg ! videoconvert ! x264enc name=video tune=zerolatency ! rtph264pay ! gscreamtx media-src=0 ! rtpbin.send_rtp_sink_0 rtpbin.send_rtp_src_0 ! udpsink host=$RECEIVER_IP port=5000 bind-port=5000 rtpbin.send_rtcp_src_0 ! udpsink host=$RECEIVER_IP bind-port=5001 port=5001 udpsrc port=5001 ! rtpbin.recv_rtcp_sink_0

# vaapih264enc, there seems to be some problems with the rate control here
gst-launch-1.0 rtpbin name=rtpbin ! v4l2src device=/dev/video1 ! image/jpeg,width=1920,height=1080,framerate=30/1,profile=high ! vaapijpegdec ! videoconvert ! vaapih264enc name=video rate-control=2 ! rtph264pay ! gscreamtx media-src=3 ! rtpbin.send_rtp_sink_0 rtpbin.send_rtp_src_0 ! udpsink host=$RECEIVER_IP port=5000 bind-port=5000 rtpbin.send_rtcp_src_0 ! udpsink host=$RECEIVER_IP bind-port=5001 port=5001 udpsrc port=5001 ! rtpbin.recv_rtcp_sink_0

======================================================
Comments
======================================================
The application is written to work with x264enc,rpicamsrc,uvch264src,omxh264enc and vaapih264enc
1) Simple bottlenecks with 50ms RTT, with rate limits ranging between 500kbps and infinity
2) Home network over WiFi with RTT ~5ms
3) LTE (Telia MBB) with receiver on publix fixed IP

The sender plugin currently only supports one stream, extension to more streams and even multiplexing many streams over one SCReAM instance is left as a future exercise.

======================================================
To Do
======================================================
(shorter term)
1) Clean up code, remove possible memory leaks, almost done, the sender side does not seem to leak
2) Enable reduced size RTCP

(longer term)
3) Make plugin handle more than one stream




============================
Raspberry..
Install rpicamsrc
https://thepihut.com/blogs/raspberry-pi-tutorials/16021420-how-to-install-use-the-raspberry-pi-camera


Issue. File glibconfig.h is missing when gscream plugin is built.
Solution copy file glibconfig.h from
/usr/lib/arm-linux-gnueabihf/glib-2.0/include
to
/usr/include/glib-2.0
