# AstaZero Additions
This fork adds a devcontainer and Dockerfile to build and run the SCReAM code in a containerized environment.

# How to use the docker image
1. **Build the Docker image**:

    ```bash
    docker build -t git.ri.se:4567/astazero/az-scream -f .devcontainer/Dockerfile  .
    ```

2. **Run the Docker container**:

    ```bash
    docker run --rm -it --mount type=bind,src=/dev,dst=/dev -p 30112:30112 -p 30113:30113 --privileged  --network host -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix scream-runtime
    ```
    You might need to allow docker to access xhost to run the receiver scripts. 

    ```bash
    xhost +local:docker
    ```

3. **Modify the entrypoint** 
  The image runs sender.sh by default with the parameters from az-scream/gstscream/scripts/env.sh and sender.sh. 
  Its likely that you wish to modify these scripts and you can override the files and entrypoint command like this
    ```bash
    docker run --rm -it --mount type=bind,src=/dev,dst=/dev -p 30112:30112 -p 30113:30113 --privileged --network host \
    -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v $PWD/gstscream/scripts:/app/scripts --entrypoint "receiver.sh" \
    scream-runtime
    ```

    There are several scripts that you can use. They have been created in pairs.
    sender.sh has a corresponding receiver.sh and sender_3.sh has a receiver_3.sh and so forth.



# SCReAM
This project includes an implementation of SCReAM, a mobile optimised congestion control algorithm for realtime interactive media.

## News
- 2025-05-09 : 
  - Added new user guide for the SCReAM BW test tool with examples
  - SCReAM BW test, added end of session summary 
- 2025-04-17 :
  - Added option -txrxlog that logs time, sequence_number, tx_time, rx_time, rx_time-tx_time for each RTP packet.   
- 2025-01-29 :
  - -relaxedpacing option added. Enables increased pacing rate when max rate reached
  - -postcongestiondelay option removed, replaced with a constant
  - -openwindow option replaced with windowheadroom option
  - More conservative CWND increase when max rate reached
  - Bytes inflight restriction to target rate enabled only when queue detected
  - Max feedback interval set to 10ms (was 5ms) 
  - Release dates for SCReAM BW test changed 

  
Older version history is found here https://github.com/EricssonResearch/scream/blob/master/version-history.md 
## What is SCReAM
SCReAM (**S**elf-**C**locked **R**at**e** **A**daptation for **M**ultimedia) is a congestion control algorithm devised mainly for Video.
Congestion control for WebRTC media is currently being standardized in the IETF RMCAT WG, the scope of the working group was to define requirements for congestion control and also to standardize a few candidate solutions.
SCReAM is a congestion control candidate solution for WebRTC developed at Ericsson Research and optimized for good performance in wireless access.  

The algorithm is an IETF experimental standard [1], a Sigcomm paper [2] and [3] explains the rationale behind the design of the algorithm in more detail. Because SCReAM as most other congestion control algorithms are continously improved over time, the current implementation available here has deviated from what is described in the papers and IETF RFC. The most important new development is addition of L4S support. In addition the algorithm has been modified to become more stable.

As mentioned above, SCReAM was originally devised for WebRTC but is sofar not incorporated into that platform. Instead, SCReAM has found use as congestion control for remote controlled vehicles, cloud gaming demos and benchmarking of 5G networks with and without L4S support.

Since standardization in RFC8298, SCReAM has undergone changes and a new V2 is described in https://datatracker.ietf.org/doc/draft-johansson-ccwg-rfc8298bis-screamv2/

Test report(s) for SCReAM V2 is found here https://github.com/EricssonResearch/scream/blob/master/test-record.md 

A test report at CableLabs L4S interop test in november 2024 shows that SCReAM V2 works fine when subject to competing flows over the same bottleneck. 
https://github.com/EricssonResearch/scream/blob/master/CableLabs-L4S-interop-nov-2024-Ericsson.pdf 

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


