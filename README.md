# SCReAM
This project includes an implementation of SCReAM, a mobile optimised congestion control algorithm for realtime interactive media.

## Algorithm
SCReAM (**S**elf-**C**locked **R**at**e** **A**daptation for **M**ultimedia) is a congestion control algorithm devised mainly for Video. 
Congestion control for WebRTC media is currently being standardized in the IETF RMCAT WG, the scope of the working group is to define requirements for congestion control and also to standardize a few candidate solutions. 
SCReAM is a congestion control candidate solution for WebRTC developed at Ericsson Research and optimized for good performance in wireless access. 

The algorithm is submitted to the RMCAT WG [1], a Sigcomm paper [2] and [3] explains the rationale behind the design of the algorithm in more detail. A comparison against GCC (Google Congestion Control) is shown in [4]. A final presentation is found in [5].
Unlike many other congestion control algorithms that are rate based i.e. they estimate the network throughput and adjust the media bitrate accordingly, SCReAM is self-clocked which essentially means that the algorithm does not send in more data into a network than what actually exits the network.

To achieve this, SCReAM implements a feedback protocol over RTCP that acknowledges received RTP packets. 
A congestion window is determined from the feedback, this congestion window determines how many RTP packets that can be in flight i.e. transmitted by not yet acknowledged, an RTP queue is maintained at the sender side to temporarily store the RTP packets pending transmission, this RTP queue is mostly empty but can temporarily become larger when the link throughput decreases. 
The congestion window is frequently adjusted for minimal e2e delay while still maintaining as high link utilization as possible. The use of self-clocking in SCReAM which is also the main principle in TCP has proven to work particularly well in wireless scenarios where the link throughput may change rapidly. This enables a congestion control which is robust to channel jitter, introduced by e.g. radio resource scheduling while still being able to respond promptly to reduced link throughput. 
SCReAM is optimized in house in a state of the art LTE system simulator for optimal performance in deployments where the LTE radio conditions are limiting. In addition, SCReAM is also optimized for good performance in simple bottleneck case such as those given in home gateway deployments.

A comparison against GCC (**G**oogle **C**ongestion **C**ontrol) is shown below. Notice in particular that SCReAM utilizes free bandwidth much more efficiently. Still the reaction to reduced throughput is more prompt. SCReAMs larger video frame delay is solely because the video bitrate is higher when the throughput drops. The IP packet delay is however considerably lower, something that is highly beneficial for other flows such audio as the latter will then experience less disturbances. The fact that SCReAM maintains a RTP queue on the sender side opens up for further optimizations to congestion, for instance it is possible to discard the contents of the RTP queue and replace with an I frame in order to refresh the video quickly at congestion.

![Simple bottleneck simulation GCC](https://github.com/EricssonResearch/scream/blob/master/images/image_1.png)

Figure 1 : Simple bottleneck simulation GCC, the link bandwidth is outlined in red 

![Simple bottleneck simulation SCReAM](https://github.com/EricssonResearch/scream/blob/master/images/image_2.png)

Figure 2 : Simple bottleneck simulation SCReAM

## Real life test
A real life test of SCReAM is performed with the following setup in a car:

- Sony Camcorder placed on dashboard, HDMI output used
- Antrica ANT-35000A video encoder with 1000-8000kbps encoding range and 1080p50 mode
- Laptop with a SCReAM sender running
- Sony Xperia phone in WiFi tethering mode 

A SCReAM receiver that logged the performance and stored the received RTP packets was running in an office. The video traffic was thus transmitted in LTE uplink.The video was played out to file with GStreamer, the jitter buffer was disabled to allow for the visibility of the delay jitter artifacts,

Below is a graph that shows the bitrate, the congestion window and the queue delay. 
 
![Log from ](https://github.com/EricssonResearch/scream/blob/master/images/SCReAM_LTE_UL.png)

The graph shows that SCReAM manages high bitrate video streaming with low e2e delay despite demanding conditions both in terms of variable throughput and in a changing output bitrate from the video encoder. Packet losses occur relatively frequently, the exact reason is unknown but seem to be related to handover events, normally packet loss should not occure in LTE-UL, however this seems to be the case with the used cellphone. 
The delay increases between 1730 and 1800s, the reason here is that the available throughput was lower than the lowest possible coder bitrate. An encoder with a wider rate range would be able to make it possible to keep the delay low also in this case.

A video from the experiment is found at the link below. The artifacts and overall video quality can be correlated aginst the graph above.

Link to video : [SCReAM live demo](https://youtu.be/YYaox26WhKo "SCReAM Live demo")

## The code
The main SCReAM algorithm components are found in the C++ classes:


- ScreamTx : SCReAM sender algorithm


- ScreamRx : SCReAM receiver algorithm

A few support classes for experimental use are implemented in:


- RtpQueue : Rudimentary RTP packet queue


- VideoEnc : A very simple model of a Video encoder


- NetQueue : Simple delay and bandwidth limitation

For more information on how to use the code in multimedia clients or in experimental platforms, please see [https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pdf](https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pdf?raw=true) 


## References
[1] http://tools.ietf.org/wg/rmcat/draft-ietf-rmcat-scream-cc

[2] Sigcomm paper http://dl.acm.org/citation.cfm?id=2631976 

[3] Sigcomm presentation http://conferences.sigcomm.org/sigcomm/2014/doc/slides/150.pdf

[4] IETF RMCAT presentation, comparison against Google Congestion Control (GCC) http://www.ietf.org/proceedings/90/slides/slides-90-rmcat-3.pdf 

[5] IETF RMCAT presentation (final for WGLC) : https://www.ietf.org/proceedings/96/slides/slides-96-rmcat-0.pdf

## Feedback format
It is recommended that the experimental feedback format below is used with SCReAM until a dedicated feedback packet format is devised. 

The format is based on RTCP XR (RFC3611) and use the (reserved for future use) format tag = 255.
The feedback elements are:

- Highest received sequence number (16b): Indicates the highest (possibly wrapped around) sequence number for the given source.
- n_ECN (8b): Accumulated number of received RTP packets for teh given source that have the ECN-CE code point set.
- Q (1b): The fraction of RTCP feedback packets for the given source with this bit set, dictates how much the SCReAM sender should reduce the sending rate
- ACK vector (64b): Indicates successful receipt indication of the last 64 RTP packets, preceeding the RTP packet with the highest RTP sequence number.
- Timestamp (32b): Indicates the (wallclock) receive time (in milliseconds) when the RTP packet with the highest sequence number was received, truncated to 32bits.    

Handling of n_ECN and Q bits is currently not implemented in the SCReAM code.  

The feedback format is 28bytes and gives a reasonably low RTCP overhead that makes it possible to use SCReAM also for low bitrate applications.
The RTCP feedback interval calculation in the code gives av feedback interval between 400ms, at low media bitrates, and 20ms at high media bitrates 
Given an IP+UDP overhead of 20+8 bytes and with the used of reduced size RTCP (RFC5506), the RTCP feedback overhead at low media rates then become 
RTCP_bw = 8*(28+28)/0.400 = 1.1kbps
which should be acceptably low even at low media bitrates. 



        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |V=2|P|reserved |   PT=XR=207   |           length=6            |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                              SSRC                             |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |     BT=255    |    reserved   |         block length=4        |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                        SSRC of source                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       | Highest recv. seq. nr. (16b)  |   n_ECN       |Q|  reserved   |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                     Ack vector (b0-31)                        |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                     Ack vector (b32-63)                       |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                    Timestamp (32bits)                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

