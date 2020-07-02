/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2018  <<user@hostname.org>>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include "gstgscreamrx.h"

#define DOSCREAM true

GST_DEBUG_CATEGORY_STATIC (gst_g_scream_rx_debug);
#define GST_CAT_DEFAULT gst_g_scream_rx_debug

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SILENT
};

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("ANY")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("ANY")
    );

static GstStaticPadTemplate src_rtcp_factory = GST_STATIC_PAD_TEMPLATE ("src-rtcp",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("ANY")
    );


#define gst_g_scream_rx_parent_class parent_class
G_DEFINE_TYPE (GstgScreamRx, gst_g_scream_rx, GST_TYPE_ELEMENT);

static void gst_g_scream_rx_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_g_scream_rx_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_g_scream_rx_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);
static GstFlowReturn gst_g_scream_rx_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);

/* GObject vmethod implementations */

/* initialize the gscreamrx's class */
static void
gst_g_scream_rx_class_init (GstgScreamRxClass * gScreamRxClass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) gScreamRxClass;
  gstelement_class = (GstElementClass *) gScreamRxClass;



  gobject_class->set_property = gst_g_scream_rx_set_property;
  gobject_class->get_property = gst_g_scream_rx_get_property;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));

  gst_element_class_set_details_simple(gstelement_class,
    "gScreamRx",
    "FIXME:Generic",
    "FIXME:Generic Template Element",
    " <<user@hostname.org>>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_rtcp_factory));
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_g_scream_rx_init (GstgScreamRx * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_g_scream_rx_sink_event));
  gst_pad_set_chain_function (filter->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_g_scream_rx_chain));
  GST_PAD_SET_PROXY_CAPS (filter->sinkpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  GST_PAD_SET_PROXY_CAPS (filter->srcpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->srcrtcppad = gst_pad_new_from_static_template (&src_rtcp_factory, "src-rtcp");
  GST_PAD_SET_PROXY_CAPS (filter->srcrtcppad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcrtcppad);

  filter->gstClockTimeRef = gst_clock_get_time(gst_system_clock_obtain());
  filter->screamRx = new ScreamRx(0); // Note SSRC should be something else
  g_print("\n*************INIT*************\n\n");

  filter->rtpSession = NULL;
  filter->lastRxTime = 0.0;
  pthread_mutex_init(&filter->lock_scream, NULL);

  filter->silent = FALSE;
}

static void
gst_g_scream_rx_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstgScreamRx *filter = GST_GSCREAMRX (object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_g_scream_rx_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstgScreamRx *filter = GST_GSCREAMRX (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void print_rtcp_feedback_type(GObject *session, guint session_id,
    guint fbtype, guint media_ssrc, GstRTCPType packet_type, guint8 *fci,
    gboolean is_received)
{
    if (fbtype == GST_RTCP_FB_TYPE_INVALID) {
        g_print("INVALID\n");
    } else if (packet_type == GST_RTCP_TYPE_RTPFB) {
        switch (fbtype) {
        case GST_RTCP_RTPFB_TYPE_NACK:
            g_print("NACK\n");
            break;
        case GST_RTCP_RTPFB_TYPE_TMMBR:
            g_print("TMMBR\n");
            break;
        case GST_RTCP_RTPFB_TYPE_TMMBN:
            g_print("TMMBN\n");
            break;
        case GST_RTCP_RTPFB_TYPE_RTCP_SR_REQ:
            g_print("SR_REQ\n");
            break;
        //case GST_RTCP_RTPFB_TYPE_SCREAM:
        //    GST_CAT_INFO_OBJECT(_owrsession_debug, session, "Session %u, %s RTCP feedback for %u: SCReAM\n",
        //        session_id, is_received ? "Received" : "Sent", media_ssrc);
        //    break;
        default:
            g_print("UNKNOWN\n");
            break;
        }
    } else if (packet_type == GST_RTCP_TYPE_PSFB) {
        switch (fbtype) {
        case GST_RTCP_PSFB_TYPE_PLI:
        g_print("PLI\n");
            break;
        case GST_RTCP_PSFB_TYPE_SLI:
        g_print("SLI\n");
            break;
        case GST_RTCP_PSFB_TYPE_RPSI:
        g_print("NACK\n");
            break;
        case GST_RTCP_PSFB_TYPE_AFB:
        g_print("AFB\n");
            break;
        case GST_RTCP_PSFB_TYPE_FIR:
        g_print("FIR\n");
            break;
        case GST_RTCP_PSFB_TYPE_TSTR:
        g_print("TSTR\n");
            break;
        case GST_RTCP_PSFB_TYPE_TSTN:
        g_print("TSTN\n");
            break;
        case GST_RTCP_PSFB_TYPE_VBCN:
        g_print("VBCN\n");
            break;
        default:
        g_print("UNKNOWN\n");
            break;
        }
    }
}

static void print_rtcp_type(GObject *session, guint session_id,
    GstRTCPType packet_type)
{
    if (packet_type == GST_RTCP_TYPE_INVALID) g_print("Invalid\n");
    if (packet_type == GST_RTCP_TYPE_SR) g_print("Sender Report (SR)\n");
    if (packet_type == GST_RTCP_TYPE_RR) g_print("Receiver Report (RR)\n");
    if (packet_type == GST_RTCP_TYPE_SDES) g_print("Source Description (SDES)\n");
    if (packet_type == GST_RTCP_TYPE_BYE) g_print("Goodbye (BYE)\n");
    if (packet_type == GST_RTCP_TYPE_APP) g_print("Application defined (APP)\n");
    if (packet_type == GST_RTCP_TYPE_RTPFB) g_print("RTP Feedback (RTPFB)\n");
    if (packet_type == GST_RTCP_TYPE_PSFB) g_print("Payload-Specific Feedback (PSFB)\n");
    /*GST_CAT_DEBUG_OBJECT(_owrsession_debug, session, "Session %u, Received RTCP %s\n", session_id,
        packet_type == GST_RTCP_TYPE_INVALID ? "Invalid type (INVALID)" :
        packet_type == GST_RTCP_TYPE_SR ? "Sender Report (SR)" :
        packet_type == GST_RTCP_TYPE_RR ? "Receiver Report (RR)" :
        packet_type == GST_RTCP_TYPE_SDES ? "Source Description (SDES)" :
        packet_type == GST_RTCP_TYPE_BYE ? "Goodbye (BYE)" :
        packet_type == GST_RTCP_TYPE_APP ? "Application defined (APP)" :
        packet_type == GST_RTCP_TYPE_RTPFB ? "RTP Feedback (RTPFB)" :
        packet_type == GST_RTCP_TYPE_PSFB ? "Payload-Specific Feedback (PSFB)" :
        "unknown");
      */
}

void getTime(GstgScreamRx *filter, float *time_s, guint32 *time_ntp) {
  GstClockTime gstClockTime = gst_clock_get_time(gst_system_clock_obtain());
  gstClockTime -= filter->gstClockTimeRef;
  *time_s = gstClockTime/1.0e9f;

  // Convert to seconds in Q16
  gstClockTime /= 1000;
  gstClockTime *= 256;
  gstClockTime /= 1000;
  gstClockTime *= 256;
  gstClockTime /= 1000;

  *time_ntp = (gstClockTime) & 0xFFFFFFFF;
}

/* GstElement vmethod implementations */
gboolean rtcpPeriodicTimer(GstClock *clock, GstClockTime, GstClockID id, gpointer user_data)
{
  GstgScreamRx *filter = (GstgScreamRx*) user_data;
  float time;
  guint32 time_ntp;

  pthread_mutex_lock(&filter->lock_scream);
  getTime(filter, &time, &time_ntp);
  bool isFeedback = filter->screamRx->isFeedback(time_ntp) &&
      (time_ntp - filter->screamRx->getLastFeedbackT() > filter->screamRx->getRtcpFbInterval() || filter->screamRx->checkIfFlushAck());
  pthread_mutex_unlock(&filter->lock_scream);

  if (true && isFeedback) {
    g_signal_emit_by_name(filter->rtpSession,"send-rtcp",20000000);
   // g_print(" SF \n");
  }
  return FALSE;
}

static gboolean on_sending_rtcp(GObject *session, GstBuffer *buffer, gboolean early, GObject *object)
{
  GstgScreamRx *filter = (GstgScreamRx*) object;

  GstClockTime gstClockTime = gst_clock_get_time(gst_system_clock_obtain());
  gstClockTime -= filter->gstClockTimeRef;
  float time = gstClockTime/1.0e9f;

  GstRTCPBuffer rtcp_buffer = {NULL, {NULL, (GstMapFlags)0, NULL, 0, 0, {0}, {0}}};
  GstRTCPPacket rtcp_packet;
  GstRTCPType packet_type;
  guint session_id;
  gboolean has_packet, do_not_suppress = FALSE;

  session_id = GPOINTER_TO_UINT(g_object_get_data(session, "session_id"));
  //g_print("%6.3f SEND RTCP ?  %d bytes session %d\n", time , gst_buffer_get_size(buffer), session_id);
  int iter = 0;

  if (gst_rtcp_buffer_map(buffer, (GstMapFlags)(GST_MAP_READ | GST_MAP_WRITE), &rtcp_buffer)) {



/*
    for (; has_packet; has_packet = gst_rtcp_packet_move_to_next(&rtcp_packet)) {
        if (iter==1)
          gst_rtcp_packet_remove(&rtcp_packet);
        iter++;
        */
/*
        packet_type = gst_rtcp_packet_get_type(&rtcp_packet);
        print_rtcp_type(session, session_id, packet_type);
        if (packet_type == GST_RTCP_TYPE_PSFB || packet_type == GST_RTCP_TYPE_RTPFB) {
            print_rtcp_feedback_type(session, session_id, gst_rtcp_packet_fb_get_type(&rtcp_packet),
                gst_rtcp_packet_fb_get_media_ssrc(&rtcp_packet), packet_type,
                gst_rtcp_packet_fb_get_fci(&rtcp_packet), FALSE);
            do_not_suppress = TRUE;
            break;
        }
*/
    //}

    float time;
    guint32 time_ntp;


    int rtcpSize = 0;
    guint8 buf[2000];
    pthread_mutex_lock(&filter->lock_scream);
    getTime(filter, &time, &time_ntp);
    bool isFb = filter->screamRx->createStandardizedFeedback(time_ntp, buf , rtcpSize);
    pthread_mutex_unlock(&filter->lock_scream);
    if (isFb && time - filter->lastRxTime < 2.0) {
      //g_print("TS %X \n",(time_ntp));
      gst_rtcp_buffer_add_packet(&rtcp_buffer, GST_RTCP_TYPE_RTPFB, &rtcp_packet);
      guint32 ssrc_n,ssrc_h;
      memcpy(&ssrc_n, &buf[8],4);


      ssrc_h = g_ntohl(ssrc_n);
      //g_print("MSSRC %x \n", ssrc_h);
      gst_rtcp_packet_fb_set_media_ssrc(&rtcp_packet, ssrc_h);
      //gst_rtcp_packet_fb_set_type(&rtcp_packet, 0);
      //gst_rtcp_packet_set_type(rtcp_packet, GST_RTCP_TYPE_RTPFB);
      int len = rtcpSize/4-3;
      gst_rtcp_packet_fb_set_fci_length(&rtcp_packet, len);
      guint8* fci_buf = gst_rtcp_packet_fb_get_fci(&rtcp_packet);
      memcpy(fci_buf, buf+12, len*4);
      //g_print("%6.3f RTCP fb of size %d TS %X \n", time, rtcpSize, time_ntp);

      //has_packet = gst_rtcp_buffer_get_first_packet(&rtcp_buffer, &rtcp_packet);
      //gst_rtcp_packet_remove(&rtcp_packet);
/*      has_packet = gst_rtcp_packet_move_to_next(&rtcp_packet);
      gst_rtcp_packet_remove(&rtcp_packet);
*/
   }



    gst_rtcp_buffer_unmap(&rtcp_buffer);
  }
  return true;

}


/* this function handles sink events */
static gboolean
gst_g_scream_rx_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstgScreamRx *filter;
  gboolean ret;

  filter = GST_GSCREAMRX (parent);
if(DOSCREAM)   {
  if (filter->rtpSession == NULL) {
    GstElement *pipe = GST_ELEMENT_PARENT(parent);
    //g_print("\ngscream name2 %s \n", gst_element_get_name(pipe));
    GstElement *rtpbin = gst_bin_get_by_name_recurse_up(GST_BIN(pipe), "rtpbin");
    g_assert(rtpbin);
    g_signal_emit_by_name(rtpbin,"get_internal_session", 0, &(filter->rtpSession));

    g_object_set(&(filter->rtpSession), "rtcp-reduced-size", true, NULL);

    GstClock *clock = gst_pipeline_get_clock((GstPipeline*) pipe);
    filter->clockId=gst_clock_new_periodic_id(clock,gst_clock_get_internal_time(clock),5000000);
    g_assert(filter->clockId);
    GstClockReturn t = gst_clock_id_wait_async(filter->clockId, rtcpPeriodicTimer, (gpointer) filter, NULL);
    g_print("SINK EVENT \n");

    g_print("\n\n  ENABLE CALLBACK  \n\n");
    g_object_set((filter->rtpSession),
      "rtcp-min-interval", 500000000,
      "rtcp-fraction", 0.5,
      "bandwidth", 10000000.0,
      //"rtcp-rr-bandwidth", 40000,
      //"rtcp-min-interval", 100000,
      //"rtcp-fraction", 50000.0,
      //"bandwidth", 0.0,
      //"rtcp-rr-bandwidth", 40000,
      "rtp-profile", GST_RTP_PROFILE_AVPF,
      NULL);
    g_signal_connect_after((filter->rtpSession), "on-sending-rtcp", G_CALLBACK(on_sending_rtcp), filter);
}
  }
  GST_LOG_OBJECT (filter, "Received %s event: %" GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps * caps;

      gst_event_parse_caps (event, &caps);
      /* do something with the caps */

      /* and forward */
      ret = gst_pad_event_default (pad, parent, event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }
  return ret;
}


/* chain function
 * this function does the actual processing
 */
static GstFlowReturn
gst_g_scream_rx_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstgScreamRx *filter;

  filter = GST_GSCREAMRX (parent);
if (DOSCREAM)  {
  //if (filter->silent == FALSE)
  //  g_print ("I'm plugged, therefore I'm in.\n");

  guint8 b0,b1;
  guint16 sn_n;
  guint32 ssrc_n;

  //if (scream->silent == FALSE)

  gst_buffer_extract (buf, 0, &b0, 1);
  gst_buffer_extract (buf, 1, &b1, 1);
  int pt = b1;
  gst_buffer_extract (buf, 2, &sn_n, 2);
  guint16 sn_h = g_ntohs(sn_n);
  gst_buffer_extract (buf, 8, &ssrc_n, 4);
  guint32 ssrc_h = g_ntohl(ssrc_n);


  int size = gst_buffer_get_size (buf);

  float time;
  guint32 time_ntp;
  getTime(filter, &time, &time_ntp);

  //g_print ("%d %d %d %x %d %f\n",
  //    size , pt, sn_h, ssrc_h, time_ntp, time);

  if (time - filter->lastRxTime > 2.0) {
    pthread_mutex_lock(&filter->lock_scream);
    delete filter->screamRx;
    filter->screamRx = new ScreamRx(0); // Note SSRC should be something else
    pthread_mutex_unlock(&filter->lock_scream);
    g_print("Restart receiver\n");

  }
  filter->lastRxTime = time;
  //g_print("%6.3f RX %d \n", time, sn_h);

  pthread_mutex_lock(&filter->lock_scream);
  getTime(filter, &time, &time_ntp);
  filter->screamRx->receive(time_ntp, 0, ssrc_h, size, sn_h, 0);
  pthread_mutex_unlock(&filter->lock_scream);


  //bool isFeedback = filter->screamRx->isFeedback(time_ntp) &&
  //    (time_ntp - filter->screamRx->getLastFeedbackT() > filter->screamRx->getRtcpFbInterval() || filter->screamRx->checkIfFlushAck());

  //if (true && isFeedback) {
    //g_print("%6.3f Request SEND RTCP  1 %d \n", time, sn_h);
    //g_signal_emit_by_name(filter->rtpSession,"send-rtcp",20000000);
  //}

}
  /* just push out the incoming buffer without touching it */
  return gst_pad_push (filter->srcpad, buf);
}


/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
gscreamrx_init (GstPlugin * gscreamrx)
{
  /* debug category for fltering log messages
   *
   * exchange the string 'Template gscreamrx' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_g_scream_rx_debug, "gscreamrx",
      0, "Template gscreamrx");

  return gst_element_register (gscreamrx, "gscreamrx", GST_RANK_NONE,
      GST_TYPE_GSCREAMRX);
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "myfirstgscreamrx"
#endif

/* gstreamer looks for this structure to register gscreamrxs
 *
 * exchange the string 'Template gscreamrx' with your gscreamrx description
 */
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    gscreamrx,
    "SCReAM receiver side",
    gscreamrx_init,
    VERSION,
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)
