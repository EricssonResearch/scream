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

3) Make applications
>make

3) Make plugins

3a) Make gscreamtx plugin
cd ./gst-gscreamtx/gst-plugin
chmod +x autogen.sh
chmod +x config.status
./autogen.sh

3b) Make gscreamrx plugin
cd ./gst-gscreamrx/gst-plugin
chmod +x autogen.sh
chmod +x config.status
./autogen.sh

4) Run applications

4a) Start receiver side
./gscream_app_rx <sender-host-ip>
Will listen to incoming RTP on port 5000, RTCP SR on port 5001
RTCP feedback is sent on port 5003

4b) Start sender side
./gscream_app_tx /dev/video0 <receiver-host-ip>
Will send RTP on port 5000, RTCP SR on port 5001
Listen to RTCP feedback on port 5003
a USB camera can be plugged in, it is then likely /dev/video1

======================================================
Comments
======================================================
The application is written to work with x264enc and it is configured to run in zerolatency mode, the camera to screen latency is then measured to be ~130ms. The application is so far tried out over 
1) Simple bottlenecks with 50ms RTT, with rate limits ranging between 500kbps and infinity
2) Home network over WiFi with RTT ~5ms

The sender plugin should be possible to make fairly generic in the end but currently it is assumed that the video coder is x264enc, because the bitrate is set via the object "bitrate" and the unit is in kbps, for instance vp8enc has the name "target-bitrate" for same property.
There is a problem in the sender plugin that the reference to the filter is not passed correctly to the on_receiving_rtp callback. This is currently fixed with an ugly hack.
The sender plugin currently only supports one stream, extension to more streams and even multiplexing many streams over one SCReAM instance is left as a future exercise.

Intra-refresh mode is tried out but it seems to be sensitive to delay jitter as it clears the playout screen for small delays. On reflection on IDR frames in x264 is that they seem to spike very little in bitrate, this may be because of the zero latency mode.
If you somehow exerience that the video bitrate drops unexpectedly, even though no congestion exists, it may be because of CPU overload in the laptop. For instance an ASUS UX360C with a Core M3 seems to overheat with the result that the bitrate drops from 5Mbps down to 500kbps. A Lenovo E470 does not show these issues.

As of Feb 15, it runs on a Raspberry PI 3 B with the Raspberry camera. The performance is goo, however the rate control in the video encoder (rpicamsrc) is a bit slow, which makes rate control a bit sluggish, for that reason the rate rampup speed in SCReAM is tuned down a bit to avoid unnecessary latency increase.

======================================================
To Do
======================================================
(shorter term)
1) Clean up code, remove possible memory leaks, almost done, the sender side does not seem to leak
2) Enable reduced size RTCP

(longer term)
5) Make plugin handle more than one stream




============================
Raspberry..
Install rpicamsrc
https://thepihut.com/blogs/raspberry-pi-tutorials/16021420-how-to-install-use-the-raspberry-pi-camera

Enable the use of rpicamsrc with the 
#define RPICAM 
in gstgscreamtx.cpp
Build gst-screamtx plugin

Issue. File glibconfig.h is missing when gscream plugin is built.
Solution copy file glibconfig.h from 
/usr/lib/arm-linux-gnueabihf/glib-2.0/include
to 
/usr/include/glib-2.0

make the applications
run sender side with 
./gscream_app_rpi_tx <ip>











