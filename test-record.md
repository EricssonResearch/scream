# Test activities with SCReAM

## Test activity 2024-10-28
SCReAM test with 100Mbps bottleneck with DualPi2 AQM
SCReAM commit 994f9a40d4b4a78b8052551e3480b30269d75a3a (2024-10-28)

### Hardware and qdisc configuration
The test laptops are two Lenovo ThinkPad E470 with Ubuntu 20.04. The PCs are interconnected with an ethernet cable
Linux release 5.15 with TCP Prague and DualPi2 from 
https://github.com/L4STeam/linux is used. Iperf is used as additional load generator, confugured to use L4S and Prague

The bottleneck is configured on the receiving (client side) with the below script

~~~
# Script that runs on the receiving side  to model an 
# L4S-capable link with 100Mbps throughput and 25ms RTT
# $INTERFACE is the network interface

sudo modprobe ifb numifbs=1
sudo ip link set dev ifb0 up

# Delete old stuff
sudo tc qdisc del dev $INTERFACE root
sudo tc qdisc del dev ifb0 root
sudo tc qdisc del dev $INTERFACE handle ffff: ingress

sudo tc qdisc add dev $INTERFACE handle ffff: ingress
sudo tc filter add dev $INTERFACE parent ffff: protocol ip u32 match u32 0 0 action mirred egress redirect dev ifb0

# Add 25ms RTT to egress interface
sudo tc qdisc add dev $INTERFACE root netem delay 25ms

# Ingress, limit and add L4S marking
sudo tc qdisc add dev ifb0 root handle 1: htb default 10
sudo tc class add dev ifb0 parent 1: classid 1:1 htb rate 100mbit ceil 100mbit
sudo tc class add dev ifb0 parent 1:1 classid 1:10 htb rate 100mbit ceil 100mbit
sudo tc qdisc add dev ifb0 parent 1:10 dualpi2
~~~


The expected SCReAM bitrate  [Mbps] given the number of additional TCP loads  is 

| 0 TCP | 1 TCP | 2 TCP | 3 TCP | 4 TCP |
|:-:|:-:|:-:|:-:|:-:|
| 100 | 50 | 33 | 25 | 20 |

All tests are run with only one iteration so one can expect a roughlt +/- 5% deviation from the figures below.

### Test 1, impact of microburst interval
The test examines the impact of the microburst interval in the SCReAM performance without additional load and with 
-microburstinterval X
-mtu 1360 (1400 byte in IP level)
-rand 0

| X [ms]  | 0 TCP | 1 TCP | 2 TCP | 3 TCP | 4 TCP |
| :- |:-:|:-:|:-:|:-:|:-:|
| 0.25 | 71 | 56 | 46 | 39 | 28 |
| 0.5 | 75 | 60 | 48 | 40 | 30 |
| 1.0 | 68 | 56 | 44 | 38 | 30 |
| 2.0 | 41 | 31 | 29 | 29 | 26 |

The results indicate that a 2ms microburst interval causes SCReAM to over mark in the DualPi2 AQM. It is therefore recommended to use a smaller microburst interval such as 0.5ms.
The results in general indicated that SCReAM underutilizes the link capacity when is does not need to share the capacity with other flows. This is an expected behavior because of the 50% pacing headroom in SCReAM. Furthermore, SCReAM gets more than its fair share when additional load is applied.

### Test 2, different RTTs
The test is run with 4 different values of RTT and up to 4 additional TCP loads

-microburstinterval 0.5
-mtu 1360 (1400 byte in IP level)
-rand 0 (fixed frame sizes)

| RTT [ms]  | 0 TCP | 1 TCP | 2 TCP | 3 TCP | 4 TCP |
| :- |:-:|:-:|:-:|:-:|:-:|
| 5 | 80 | 55 | 35 | 25 | 22 |
| 10 | 75 | 55 | 44 | 30 | 23 |
| 25 | 75 | 60 | 48 | 40 | 30 |
| 50 | 72 | 50 | 32 | 27 | 22 |

The results give that SCReAM gives rougly the same results regardless of RTT, within the 5ms - 50ms range. SCReAM also gets more than its fair share  when additional load is applied.

### Test3, different RTTs, and varying video frame sizes
Like test # 2 but varying video frame sizes

-microburstinterval 0.5
-mtu 1360 : 1400 byte on IP level
-rand 50 : frame sizes vary +/-1 50% from the nominal (uniform distribution)

| RTT [ms]  | 0 TCP | 1 TCP | 2 TCP | 3 TCP | 4 TCP |
| :- |:-:|:-:|:-:|:-:|:-:|
| 5 | 69 | 45 | 35 | 26 | 18 |
| 10 | 66 | 46 | 40 | 28 | 21 |
| 25 | 64 | 53 | 40 | 32 | 22 |
| 50 | 63 | 52 | 39 | 25 | 18 |

SCReAM gives a lower bitrate when video frame sizes vary. This is an expected result, given that SCReAM adjusts itself to avoid queue build-up because of larger video frames than expected. SCReAM is also (roughly) more fair against other flows

| 0 TCP | 1 TCP | 2 TCP | 3 TCP | 4 TCP |
|:-:|:-:|:-:|:-:|:-:|
| 100 | 50 | 33 | 25 | 20 |

### Summary
In the given test setup with a 100Mbps DualPi2 AQM it can be concluded that: 

+ SCReAM is quite robust under competition from up to 4 competing TCP flows within a 5 to 50ms RTT range.
+ SCReAM underutilizes the link capacity when no other competing traffic. This is an expected outcome because of the 50% pacing headroom.
+ SCReAM gets more than its fair share of the capacity when frame size is fixed. The unfairness is smaller when frame sizes vary in size.
+ Microburst interval = 0.5ms is recommended.

