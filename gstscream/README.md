# Installation

[rust](https://doc.rust-lang.org/book/ch01-01-installation.html#installing-rustup-on-linux-or-macos)

[gstreamer](https://gstreamer.freedesktop.org/documentation/installing/on-linux.html?gi-language=c)

# Building gstscream and sample applications
Should be possible to build and run on Windows (not tested)  
However  SCReAM BW  can't be built for Windows.  
Therefore you cannot build with --features screamtxbw-enabled

```bash
# To build with L4S and with SCReAM BW
cargo build --features=ecn-enabled,screamtxbw-enabled

# To build without SCReAM BW
cargo build --features=ecn-enabled
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
# __Be aware of legal implications of using software build with gpl=enabled__

# Modifying gstreamer to use L4S
Based on patch [https://gitlab.freedesktop.org/gstreamer/gstreamer/-/merge_requests/2717]  
```bash
cd scream/..
git clone https://gitlab.freedesktop.org/gstreamer/gstreamer.git
cd gstreamer
meson setup \
   --prefix==/path/to/install/prefix build

# to build x264, you might want to add to meson setup
-Dgst-plugins-ugly:x264=enabled -Dgpl=enabled  

meson compile -C build
patch -p1 < scream/gstscream/udp_ecn_diff.txt
# manually modify gstnvbaseenc.c to fix issue with  nvenc bitrate changes run time 
# [https://github.com/EricssonResearch/scream/issues/44#issuecomment-1150002189]
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
[Issues with nvenc bitrate changes run time](with https://github.com/EricssonResearch/scream/issues/44#issuecomment-1150002189 )

[Issues with using tc](https://github.com/EricssonResearch/scream/issues/44#issuecomment-1112448356 )

