# Sender side build instructions

The NVIDIA is built with the Jetpack and install scripts provided by e-consystems.com upon purchase of the cameras. Make sure that the correct Jetpack version is used as e-consystems may lag behind the Jetpack updates slighly.

The sender side platform contains interfacing with the gstreamer framework
and the SCReAM congestion control. The SCReAM congestion control is currently
run in a stand alone application outside the gstreamer pipeline as it is necessary
to congestion control 2 streams

## codecctrl
The gstreamer codec control is a plugin that serves as an interface between the SCReAM multi
stream congestion control and the gstreamer video encoding entities

**1. Install applicable gstreamer thingys**
You need these
https://developer.ridgerun.com/wiki/index.php?title=Setting_a_GStreamer_Alternative_Environment
`sudo apt-get install pkg-config bison flex git libglib2.0-dev liborc-0.4-dev libtool autopoint autoconf gettext yasm`

From https://gstreamer.freedesktop.org/documentation/installing/on-linux.html
`$ apt-get install libgstreamer1.0-0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-doc gstreamer1.0-tools gstreamer1.0-x`

In addition you need

`$ sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`

**2. Set path**

`export GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0/`

**3. Make codecctrl plugin**

`cd ./gst-codec-ctrl/gst-plugin`

`chmod +x autogen.sh`

`chmod +x config.status`

`./autogen.sh`

`sudo make install`

**Issue.** File glibconfig.h is missing when gscream plugin is built.

**Solution** copy file glibconfig.h from
/usr/lib/aarch64-linux-gnu/glib-2.0/include/ (or where it may be)
to
/usr/include/glib-2.0

**4. Install v4l2-ctl
`sudo apt-get install -y v4l-utils`

**5. Build SCReAM sender side
The SCReAM sender side is built with the instructions
`$ cd ./scream`
`$ cmake .`
`$ make`

To enable SCReAM V2, change SET(CMAKE_CXX_FLAGS "-fPIC -fpermissive -pthread") to SET(CMAKE_CXX_FLAGS "-fPIC -fpermissive -pthread -V2") in CMakeLists.txt


**5. Start streaming
`$ ./startsender.sh`

To ensure proper function it is recommended to start the sender side first, then the receiver side. Or more correctly, the remote end that is connected to a cellular modem should be started first. This can avoid issues with remapped ports.
