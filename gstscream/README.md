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

