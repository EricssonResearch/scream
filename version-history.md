# Older version history
- 2024-10-03 :
  - Features that more cautiously increases CWND are removed/refined as these deviated too much from the principle of two marked packets per RTT at steady state.
- 2024-09-27 :
  - Limit growth on very small CWND added (optional), to make algorithm more stable at very low bitrates when CWND is just a few MSS  
- 2024-07-05 :
  - Robustness to large packet reordering improved. Feature is currently only enabled for the BW test application. To enable featire in multicam, add #define EXT_OOO_FB in multicam scream_receiver.cpp. Updated code also fixed an issue where even little reordering triggered packet loss reaction.
  - -periodicdropinterval option removed. Periodic rate reduction is instead implemented in ScreamV2Tx.cpp. 
  - ScreamV1Tx.cpp removed. 
  - BW test tool, summary output displays %-age of packets marked Not-ECT, ECT(0), ECT(1) and CE.
- 2024-05-31 :
  - Congestion window validation modified
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