A short [video](https://www.youtube.com/watch?v=_jBFu-Y0wwo) exemplifies the use of SCReAM in a small vehicle, remote controlled over a public LTE network. [8] explains the rationale behind the use of SCReAM in remote controlled applications over LTE/5G.

#### ECN (Explicit Congestion Notification)
SCReAM supports "classic" ECN, i.e. that the sending rate is reduced as a result of one or more ECN marked RTP packets in one RTT, similar to the guidelines in RFC3168. .

In addition SCReAM also supports L4S, i.e that the sending rate is reduced proportional to the fraction of the RTP packets that are ECN-CE marked. This enables lower network queue delay.  

Below is shown two simulation examples with a simple 50Mbps 25ms. The video trace is from a video encoder.

L4S gives a somewhat lower media rate, the reason is that a larger headroom is added to ensure the low delay, considering the varying output rate of the video encoder. This is self-adjusting by inherent design because the larger frames hit the L4S enabled queue more and thus causes more marking. The average bitrate would increase if the frame size variations are smaller.

![Simple bottleneck simulation SCReAM no L4S support](https://github.com/EricssonResearch/scream/blob/master/images/SCReAM-V2-noL4S.png)
Figure 2 : SCReAM V2 without L4S support

![Simple bottleneck simulation SCReAM with L4S support](https://github.com/EricssonResearch/scream/blob/master/images/SCReAM-V2-L4S.png)
Figure 3 : SCReAM V2 with L4S support. L4S ramp-marker (Th_low=2ms, Th_high=10ms)

Another example with the SCreAM BW test tool over a 50Mbps, 25ms bottleneck with DualPi2 AQM 
![SCReAM V2 no L4S support](https://github.com/EricssonResearch/scream/blob/master/images/SCReAM-DualPi2-50Mbps-25ms-noL4S.png)
Figure 4 : SCReAM V2 without L4S support, 50Mbps, 25ms bottleneck with DualPi2 AQM 

![SCReAM V2 L4S support](https://github.com/EricssonResearch/scream/blob/master/images/SCReAM-DualPi2-50Mbps-25ms-L4S.png)
Figure 5 : SCReAM V2 with L4S support, 50Mbps, 25ms bottleneck with DualPi2 AQM 

SCReAM is also implemented in a remote controlled car prototype. The two videos below show how it works in different situations
- [Boliden Kankberg mine](https://www.youtube.com/watch?v=r7QxdTP3jB0 "Boliden Kankberg mine")
- [Winter wonderland](https://www.youtube.com/watch?v=eU1crtEvMv4 "Winter wonderland")

SCReAM V2 was also implemented for MWC 2023 demo in collaboration between Ericsson, DT and Vay. https://vay.io/press-release/mwc-ericsson-deutsche-telekom-and-vay-show-live-teledrive-technology-demo-with-5g/

----------

A older comparison against GCC (Google Congestion Control) is shown in [5]. Final presentations are found in [6] and [7].that show how SCReAM performs


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
  - ScreamV1Tx : Older version, removed
  - ScreamV2Tx, ScreamV2Stream : Version

- ScreamRx : SCReAM receiver algorithm

- RtpQueue : Rudimentary RTP packet queue

A few support classes for experimental use are implemented in:

- VideoEnc : A very simple model of a Video encoder


- NetQueue : Simple delay and bandwidth limitation

For more information on how to use the code in multimedia clients or in experimental platforms, please see [https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx](https://github.com/EricssonResearch/scream/blob/master/SCReAM-description.pptx?raw=true)

### Feedback format
The feedback format is according to [9]. The feedback interval depends heavily on the media bitrate.

### Build SCReAM BW test tool
The SCReAM BW test application runs on e.g Ubuntu 16.04 and later. The build steps are:

```
cmake .
make
```

You need git, cmake, make and g++ installed

A SCReAM BW test tool user guide is found at [https://github.com/EricssonResearch/scream/blob/master/SCReAM-BW-test-tool.docx](https://github.com/EricssonResearch/scream/blob/master/SCReAM-BW-test-tool.docx?raw=true)


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
