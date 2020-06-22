#include <iostream>
#include <string>
#include <gst/gst.h>
#include <glib.h>
#include <gst/rtp/rtp.h>

#define SCREAM

int main (int argc, char *argv[])
{
  g_printerr(" \n --- Initializing  rpicamsrc with SCReAM congestion control ---\n");

  if(argc < 2){
    std::cout << "Usage: " << argv[0] << " ip(recieving)" << std::endl;
    std::cout << "Example: " << argv[0] << " 127.0.0.1   sends the video stream to localhost on port 5000" << std::endl;
    return -1;
  }
  std::printf("Sending video to ip: %s port %i\n",argv[1],5000);

  GMainLoop *loop;
  GstElement *pipeline, *capsfilter, *rtpsink, *rtcpsink,
   *videoencode, *videopayload, *gscreamtx;
  GstElement *rtpbin, *rtcpsrc;
  GstPad *srcpad, *sinkpad;
  GstCaps *capsfiltercaps;

  /* Initialisation */
  gst_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  /* Create gstreamer elements */
  pipeline = gst_pipeline_new ("pipeline");

  /*Main imports*/
  videoencode     = gst_element_factory_make ("rpicamsrc",      "encoder");
  capsfilter      = gst_element_factory_make ("capsfilter",     "capsfilter");
  videopayload    = gst_element_factory_make ("rtph264pay",     "payload");
#ifdef SCREAM
  gscreamtx       = gst_element_factory_make ("gscreamtx",      "scream-tx");
  g_assert (gscreamtx);
#endif  

  g_assert (videoencode);
  g_assert (capsfilter);
  g_assert (videopayload);


  /* Setting element properties*/
  capsfiltercaps = gst_caps_from_string("video/x-h264,width=1280,height=720,framerate=30/1,profile=high");//,width=800,height=600");
  g_object_set (G_OBJECT(capsfilter), "caps",  capsfiltercaps, NULL);

  g_object_set(G_OBJECT(videoencode), "bitrate", 1000000, NULL);
  g_object_set(G_OBJECT(videoencode), "quantisation-parameter", 0, NULL);
  g_object_set(G_OBJECT(videoencode), "intra-refresh-type", 2130706433, NULL);//2130706433, NULL);
  g_object_set(G_OBJECT(videoencode), "keyframe-interval", 30, NULL);
  g_object_set(G_OBJECT(videoencode), "awb-mode", 6, NULL);
  g_object_set(G_OBJECT(videoencode), "preview", false, NULL);
  g_object_set(G_OBJECT(videoencode), "annotation-mode", 1, NULL);
  g_object_set(G_OBJECT(videoencode), "inline-headers", true, NULL);


  /* we add all elements into the pipeline */
#ifdef SCREAM
  gst_bin_add_many (GST_BIN (pipeline),
                    videoencode, capsfilter, videopayload, gscreamtx, NULL);
#else
  gst_bin_add_many (GST_BIN (pipeline),
                    videoencode, capsfilter, videopayload, NULL);
#endif


  /* we link the elements together */
#ifdef SCREAM
  gst_element_link_many (videoencode, capsfilter, videopayload, gscreamtx, NULL);
#else
  gst_element_link_many (videoencode, capsfilter, videopayload,  NULL);
#endif

  rtpbin          = gst_element_factory_make ("rtpbin",         "rtpbin");
  g_object_set (rtpbin, "rtp-profile", GST_RTP_PROFILE_AVPF, NULL);
  gst_bin_add (GST_BIN (pipeline), rtpbin);


  /* the udp sinks and source we will use for RTP and RTCP */
  rtpsink = gst_element_factory_make ("udpsink", "rtpsink");
  g_assert (rtpsink);
  g_object_set (rtpsink, "port", 5000, "host", argv[1], NULL);

  rtcpsink = gst_element_factory_make ("udpsink", "rtcpsink");
  g_assert (rtcpsink);
  g_object_set (rtcpsink, "port", 5001, "host", argv[1], "bind-port", 5001, NULL);
  /* no need for synchronisation or preroll on the RTCP sink */
  g_object_set (rtcpsink, "async", FALSE, "sync", FALSE, NULL);

  rtcpsrc = gst_element_factory_make ("udpsrc", "rtcpsrc");
  g_assert (rtcpsrc);
  g_object_set (rtcpsrc, "port", 5001, NULL);

  gst_bin_add_many (GST_BIN (pipeline), rtpsink, rtcpsink, rtcpsrc, NULL);

  /* now link all to the rtpbin, start by getting an RTP sinkpad for session 0 */
  sinkpad = gst_element_get_request_pad (rtpbin, "send_rtp_sink_0");
#ifdef SCREAM
  srcpad = gst_element_get_static_pad (gscreamtx, "src");
#else
  srcpad = gst_element_get_static_pad (videopayload, "src");
#endif  
  if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK)
    g_error ("Failed to link video payloader to rtpbin");
  gst_object_unref (srcpad);

  /* get the RTP srcpad that was created when we requested the sinkpad above and
   * link it to the rtpsink sinkpad*/
  srcpad = gst_element_get_static_pad (rtpbin, "send_rtp_src_0");
  sinkpad = gst_element_get_static_pad (rtpsink, "sink");
  if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK)
    g_error ("Failed to link rtpbin to rtpsink");
  gst_object_unref (srcpad);
  gst_object_unref (sinkpad);

  /* get an RTCP srcpad for sending RTCP to the receiver */
  srcpad = gst_element_get_request_pad (rtpbin, "send_rtcp_src_0");
  sinkpad = gst_element_get_static_pad (rtcpsink, "sink");
  if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK)
    g_error ("Failed to link rtpbin to rtcpsink");
  gst_object_unref (sinkpad);

  /* we also want to receive RTCP, request an RTCP sinkpad for session 0 and
   * link it to the srcpad of the udpsrc for RTCP */
  srcpad = gst_element_get_static_pad (rtcpsrc, "src");
  sinkpad = gst_element_get_request_pad (rtpbin, "recv_rtcp_sink_0");
  if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK)
    g_error ("Failed to link rtcpsrc to rtpbin");
  gst_object_unref (srcpad);

  /* Set the pipeline to "playing" state*/
  g_print ("Now set pipeline in state playing\n");
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  //GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "send-pipeline-af-playing-bf-running");

  /* Iterate */
  g_print ("Running...\n");
  g_main_loop_run (loop);


  /* Out of the main loop, clean up nicely */
  g_print ("Returned, stopping playback\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "send-pipeline-af-stopping");

  g_print ("Deleting pipeline\n");
  gst_object_unref (GST_OBJECT (pipeline));
  g_main_loop_unref (loop);

  return 0;
}
