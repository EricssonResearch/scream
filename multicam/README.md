# SCReAM multicamera platform

This README describes the streaming platform for a multicamera solution with SCReAM congestion control. The platform supports up to four cameras and an overview is shown in the figures below (two camera solution). <br>
This streaming solution has been extensively tested for remote control cars but can also be used for e.g drones or remote video production.

The `capture/encode` and `decode/render` blocks are implemented as GStreamer pipelines, this makes the solution portable to different platforms. The `SCReAM sender` and `SCReAM receiver` blocks builds on Linux platforms and supports ECN and L4S. <br>

The `SCReAM sender` receives RTP/UDP traffic from the `capture/render` GStreamer pipelines over local ports 30000,30002,30004,30006 and sends codec control commands (target bitrate and force IDR) on local ports 30001,30003,30005,30007. A dedicated `codecctrl` GStreamer plugin serves as an interface between the `SCReAM sender` and the video encoders in the GStreamer piplelines. <br>

The RTP/UDP streams are multiplexed in the `SCReAM sender` block and transmitted to RECEIVER_IP:UDP_PORT_VIDEO, the RTCP feedback from the receiver is also over the same UDP socket. The `SCReAM receiver` block receives the multiplexed RTP/UDP streams, generates RTCP feedback (RFC8888) to the sender and demultiplexes the streams and forwards them to the `decode/render` GStreamer pipelines.


The sender side schematic is according to figure 1 below

    +-----------------+  Lo:30001  +-------------------+
    |                 +<-----------+                   |
    |  /dev/video0    |            |                   |
    |  capture/encode +----------->+                   |
    +-----------------+  Lo:30000  |                   +--------------------------->
                                   |   SCReAM sender   | RECEIVER_IP:UDP_PORT_VIDEO
    +-----------------+  Lo:30003  |                   +<---------------------------
    |                 +<-----------+                   |
    |  /dev/video1    |            |                   |
    |  capture/encode +----------->+                   |
    +-----------------+  Lo:30002  +-------------------+
    Figure 1

The receiver side schematic is according to figure 2 below

                       +-------------------+            +-----------------+
                       |                   |  Lo:30112  |                 |
                       |                   +----------->+  Front camera   |
                       |                   |            |  decode/render  |
    +----------------->+                   |            +-----------------+
     $1:UDP_PORT_VIDEO |  SCReAM receiver  |
    <------------------+                   |            +-----------------+
                       |                   |  Lo:30114  |                 |
                       |                   +----------->+  Rear  camera   |
                       |                   |            |  decode/render  |
                       +-------------------+            +-----------------+
    Figure 2

## Start streaming
The sender side is started with he script <br>
`$ ./startsender.sh` <br>
Note that the IP address needs to be changed to point at the receiving side

The receiver side is started with <br>
`$ ./startreceiver.sh` <br>
This scripts waits for streaming traffic to arrive on the configured UDP port, in addition it also decodes the dst_port in case a NAT has remapped this port.
To ensure proper function it is recommended to start the sender side first, then the receiver side.

## Bill of materials
The sender side is built on the NVIDIA Jetson platforms, other platforms and cameras are possible. The BoM for the NVIDIA Jetson is: <br>
  * NVIDIA Jetson Nano Rev B01. For improved performance the Xavier NX can be used <br>
  * e-CAM50_CUNANO, one or two depending on desired configuration https://www.e-consystems.com/nvidia-cameras/jetson-nano-cameras/5mp-mipi-nano-camera.asp  .. or.. <br>
  * Raspberry PI HQ <br>
  * A proper 4G or 5G modem
  * A battery

## Other platforms
The solution is possible to implement on other Linux based platforms such as Raspberry PI 4. The `SCReAM sender` and `SCReAM receiver` should not need any changes.<br>
The GStreamer pipelines do however need modifications as other hardware video encoders and decoder blocks are used. Furthermore the parameter settings for the `codecctrl` as well as the implementation may need to be modified as different video encoders can use different units ( [bit/s] or [kbit/s] ) and also the parameter name for the target bitrate can vary.

## More than two cameras
The scripts in the sender and receiver folders support up to 2 cameras. This can be extended to up to 4 cameras without modifications in the `SCReAM sender` and `SCReAM receiver` code. Platforms such as NVIDIA Jetson AGX Xavier support many cameras, for this purpose the `startsender.sh` and `rendermedia.sh` scripts should be modified to support more cameras.
