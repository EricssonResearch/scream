# Installation

[rust](https://doc.rust-lang.org/book/ch01-01-installation.html#installing-rustup-on-linux-or-macos)

[gstreamer](https://gstreamer.freedesktop.org/documentation/installing/on-linux.html?gi-language=c)

# Building gstscream and sample applications
Should be possible to build and run on Windows (not tested)  
However  SCReAM BW  can't be built for Windows.  
Therefore modify build.sh to remove --features screamtxbw-enabled
```bash
./scripts/build.sh
```

# Environment Variables are set in script/env.sh
```bash
export SENDER_STATS_TIMER=500               # default 1000 ms
export SENDER_STATS_FILE_NAME="xxx.csv"     # default sender_scream_stats.csv
```
See script/env.sh to set  
ECN_ENABLED, USE_SCREAM, SENDER_IP, RECEIVER_IP, ENCODER, DECODER, ENC_ID  

# Tuning  Linux system 
```bash
sudo ./scripts/sysctl.sh 
```
# Running remote rendering test applications
```bash
# first window
./scripts/receiver.sh
# second window
./scripts/sender.sh
```
# Running remote rendering test applications with 3 video screams
```bash
# first window
./scripts/receiver_3.sh
# second window
./scripts/sender_3.sh
```
# Running SCReAM BW test applications
```bash
# first window
./scripts/receiver_bw.sh
# second window
./scripts/sender_bw.sh
```
# To use unattended machine:
```shell
#ssh-1 window
systemctl isolate multi-user.target
sudo Xorg

#ssh-2 window
export DISPLAY=:0
./receiver_run.sh
#ssh-3 window
export DISPLAY=:0
./sender_run.sh
```

# __Be aware of legal implications of using software build with gpl=enabled__

# Modifying gstreamer to use L4S
Based on patch [https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/2717]  
```bash
cd scream/..
git clone https://gitlab.freedesktop.org/gstreamer/gstreamer.git
cd gstreamer
meson setup --prefix=/path/to/install/prefix build

# to build x264, you might want to add to meson setup
-Dgst-plugins-ugly:x264=enabled -Dgpl=enabled  

meson compile -C build
patch -p1 < scream/gstscream/udp_ecn_diff.txt
meson compile -C build
meson install -C build
```
# Build and run with modified gstreamer
## Modify scripts/env.sh 
set ECN_ENABLED=1
Make sure that MY_GST_INSTALL points to the correct install directory.
## Build
./scripts/build.sh
## Run
Use receiver and sender scripts described earlier
Receiver should print once "screamrx/imp.rs ecn-enabled"
If export GST_DEBUG="screamrx:7" is set, the following trace log should be displayed:
screamrx src/screamrx/imp.rs ..... ecn_ce 1 

# Issues
[Issues with using tc](https://github.com/EricssonResearch/scream/issues/44#issuecomment-1112448356 )

# Logging
File `gstscream/src/sender_util.rs` provides an example of how to retrieve periodic statistics
from the SCReAM library into a `.csv` file. Below is an explanation for some of the important
fields:

* `time-ns`          Time in ns of getting log record.
* `queueDelay`       The estimated network queue delay, i.e time a packet is read from
   internal RTP queue and time a packer read by receiver screamrx element. It is "estimated"
   because the clocks on the sender and receiver sides are not synchronized. (In sec)

* `queueDelayMax`    The maximum value of `queueDelay` in seconds, which is reset after
   printing logs.

* `queueDelayMin`    The minimum value of `queueDelay` in seconds, which is reset after
   printing logs. Together,  queueDelayXXX  fields indicate size of jitter.

* `rtpQueueDelay`    The duration for which RTP packets are kept in the RTP queue.
   If `rtpQueueDelay` exceeds a hardcoded threshold the RTP queue
   is completely emptied.  Number of cleared packets added to Counter packetetsRtpCleared

* `acketsLost`       Unacked packets outside window are considered lost. They might be received by receiver.
   Used to calculate "targetBitrate"

* `rateRtp vs. rateTransmitted` `rateRtp` is the moving avr bitrate including RTP header
   of packet read by screamtx element, while `rateTransmitted` is the transmitted bitrate. These
   can differ if RTP packets are queued on the sender side. (In  kbit/sec)


* `rtpQueueBytes and rtpQueueSize` These represent bytes in the RTP queue and the number of packets,
   respectively, at the time of logging. Not average.

* `targetBitrate`    SCReAM recommended to encoder at the time of logging. Not avr.

* `bytesInFlightLog` Max of bytesInFlight for time between two log records.

* `Cwnd`             Congestion Window  the amount of data that can be sent into the network before needing an acknowledgment. in Bytes

* `srtt`             Smoothed Round-Trip Time. a measure to estimate the round-trip time(RTT) in seconds

# building graphs in send_graphs directory
```bash
./scripts/screamtx_stats.py  sender_scream_stats_0.csv
```
# Arguments:
Scream has many parameters kept for historical reasons or for development. Current SCReAM is self
adjusting with default parameters.
Here are few that you might want to set
* `-minrate, -maxrate, initrate`  in kbit/sec
   If encoder you are using uses bit/sec as bitrate property, you'll have to convert in  your code when you set encoder
   bitrate property.
* `force_idr` If packet loss is detected, gstscream, will send :UpstreamForceKeyUnitEvent, no more
   often than every 100ms
   There is alternative way to receiver application to send upstream GstForceKeyUnit event. In
   this case -force_idr should not be set

