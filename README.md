# SCReAM
A mobile optimised congestion control algorithm

## Algorithm
SCReAM (**S**elf-**C**locked **R**at**e** **A**daptation for **M**ultimedia) is a congestion control algorithm devised mainly for Video.
Congestion control for WebRTC media is currently being standardized in the IETF RMCAT WG, the scope of the working group is to define requirements for congestion control and also to standardize a few candidate solutions. 
SCReAM is a congestion control candidate solution for WebRTC developed at Ericsson Research and optimized for good performance in wireless access. 

The algorithm is submitted to the RMCAT WG [1], a Sigcomm paper [2] and [3] explains the rationale behind the design of the algorithm in more detail. A comparison against GCC (Google Congestion Control) is shown in [4].
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

## The code
The main SCReAM algorithm components are found in the C++ classes:


- ScreamTx : SCReAM sender algorithm


- ScreamRx : SCReAM receiver algorithm

A few support classes for experimental use are implemented in:


- RtpQueue : Rudimentary RTP packet queue


- VideoEnc : A very simple model of a Video encoder


- NetQueue : Simple delay and bandwidth limitation

For more information on how to use the experimental code, please see [https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pdf](https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pdf?raw=true) 


## References
[1] http://tools.ietf.org/wg/rmcat/draft-ietf-rmcat-scream-cc

[2] Sigcomm paper http://dl.acm.org/citation.cfm?id=2631976 

[3] Sigcomm presentation http://conferences.sigcomm.org/sigcomm/2014/doc/slides/150.pdf

[4] IETF RMCAT presentation, comparison against Google Congestion Control (GCC) http://www.ietf.org/proceedings/90/slides/slides-90-rmcat-3.pdf 
