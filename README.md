
# SCReAM
This project includes an implementation of SCReAM, a mobile optimised congestion control algorithm for realtime interactive media.

## News
- 2024-05-20 :
  - -emulatecubic option decoupled for very large CWND.
  - -emulatecubic option also made available for gstreamer plugin wrapper
- 2024-05-11 :
  - -emulatecubic option modified to be adaptive so that SCReAM will have a reasonable chance to compete against competing Prague flows. -emulatecubic is made optional but is recommended as it stabilizes SCReAM target bitrate and can also reduce delay jitter. 
- 2024-05-03 :
  - isslowencoder option renamed to -emulatecubic as it behaves similar to TCP Cubic with a more moderate CWND growth close to the last known max value. The -emulatecubic option reduces bitrate variations at steady state but comes with the drawback that SCReAM can have problems to compete with Prague CC
- 2024-04-25 :
  - Improved stability at very low bitrates 
  - Added isslowencoder option for the case that rate increase should be more moderate. This comes with the risk that SCReAM is starved by e.g TCP Prague.
- 2024-04-19 :
  - Log item RTP queue delay modified to make it possible to add network queue delay and RTP queue delay to see the actual extra delay for transmitted RTP packets.
  - SCReAM BW test tool (SCReAMv2 only): Added openWindow option
- 2024-04-17 :
  - ScreamV2Tx :  openWindow option did not function properly, fixed. Reaction to sudden drop in throughput improved.
  - RtpQueue :  code after return statement fixed.
- 2024-04-03 :  
  - Multicam scream_sender.cpp : multiplicativeIncreaseFactor was erroneously set to 1.0, replaced with a much more correct value 0.05. 
  - ScreamV2Tx : targetBitrate and RTP queue delay added to extra detailed log (-log option).
- 2024-03-27 :  
  - Stability issue at very low RTTs fixed.
- 2024-03-22 :  
  - Added method setIsSlowEncoder to increase robustness when video encoders react slowly to updated target bitrates.
- 2024-03-11 :  
  - Added support for IPv6 in SCReAM BW test application.
  - Added counter for Not-ECT, ECT(0), ECT(1) and CE in summary printout.
- 2024-02-21 :
  - Added averaging to transmit and rtp rate logs.
- 2024-01-24 :
  - Robustness to sudden jumps in sender or receiver clocks improved.
