#include <iostream>
#include <string>
#include <gst/gst.h>
#include <glib.h>
#include <gst/rtp/rtp.h>

static gboolean bus_call (GstBus *bus, GstMessage *msg, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;

  switch (GST_MESSAGE_TYPE (msg)) {

    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
      break;

    case GST_MESSAGE_ERROR: {
      gchar  *debug;
      GError *error;

      gst_message_parse_error (msg, &error, &debug);
      g_free (debug);

      g_printerr ("Error: %s\n", error->message);
      g_error_free (error);

      g_main_loop_quit (loop);
      break;
    }
    default:
      break;
  }

  return TRUE;
}

/* will be called when rtpbin signals on-ssrc-active. It means that an RTCP
 * packet was received from another source. */
static void
on_ssrc_active_cb (GstElement * rtpbin, guint sessid, guint ssrc,
    GstElement * depay)
{
  GObject *session, *osrc;

  g_print ("got RTCP from session %u, SSRC %u\n", sessid, ssrc);

  /* get the right session */
  g_signal_emit_by_name (rtpbin, "get-internal-session", sessid, &session);


  /* get the remote source that sent us RTCP */
  g_signal_emit_by_name (session, "get-source-by-ssrc", ssrc, &osrc);
  //print_source_stats (osrc);
  g_object_unref (osrc);
  g_object_unref (session);
}

/* will be called when rtpbin has validated a payload that we can depayload */
static void
pad_added_cb (GstElement * rtpbin, GstPad * new_pad, GstElement * depay)
{
  GstPad *sinkpad;
  GstPadLinkReturn lres;

  g_print ("new payload on pad: %s\n", GST_PAD_NAME (new_pad));

  sinkpad = gst_element_get_static_pad (depay, "sink");
  g_assert (sinkpad);

  lres = gst_pad_link (new_pad, sinkpad);
  g_assert (lres == GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);
}

int main (int argc, char *argv[])
{
  std::printf("going for video from IP: %s\n  on port 5000", argv[1]);

  GMainLoop *loop;

  GstElement *rtpbin, *pipeline, *rtpsrc, *rtcpsrc, *rtcpsink, *rtpdepay,
    *decodebin, *videoconvert, *videosink, *gscreamrx, *rtpjitterbuffer;
  GstBus *bus;
  guint bus_watch_id;
  GstCaps *udpcaps;

  /* Initialisation */
  gst_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  /* Create gstreamer elements */
  pipeline        = gst_pipeline_new ("pipeline");

  rtpsrc          = gst_element_factory_make("udpsrc",           "rtpsrc");
  udpcaps = gst_caps_new_simple ("application/x-rtp",
    "media", G_TYPE_STRING,"video",
    "clock-rate", G_TYPE_INT, 90000,
    "encoding-name",G_TYPE_STRING,"H264",
    NULL);
  g_object_set(G_OBJECT(rtpsrc), "port", 5000, "caps", udpcaps, NULL);
  gst_object_unref(udpcaps);

  rtcpsrc         = gst_element_factory_make("udpsrc",           "rtcpsrc");
  g_object_set(G_OBJECT(rtcpsrc), "port", 5001, NULL);
  g_assert (rtcpsrc);
  rtcpsink        = gst_element_factory_make("udpsink",          "rtcpsink");
  g_object_set (rtcpsink, "port", 5001, "host", argv[1], "bind-port", 5001, NULL);
  /* no need for synchronisation or preroll on the RTCP sink */
  g_object_set (rtcpsink, "async", FALSE, "sync", FALSE, NULL);
  g_assert (rtcpsink);

  gscreamrx       = gst_element_factory_make("gscreamrx",    "gscreamrx");
  rtpjitterbuffer    = gst_element_factory_make("rtpjitterbuffer",    "rtpjitterbuffer");
  rtpdepay        = gst_element_factory_make("rtph264depay",    "rtph264depay");
  decodebin       = gst_element_factory_make ("avdec_h264",     "avdec_h264");
  videoconvert    = gst_element_factory_make ("videoconvert",   "videoconvert");
  videosink   = gst_element_factory_make ("xvimagesink",  "videosink");

  if (!pipeline || !rtpdepay || !decodebin || !videoconvert || !videosink) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }

  /* Set up the pipeline */

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
  gst_object_unref (bus);

  /* we add all elements into the pipeline */
  gst_bin_add_many (GST_BIN (pipeline),
                    rtpsrc, gscreamrx, rtpjitterbuffer, rtpdepay, decodebin, videoconvert, videosink, NULL);

  /* we link the elements together */
  /* videotestsrcm -> autovideosink */
  if(!gst_element_link_many (rtpsrc, gscreamrx, rtpjitterbuffer, rtpdepay, decodebin, videoconvert, videosink, NULL)){
    g_error("Could not link on ore more of elements udpsrc, rtpdepay decodebin");
    return -1;
  }

  /* the rtpbin element */
  rtpbin = gst_element_factory_make ("rtpbin", "rtpbin");
  g_assert (rtpbin);
  gst_bin_add(GST_BIN (pipeline), rtpbin);
  g_object_set(rtpbin, /*"latency", 5, */"rtp-profile", GST_RTP_PROFILE_AVPF, NULL);
  g_object_set(videosink, "sync", false, "async", false, NULL);
  g_object_set(rtpjitterbuffer, "latency", 50, NULL);

  gst_bin_add_many(GST_BIN (pipeline), rtcpsrc, rtcpsink, NULL);

  /* now link all to the rtpbin, start by getting an RTP sinkpad for session 0 */
  gst_element_link_pads(rtpsrc, "src", rtpbin, "recv_rtp_sink_0");

  /* get an RTCP sinkpad in session 0 */
  gst_element_link_pads(rtcpsrc, "src", rtpbin, "recv_rtp_sink_0");

  /* get an RTCP srcpad for sending RTCP back to the sender */
  gst_element_link_pads(rtpbin, "send_rtcp_src_0", rtcpsink, "sink");


  GObject *rtp_session = NULL;
  GstElement *rtpbin_ = NULL;
  GstElement *pipe = GST_ELEMENT_PARENT(gscreamrx);
  g_assert(pipe);
  std::printf("\n name %s \n", gst_element_get_name(pipe));
  rtpbin_ = gst_bin_get_by_name_recurse_up(GST_BIN(pipe), "rtpbin");
  g_assert(rtpbin_);
  g_signal_emit_by_name(rtpbin_,"get_internal_session", 0, &rtp_session);
  std::printf("\n    SESSION     %p  \n", rtp_session);
  g_assert(rtp_session);


  /* the RTP pad that we have to connect to the depayloader will be created
   * dynamically so we connect to the pad-added signal, pass the depayloader as
   * user_data so that we can link to it. */
  g_signal_connect (rtpbin, "pad-added", G_CALLBACK (pad_added_cb), rtpdepay);

  /* give some stats when we receive RTCP */
  g_signal_connect (rtpbin, "on-ssrc-active", G_CALLBACK (on_ssrc_active_cb),
      rtpdepay);

  //GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "recv-pipeline-bf-playing");

  /* Set the pipeline to "playing" state*/
  g_print ("Now set pipeline in state playing\n");
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "recv-pipeline-af-playing-bf-running");

  /* Iterate */
  g_print ("Running...\n");
  g_main_loop_run (loop);
  GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "recv-pipeline-af-running");

  /* Out of the main loop, clean up nicely */
  g_print ("Returned, stopping playback\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "recv-pipeline-af-stop");

  g_print ("Deleting pipeline\n");
  gst_object_unref (GST_OBJECT (pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (loop);

  return 0;
}
