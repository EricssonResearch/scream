# SCReAM
This project includes an implementation of SCReAM, a mobile optimised congestion control algorithm for realtime interactive media.

## Algorithm
SCReAM (**S**elf-**C**locked **R**at**e** **A**daptation for **M**ultimedia) is a congestion control algorithm devised mainly for Video. 
Congestion control for WebRTC media is currently being standardized in the IETF RMCAT WG, the scope of the working group is to define requirements for congestion control and also to standardize a few candidate solutions. 
SCReAM is a congestion control candidate solution for WebRTC developed at Ericsson Research and optimized for good performance in wireless access.  

The algorithm is an IETF experimental standard [1], a Sigcomm paper [2] and [3] explains the rationale behind the design of the algorithm in more detail. A comparison against GCC (Google Congestion Control) is shown in [4]. Final presentations are found in [5] and [6]. A short [video](https://www.youtube.com/watch?v=_jBFu-Y0wwo) exemplifies the use of SCReAM in a small vehicle, remote controlled over a public LTE network. [7] explains the rationale behind the use of SCReAM in remote controlled applications over LTE/5G.

Unlike many other congestion control algorithms that are rate based i.e. they estimate the network throughput and adjust the media bitrate accordingly, SCReAM is self-clocked which essentially means that the algorithm does not send in more data into a network than what actually exits the network.

To achieve this, SCReAM implements a feedback protocol over RTCP that acknowledges received RTP packets. 
A congestion window is determined from the feedback, this congestion window determines how many RTP packets that can be in flight i.e. transmitted by not yet acknowledged, an RTP queue is maintained at the sender side to temporarily store the RTP packets pending transmission, this RTP queue is mostly empty but can temporarily become larger when the link throughput decreases. 
The congestion window is frequently adjusted for minimal e2e delay while still maintaining as high link utilization as possible. The use of self-clocking in SCReAM which is also the main principle in TCP has proven to work particularly well in wireless scenarios where the link throughput may change rapidly. This enables a congestion control which is robust to channel jitter, introduced by e.g. radio resource scheduling while still being able to respond promptly to reduced link throughput. 
SCReAM is optimized in house in a state of the art LTE system simulator for optimal performance in deployments where the LTE radio conditions are limiting. In addition, SCReAM is also optimized for good performance in simple bottleneck case such as those given in home gateway deployments. SCReAM is verified in simulator and in a testbed to operate in a rate range from ~20kbps up to 100Mbps.
The fact that SCReAM maintains a RTP queue on the sender side opens up for further optimizations to congestion, for instance it is possible to discard the contents of the RTP queue and replace with an I frame in order to refresh the video quickly at congestion.

Below is shown an example of SCReAM congestion control when subject to a bottleneck with varying bandwidth.

![Simple bottleneck simulation SCReAM](https://github.com/EricssonResearch/scream/blob/master/images/image_2.png)

Figure 1 : Simple bottleneck simulation SCReAM

## ECN (Explicit Congestion Notification) 
SCReAM supports "classic" ECN, i.e. that the sending rate is reduced as a result of one or more ECN marked RTP packets in one RTT, similar to the guidelines in RFC3168. . 

In addition SCReAM also supports L4S, i.e that the sending rate is reduced proportional to the fraction of the RTP packets that are ECN marked. This enables lower network queue delay.  

Below is shown three simulation examples with a simple 50Mbps bottleneck that changes to 25Mbps after 50s, the min RTT is 20ms. The video trace is from NVENC.

The graphs show that ECN improves on the e2e delay and that the use of L4S reduces the delay considerably more. 

L4S gives a somewhat lower media rate, the reason is that a larger headroom is added to ensure the low delay, considering the varying output rate of the video encoder. This is self-adjusting by inherent design, the average bitrate would increase if the frame size variations are smaller.

Note carefully that the scales in the graphs differ, especially the delay graph is zoomed in for the L4S alternative to illustrate the very low queue delay. It is obvious that L4S gives a much lower queue delay.

![Simple bottleneck simulation SCReAM no ECN support](https://github.com/EricssonResearch/scream/blob/master/images/scream_noecn_2.png)

Figure 2 : SCReAM without ECN support
    
![Simple bottleneck simulation SCReAM with ECN support](https://github.com/EricssonResearch/scream/blob/master/images/scream_ecn_2.png)

Figure 3 : SCReAM with ECN support ECN beta = 0.8. CoDel with default settings (5ms, 100ms)  

![Simple bottleneck simulation SCReAM with L4S support](https://github.com/EricssonResearch/scream/blob/master/images/scream_l4s_2.png)

Figure 4 : SCReAM with L4S support. L4S ramp-marker (Th_low=2ms, Th_high=10ms)

----------

The two videos below show a simple test with a simple 3Mbps bottleneck (CoDel AQM, ECN cabable). The first video is with ECN disabled in the sender, the other is with ECN enabled. SCReAM is here used with a Panasonic WV-SBV111M IP camera. One may argue that one can disable CoDel to avoid the packet losses, unfortunately one then lose the positive properties with CoDel, mentioned earlier.

[Without ECN](https://www.youtube.com/watch?v=J0po78q1QkU "Without ECN") 

[With ECN](https://www.youtube.com/watch?v=qIe0ubw9jPw "With ECN") 

The green areas that uccur due to packet loss is an artifact in the conversion of the RTP dump.
## Real life test
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

## The code
The main SCReAM algorithm components are found in the C++ classes:


- ScreamTx : SCReAM sender algorithm


- ScreamRx : SCReAM receiver algorithm

A few support classes for experimental use are implemented in:


- RtpQueue : Rudimentary RTP packet queue


- VideoEnc : A very simple model of a Video encoder


- NetQueue : Simple delay and bandwidth limitation

For more information on how to use the code in multimedia clients or in experimental platforms, please see [https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx](https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx?raw=true) 


## References
[1] https://tools.ietf.org/html/rfc8298

[2] Sigcomm paper http://dl.acm.org/citation.cfm?id=2631976 

[3] Sigcomm presentation http://conferences.sigcomm.org/sigcomm/2014/doc/slides/150.pdf

[4] IETF RMCAT presentation, comparison against Google Congestion Control (GCC) http://www.ietf.org/proceedings/90/slides/slides-90-rmcat-3.pdf 

[5] IETF RMCAT presentation (final for WGLC) : https://www.ietf.org/proceedings/96/slides/slides-96-rmcat-0.pdf

[6] IETF RMCAT presention , SCReAM for remote controlled vehicles over 4G/5G : https://datatracker.ietf.org/meeting/100/materials/slides-100-rmcat-scream-experiments

[7] Adaptive Video with SCReAM over LTE for Remote-Operated Working Machines : https://www.hindawi.com/journals/wcmc/2018/3142496/

## Feedback format
The feedback format is according to [https://tools.ietf.org/html/draft-ietf-avtcore-cc-feedback-message-02](https://tools.ietf.org/html/draft-ietf-avtcore-cc-feedback-message-02 "IETF feedback format"), a previous proprietary format is deprecated.
The feedback overhead depends on the media bitrate. The table below shows the IP+UDP+RTCP bitrate at varuious media bitrates.


       +-----------------------------------------------------+
       | Media bitrate [kbps] | RTCP feedback bitrate [kbps] |
       +-----------------------------------------------------+
       |                   50 |                           15 |
       |                  100 |                           15 |
       |                  500 |                           30 |
       |                 1000 |                           60 |
       |                10000 |                          300 |
       |                30000 |                          500 |
       +-----------------------------------------------------+

## Build
The SCReAM code comes in two (three) applications

- Windows based test application : This application implements a simple bottleneck and does only local simulation. Open the scream.sln application in Visual studio and build.
- Linux based BW test application :  Makes in possible to benchmark the throughput live networks and test beds. The tool models a video encoder. See https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx for further instructions.
- gstreamer plugin : This application is kept in ./code/gscream . It is however currently not maintained and may not work properly

### Build SCReAM BW test application

The SCReAM BW test application runs on e.g Ubuntu 16.04 and later. The build steps are:

```
cmake .
make
```

You need git, cmake, make and g++ installed  