- 2024-01-19 :
  - Stream prioritization refined.
  - SCReAM RFC8298 update. First draft available [RFC8298-bis](https://github.com/IngJohEricsson/draft-johansson-ccwg-scream-bis "RFC8298-bin")
  - SCReAM V2 is made default for gstreamer application.
- 2024-01-10 :
  - SCReAM V2 is made default for BW test and multicam application.
- 2023-12-07 :
  - General : Added CE marking percentage to statistics, added function to get statistics items. RTCP format error fixed.
  - SCReAM V2 : Delay based CC modified for more stable bitrate. Frame size histogram added to handle large frame size variation better
- 2023-11-12 : SCReAM V2 update. Conditional packet pacing is changed to always pacing when packet pacing is enabled. This removes an odd on/off effect in the packet pacing. Packet pacing implementation is updated to allow for micro burst intervals down to 0.2ms
- 2023-11-03 : SCReAM V2 update. The delay based part of SCReAM V2 is modified such that a virtual L4S alpha is computed when L4S is either disabled or inactive. The virtal L4S alpha is calculated based on the estimated queue delay. The resulting algorithm now abandoned most the LEDBAT style approach that was outlined in RFC8298. Some additional previous voodoo magic is removed in the process.
- 2023-09-20 : SCReAM V2. Version 2 is a major rewrite of the complete algorithm with the goal to make the algorithm more stable, especially when used with L4S. Support for V2 is in the BW test algorithm application and the multicam code. SCReAM V2 is enabled by adding -DV2 in the CMAKE_CXX_FLAGS in CMakeLists.txt  <br> The main changes are:
  - The congestion window serves mainly as a hand brake to avoid that excessive amounts of data is injected to the network when link thorughput drops dramatically. The congestion window is otherwise seldom a limiting factor in more normal working conditions
  - The packet pacing headroom is made large, as a default, the pacing rate is 50% larger than the nominal target rate. The congestion down-scale is adapted to this to still achieve high link ultilization
  - The rate control algorith is greatly simplified, with a minimal amount of voodoo magic that is difficult to explain
  - The fast increase mode is replaced with a multiplicative increase that sets in fully a configurable time after congestion
  - The final algorithm closely follows the 2 CE marks per RTT rule when used with L4S.        

## What is SCReAM
SCReAM (**S**elf-**C**locked **R**at**e** **A**daptation for **M**ultimedia) is a congestion control algorithm devised mainly for Video.
Congestion control for WebRTC media is currently being standardized in the IETF RMCAT WG, the scope of the working group is to define requirements for congestion control and also to standardize a few candidate solutions.
SCReAM is a congestion control candidate solution for WebRTC developed at Ericsson Research and optimized for good performance in wireless access.  

The algorithm is an IETF experimental standard [1], a Sigcomm paper [2] and [3] explains the rationale behind the design of the algorithm in more detail. Because SCReAM as most other congestion control algorithms are continously improved over time, the current implementation available here has deviated from what is described in the papers and IETF RFC. The most important new development is addition of L4S support. In addition the algorithm has been modified to become more stable.

As mentioned above, SCReAM was originally devised for WebRTC but did not make it into being incorporated into that platform. Instead, SCReAM has found use as congestion control for remote controlled vehicles, cloud gaming demos and benchmarking of 5G networks with and without L4S support.

## What is L4S ?
L4S is short for **L**ow **L**atency **L**ow **L**oss **S**calable thorughput, L4S is specified in [4]. A network node that is L4S capable can remark packets that have the ECT(1) code point set to CE. The marking threshold is set very low (milliseconds).

A sender that is L4S capable sets the ECT(1) code point on outgoing packets. If CE packets are detected, then the sender should reduce the transmission rate in proportion to the amount of packets that are marked. A document that highlights how L4S improves performance for low latency applications is found in [https://github.com/EricssonResearch/scream/blob/master/L4S-Results.pdf](https://github.com/EricssonResearch/scream/blob/master/L4S-Results.pdf?raw=true)

In steady state, 2 packets per RTT should be marked. The expected rate then becomes <br> rate = (2.0/pMark) * MSS * 8/RTT [bps]    
How SCReAM (V2) manages this is illustrated in the figure below ![SCReAM V2 mark probability vs bitrate, RTT=25ms, 1360byte packets](https://github.com/EricssonResearch/scream/blob/master/images/SCReAM-V2-RTT-25ms-1360B.png)  
Figure 1 : SCReAM V2 bitrate as function of packet marking probability. RTT = 25ms, MSS=1360B. Dotted is theoretical, blue is actual


### The more nitty gritty details
Unlike many other congestion control algorithms that are rate based i.e. they estimate the network throughput and adjust the media bitrate accordingly, SCReAM is self-clocked which essentially means that the algorithm does not send in more data into a network than what actually exits the network.

To achieve this, SCReAM implements a feedback protocol over RTCP that acknowledges received RTP packets.
A congestion window is determined from the feedback, this congestion window determines how many RTP packets that can be in flight i.e. transmitted by not yet acknowledged, an RTP queue is maintained at the sender side to temporarily store the RTP packets pending transmission, this RTP queue is mostly empty but can temporarily become larger when the link throughput decreases.
The congestion window is frequently adjusted for minimal e2e delay while still maintaining as high link utilization as possible. The use of self-clocking in SCReAM which is also the main principle in TCP has proven to work particularly well in wireless scenarios where the link throughput may change rapidly. This enables a congestion control which is robust to channel jitter, introduced by e.g. radio resource scheduling while still being able to respond promptly to reduced link throughput.
SCReAM is optimized in house in a state of the art LTE system simulator for optimal performance in deployments where the LTE radio conditions are limiting. In addition, SCReAM is also optimized for good performance in simple bottleneck case such as those given in home gateway deployments. SCReAM is verified in simulator and in a testbed to operate in a rate range from a couple of 100kbps up to well over 100Mbps.
The fact that SCReAM maintains a RTP queue on the sender side opens up for further optimizations to congestion, for instance it is possible to discard the contents of the RTP queue and replace with an I frame in order to refresh the video quickly at congestion.

### SCReAM performance and behavior
SCReAM has been evaluated in a number of experiments over the years. Some of these are exemplified below.

A comparison against GCC (Google Congestion Control) is shown in [5]. Final presentations are found in [6] and [7].
A short [video](https://www.youtube.com/watch?v=_jBFu-Y0wwo) exemplifies the use of SCReAM in a small vehicle, remote controlled over a public LTE network. [8] explains the rationale behind the use of SCReAM in remote controlled applications over LTE/5G.

#### ECN (Explicit Congestion Notification)
SCReAM supports "classic" ECN, i.e. that the sending rate is reduced as a result of one or more ECN marked RTP packets in one RTT, similar to the guidelines in RFC3168. .

In addition SCReAM also supports L4S, i.e that the sending rate is reduced proportional to the fraction of the RTP packets that are ECN-CE marked. This enables lower network queue delay.  

Below is shown two simulation examples with a simple 50Mbps bottleneck that changes to 25Mbps between 50 and 70s, the min RTT is 25ms. The video trace is from a video encoder.

L4S gives a somewhat lower media rate, the reason is that a larger headroom is added to ensure the low delay, considering the varying output rate of the video encoder. This is self-adjusting by inherent design because the larger frames hit the L4S enabled queue more and thus causes more marking. The average bitrate would increase if the frame size variations are smaller.

![Simple bottleneck simulation SCReAM no L4S support](https://github.com/EricssonResearch/scream/blob/master/images/SCReAM-V2-noL4S.png)
Figure 2 : SCReAM V2 without L4S support

![Simple bottleneck simulation SCReAM with L4S support](https://github.com/EricssonResearch/scream/blob/master/images/SCReAM-V2-L4S.png)
Figure 3 : SCReAM with L4S support. L4S ramp-marker (Th_low=2ms, Th_high=10ms)

----------

Below are a few older examples that show how SCReAM performs

The two videos below show a simple test with a simple 3Mbps bottleneck (CoDel AQM, ECN cabable). The first video is with ECN disabled in the sender, the other is with ECN enabled. SCReAM is here used with a Panasonic WV-SBV111M IP camera. One may argue that one can disable CoDel to avoid the packet losses, unfortunately one then lose the positive properties with CoDel, mentioned earlier.

[Without ECN](https://www.youtube.com/watch?v=J0po78q1QkU "Without ECN")

[With ECN](https://www.youtube.com/watch?v=qIe0ubw9jPw "With ECN")

The green areas that occur due to packet loss is an artifact in the conversion of the RTP dump.
#### Real life test
A real life test of SCReAM is performed with the following setup in a car:

- Sony Camcorder placed on dashboard, HDMI output used
- Antrica ANT-35000A video encoder with 1000-8000kbps encoding range and 1080p50 mode
- Laptop with a SCReAM sender running
- Sony Xperia phone in WiFi tethering mode

A SCReAM receiver that logged the performance and stored the received RTP packets was running in an office. The video traffic was thus transmitted in LTE uplink.The video was played out to file with GStreamer, the jitter buffer was disabled to allow for the visibility of the delay jitter artifacts,

Below is a graph that shows the bitrate, the congestion window and the queue delay.

![Log from ](https://github.com/EricssonResearch/scream/blob/master/images/SCReAM_LTE_UL.png)

Figure 5 : Trace from live drive test

The graph shows that SCReAM manages high bitrate video streaming with low e2e delay despite demanding conditions both in terms of variable throughput and in a changing output bitrate from the video encoder. Packet losses occur relatively frequently, the exact reason is unknown but seem to be related to handover events, normally packet loss should not occure in LTE-UL, however this seems to be the case with the used cellphone.
The delay increases between 1730 and 1800s, the reason here is that the available throughput was lower than the lowest possible coder bitrate. An encoder with a wider rate range would be able to make it possible to keep the delay low also in this case.

A video from the experiment is found at the link below. The artifacts and overall video quality can be correlated aginst the graph above.

Link to video : [SCReAM live demo](https://youtu.be/YYaox26WhKo "SCReAM Live demo")

SCReAM is also implemented in a remote controlled car prototype. The two videos below show how it works in different situations

- [Boliden Kankberg mine](https://www.youtube.com/watch?v=r7QxdTP3jB0 "Boliden Kankberg mine")
- [Winter wonderland](https://www.youtube.com/watch?v=eU1crtEvMv4 "Winter wonderland")

SCReAM has been successfully be used on more recent experiments, examples will be added later.

## Build
The SCReAM code comes in two (three) applications

- Windows based test application : This application implements a simple bottleneck and does only local simulation. Open the scream.sln application in Visual studio and build.
- Linux based BW test application :  Makes in possible to benchmark the throughput live networks and test beds. The tool models a video encoder. See https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx for further instructions.
- multicam version :  See ./multicam/README.md for details.
- gstreamer plugin :  See ./gstscream/README.md for details.

### The code
The main SCReAM algorithm components are found in the C++ classes:

- ScreamTx : SCReAM sender algorithm
  - ScreamV1Tx : Older version
  - ScreamV2Tx, ScreamV2Stream : Version

- ScreamRx : SCReAM receiver algorithm

- RtpQueue : Rudimentary RTP packet queue

A few support classes for experimental use are implemented in:

- VideoEnc : A very simple model of a Video encoder


- NetQueue : Simple delay and bandwidth limitation

For more information on how to use the code in multimedia clients or in experimental platforms, please see [https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx](https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx?raw=true)

### Feedback format
The feedback format is according to [9]. The feedback interval depends heavily on the media bitrate.

### Build SCReAM BW test application
The SCReAM BW test application runs on e.g Ubuntu 16.04 and later. The build steps are:

```
cmake .
make
```

You need git, cmake, make and g++ installed

To enable SCReAM V2, change SET(CMAKE_CXX_FLAGS "-fPIC -fpermissive -pthread") to SET(CMAKE_CXX_FLAGS "-fPIC -fpermissive -pthread -DV2") in CMakeLists.txt


# References
[1] https://tools.ietf.org/html/rfc8298

[2] Sigcomm paper http://dl.acm.org/citation.cfm?id=2631976

[3] Sigcomm presentation http://conferences.sigcomm.org/sigcomm/2014/doc/slides/150.pdf

[4] https://tools.ietf.org/html/rfc9331

[5] IETF RMCAT presentation, comparison against Google Congestion Control (GCC) http://www.ietf.org/proceedings/90/slides/slides-90-rmcat-3.pdf

[6] IETF RMCAT presentation (final for WGLC) : https://www.ietf.org/proceedings/96/slides/slides-96-rmcat-0.pdf

[7] IETF RMCAT presention , SCReAM for remote controlled vehicles over 4G/5G : https://datatracker.ietf.org/meeting/100/materials/slides-100-rmcat-scream-experiments

[8] Adaptive Video with SCReAM over LTE for Remote-Operated Working Machines : https://www.hindawi.com/journals/wcmc/2018/3142496/

[9] https://tools.ietf.org/html/rfc8888
