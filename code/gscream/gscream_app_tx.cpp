#include <iostream>
#include <string>
#include <gst/gst.h>
#include <glib.h>
#include <gst/rtp/rtp.h>

int main (int argc, char *argv[])
{
  g_printerr(" \n --- Initializing gscream_app_tx \n");

  if(argc < 3){
    std::cout << "Usage: " << argv[0] << " videosrc ip(recieving)" << std::endl;
    std::cout << "Example: " << argv[0] << " /dev/video0 127.0.0.1   sends the video stream to localhost on port 5000" << std::endl;
    return -1;
  }
  std::printf("Sending video from source: %s to ip: %s port %i\n",argv[1], argv[2],5000);

  GMainLoop *loop;
  GstElement *pipeline, *videosrc, *videoconvert, *capsfilter, *rtpsink, *rtcpsink,
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
  //videosrc        = gst_element_factory_make ("videotestsrc",   "videosource");
  videosrc        = gst_element_factory_make ("v4l2src",        "videosource");
  videoconvert    = gst_element_factory_make ("videoconvert",   "videoconvert");
  capsfilter      = gst_element_factory_make ("capsfilter",     "capsfilter");
  videoencode     = gst_element_factory_make ("x264enc",        "encoder");
  videopayload    = gst_element_factory_make ("rtph264pay",     "payload");
  gscreamtx       = gst_element_factory_make ("gscreamtx",      "scream-tx");

  if (!pipeline || !videosrc || !videoencode || !videopayload) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }


  /* Setting element properties*/
  //g_object_set  (G_OBJECT(videosrc), "device", argv[1], NULL);
  //g_object_set  (G_OBJECT(udpsrc), "host", argv[2], "port", std::atoi(argv[3]), NULL);
  //capsfiltercaps = gst_caps_from_string("video/x-raw-yuv,format=(fourcc)YUV2,framerate=30/1");
  capsfiltercaps = gst_caps_from_string("video/x-raw,framerate=30/1");//,width=800,height=600");
  //capsfiltercaps = gst_caps_from_string("video/x-h264,profile=main,framerate=30/1");
  //capsfiltercaps = gst_caps_from_string("video/x-raw,framerate=30/1");
  /*gst_caps_new_simple ("video/x-raw",
      "framerate" G_TYPE_FRAC, 10, 1,
      "width", G_TYPE_INT, 640,
      "height", G_TYPE_INT, 480,
      NULL);
      */
  g_object_set (G_OBJECT(capsfilter), "caps",  capsfiltercaps, NULL);

  /* Set up the pipeline */

  /* we add a message handler */


  /* we add all elements into the pipeline */
  gst_bin_add_many (GST_BIN (pipeline),
                    videosrc, capsfilter, videoconvert, videoencode, videopayload, gscreamtx,  NULL);

  /* we link the elements together */
  gst_element_link_many (videosrc, capsfilter, videoconvert, videoencode, videopayload, gscreamtx, NULL);


  GstElement *enc = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), "encoder");
  g_print("%s %p \n", gst_element_get_name(videoencode), enc);
  g_assert(enc);
  g_object_set(G_OBJECT(enc), "bitrate", 200, NULL);
  //g_object_set(G_OBJECT(enc), "intra-refresh", true, NULL);
  g_object_set(G_OBJECT(enc), "key-int-max", 50, NULL);
  g_object_set(G_OBJECT(enc), "tune", 4, NULL);
  g_object_set(G_OBJECT(enc), "vbv-buf-capacity", 200, NULL);


  //g_object_set (G_OBJECT(videosrc),"is-live", 1,  "horizontal-speed", 5, "pattern", 11, NULL);
  g_object_set (G_OBJECT(videosrc),"device", argv[1],  NULL);

  rtpbin          = gst_element_factory_make ("rtpbin",         "rtpbin");
  g_object_set (rtpbin, "rtp-profile", GST_RTP_PROFILE_AVPF, NULL);
  gst_bin_add (GST_BIN (pipeline), rtpbin);



  /* the udp sinks and source we will use for RTP and RTCP */
  rtpsink = gst_element_factory_make ("udpsink", "rtpsink");
  g_assert (rtpsink);
  g_object_set (rtpsink, "port", 5000, "host", argv[2], NULL);

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
  srcpad = gst_element_get_static_pad (/*videopayload*/ gscreamtx, "src");
  if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK)
    g_error ("Failed to link audio payloader to rtpbin");
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
