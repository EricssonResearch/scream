# Installation

[rust](https://doc.rust-lang.org/book/ch01-01-installation.html#installing-rustup-on-linux-or-macos)

[gstreamer](https://lib.rs/crates/gstreamer)

# Building gstscream and sample applications

```bash
cd scripts
./build.sh
```

# Environment Variables
```bash
export SENDER_STATS_TIMER=500               # default 1000 ms
export SENDER_STATS_FILE_NAME="xxx.csv"     # default sender_scream_stats.csv
```
# Tuning  Linux system 
```bash
cd scripts
sudo ./sysctl.sh 
```

# Running SCReAM BW test applications
```bash
cd scripts
# first window
./receiver_bw.sh
# second window
./sender_bw.sh
```

# Running remote rendering test applications
```bash
cd scripts
# first window
./receiver.sh
# second window
./sender.sh
```

# Running remote rendering test applications with 3 video screams
```bash
cd scripts
# first window
./receiver_3.sh
# second window
./sender_3.sh
```
# Issues
[Issues with nvenc bitrate changes run time](with https://github.com/EricssonResearch/scream/issues/44#issuecomment-1150002189 )

[Issues with using tc](https://github.com/EricssonResearch/scream/issues/44#issuecomment-1112448356 )

