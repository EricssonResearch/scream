# Receiver side build instructions

On the receiver side it is assumed that gstreamer is already installed.

The SCReAM receiver side is built with the instructions<br>
`$cd ./scream`<br>
`$cmake .`<br>
`$make`


#Startup

In the terminal:<br>
`$ ./startvideo.sh`<br>

To ensure proper function it is recommended to start the sender side first, then the receiver side. Or more correctly, the remote end that is connected to a cellular modem should be started first. This can avoid issues with remapped ports.

To stop the programs, in the terminal:<br>
`$ ./killitall.sh`<br>

#Streaming status
Streaming status is printed on stdout and also transmitted on UDP port 30200, this can be used for instance to visualize bitrate, RTT, packet loss etc in a graphical user interface
An example of streaming status is:  
*** 396.540, 97, 0.044, 0.010, 609409, 105159,  31465, 1, 0.003,  17433,  18372,  17868,     0,     0, 0.001,  12453,  13480,  13520,     0,     0,<br>
 396.791, 86, 0.042, 0.008, 609409,  92399,  31021, 1, 0.014,  17608,  16876,  17646,     0,     0, 0.000,  12487,  13176,  13298,     0,     0,<br>
 397.042, 88, 0.043, 0.010, 609409,  95279,  31992, 1, 0.012,  17828,  17346,  17979,     0,     0, 0.004,  12573,  13907,  13936,     0,     0,<br>***
Where the columns are (listed left to right)<br>
1. Time [s]<br>
2. Quality index for first camera (only)<br>
3. Estimated queue delay [s]<br>
4. Average RTT [s] <br>
5. Congestion window [byte]<br>
6. Bytes in flight [byte]<br>
7. Total bitrate [kbps]<br>
8. Fast increase mode indicator [0 or 1]<br>
9. Camera 1 RTP queue delay [s]<br>
10. Camera 1 Target bitrate [kbps]<br>
11. Camera 1 Video coder bitrate [kbps]<br>
12. Camera 1 Transmitted bitrate [kbps]<br>
13. Camera 1 Loss bitrate [kbps]<br>
14. Camera 1 Congestion marked bitrate (ECN, L4S) bitrate [kbps]<br>
15. Camera 2 RTP queue delay [s]<br>
16. Camera 2 Target bitrate [kbps]<br>
17. Camera 2 Video coder bitrate [kbps]<br>
18. Camera 2 Transmitted bitrate [kbps]<br>
19. Camera 2 Loss bitrate [kbps]<br>
20. Camera 2 Congestion marked bitrate (ECN, L4S) bitrate [kbps]<br>
