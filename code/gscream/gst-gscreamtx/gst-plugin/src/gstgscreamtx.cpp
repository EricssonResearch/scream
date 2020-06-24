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
 * IMPLIED, INCLUDING BUT NOT LIMITED TO
 * THE WARRANTIES OF MERCHANTABILITY,
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
#include <config.h>
#endif

#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include "gstgscreamtx.h"
#include "ScreamTx.h"

#define PACKET_PACING_ON
//#define PACKET_PACING_OFF
#define PACE_CLOCK_T_NS 2000000
#define PACE_CLOCK_T_S 0.002f

GstgScreamTx *filter_;
GST_DEBUG_CATEGORY_STATIC (gst_g_scream_tx_debug);
#define GST_CAT_DEFAULT gst_g_scream_tx_debug

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SILENT,
  PROP_MEDIA_SRC
};
#define DEST_HOST "127.0.0.1"

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */

/* GObject vmethod implementations */

/* initialize the gscreamtx's class */
static void
gst_g_scream_tx_class_init (GstgScreamTxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_g_scream_tx_set_property;
  gobject_class->get_property = gst_g_scream_tx_get_property;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output?",
          FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_MEDIA_SRC,
      g_param_spec_uint ("media-src", "Media source",
        "0=x264enc, 1=rpicamsrc, 2=uvch264src, 3=vaapih264enc, 4=omxh264enc",
        0, 4, 0,
        G_PARAM_READWRITE));

  gst_element_class_set_details_simple(gstelement_class,
    "gScreamTx",
    "FIXME:Generic",
    "FIXME:Generic Template Element",
    " <<user@hostname.org>>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_g_scream_tx_init (GstgScreamTx * filter)
{
  //g_print("INIT\n");
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  filter->rtpSession = NULL;
  filter->screamTx = new ScreamTx(0.8f, 0.9f, 0.1f, false, 1.0f, 2.0f, 50000, 0.0f, 20, false, false);
  //filter->screamTx->setCwndMinLow(5000);
  filter->gstClockTimeRef = gst_clock_get_time(gst_system_clock_obtain());
  filter->rtpQueue = new RtpQueue();
  filter->encoder = NULL;
  filter->lastRateChangeT = 0.0;
  pthread_mutex_init(&filter->lock_scream, NULL);
  pthread_mutex_init(&filter->lock_rtp_queue, NULL);

  filter->media_src = 0; // x264enc


  gst_pad_set_event_function (filter->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_g_scream_tx_sink_event));

  gst_pad_set_chain_function (filter->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_g_scream_tx_chain));

  GST_PAD_SET_PROXY_CAPS (filter->sinkpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  GST_PAD_SET_PROXY_CAPS (filter->srcpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->silent = TRUE;


}

static void
gst_g_scream_tx_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstgScreamTx *filter = GST_GSCREAMTX (object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    case PROP_MEDIA_SRC:
      filter->media_src = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_g_scream_tx_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstgScreamTx *filter = GST_GSCREAMTX (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

}

void getTime(GstgScreamTx *filter, float *time_s, guint32 *time_ntp)
{
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

gboolean txTimerEvent(GstClock *clock, GstClockTime t, GstClockID id, gpointer user_data) {
  GstgScreamTx *filter = (GstgScreamTx*) user_data;
  float time;
  guint32 time_ntp;
  getTime(filter,&time,&time_ntp);
  guint32 ssrc;
  guint16 sn;
#ifdef PACKET_PACING_ON
  pthread_mutex_lock(&filter->lock_scream);
  float retVal = filter->screamTx->isOkToTransmit(time_ntp, ssrc);
  pthread_mutex_unlock(&filter->lock_scream);
  float accRetVal = 0.0f;
  while (true && retVal >= 0 && accRetVal < PACE_CLOCK_T_S) {
    accRetVal += retVal;
    pthread_mutex_lock(&filter->lock_rtp_queue);
    RtpQueue *rtpQ = (RtpQueue*)filter->screamTx->getStreamQueue(ssrc);
    pthread_mutex_unlock(&filter->lock_rtp_queue);

    GstBuffer *buf = rtpQ->pop(sn);
    int size = gst_buffer_get_size(buf);
    gst_pad_push(filter->srcpad, buf);

    pthread_mutex_lock(&filter->lock_scream);
    getTime(filter,&time,&time_ntp);
    bool isMark = false; //XXX TODO: figure out correct value
    filter->screamTx->addTransmitted(time_ntp, ssrc, size, sn, isMark);
    retVal = filter->screamTx->isOkToTransmit(time_ntp, ssrc);
    pthread_mutex_unlock(&filter->lock_scream);
  }
#endif
  return FALSE;
}

/* GstElement vmethod implementations */
static void
on_receiving_rtcp(GObject *session, GstBuffer *buffer, gboolean early, GObject *object)
{
  //GstgScreamTx *filter = GST_GSCREAMTX (object);
  GstgScreamTx *filter = (GstgScreamTx*)object;
  //g_print("   PTR1 %x %x %x\n",  filter, filter_ , session);
  float time;
  guint32 time_ntp;
  getTime(filter_,&time,&time_ntp);

  if (filter_->rtpSession != NULL) {


  GstRTCPBuffer rtcp_buffer = {NULL, {NULL, (GstMapFlags)0, NULL, 0, 0, {0}, {0}}};
  GstRTCPPacket rtcp_packet;
  GstRTCPType packet_type;
  guint session_id;
  gboolean has_packet;
  guint8 buf[2000];

  if (gst_rtcp_buffer_map(buffer, (GstMapFlags)(GST_MAP_READ), &rtcp_buffer)) {
    has_packet = gst_rtcp_buffer_get_first_packet(&rtcp_buffer, &rtcp_packet);
    if (has_packet) {
      for (; has_packet ; has_packet = gst_rtcp_packet_move_to_next(&rtcp_packet)) {
        packet_type = gst_rtcp_packet_get_type(&rtcp_packet);
        if (packet_type == GST_RTCP_TYPE_RTPFB) {

          guint8 *fci_buf = gst_rtcp_packet_fb_get_fci(&rtcp_packet);
          guint16 fci_len = gst_rtcp_packet_fb_get_fci_length(&rtcp_packet);
          int size =  (fci_len)*4+12;

          guint32 ssrc_h = gst_rtcp_packet_fb_get_media_ssrc(&rtcp_packet);
          guint32 ssrc_n = g_htonl(ssrc_h);
          buf[0] = 0x80;
          buf[1] = 205;

          memset(&buf[4],0,4);

          memcpy(&buf[8],&ssrc_n,4);

          int streamId = 0;
          //if (getStream(ssrc_h, streamId) != NULL) {

          memcpy(&buf[12],&fci_buf[0],(fci_len)*4);
          guint16 size_field = size/4-1;

          size_field = g_htons(size_field);

          memcpy(&buf[2],&size_field,2);
          pthread_mutex_lock(&filter_->lock_scream);
          getTime(filter_,&time,&time_ntp);

          filter_->screamTx->incomingStandardizedFeedback(
            time_ntp,buf,size);

          pthread_mutex_unlock(&filter_->lock_scream);

          pthread_mutex_lock(&filter_->lock_scream);
          int rate = (int) (filter_->screamTx->getTargetBitrate(ssrc_h));
          switch (filter_->media_src) {
            case 0: // x264enc
            case 3: // vaapih264enc
              rate /= 1000;
              break;
            case 1: // rpicamsrc
            case 2: // uvch264src
            case 4: // omxh264enc
              break;
          }
          pthread_mutex_unlock(&filter_->lock_scream);

          if (true && rate > 0 && time-filter_->lastRateChangeT > 0.2) {
          //if (time-filter_->lastRateChangeT > 0.1) {
            //int rate = 1000000*(1+ (int(time/10) % 2));
            //int qp = 20+20*(int(time/5) % 2)  ;
            filter_->lastRateChangeT = time;
            switch (filter_->media_src) {
              case 0:
              case 1:
              case 3:
                g_object_set(G_OBJECT(filter_->encoder), "bitrate", rate, NULL);
                break;
              case 2:
                g_object_set(G_OBJECT(filter_->encoder), "average-bitrate", rate, NULL);
                g_object_set(G_OBJECT(filter_->encoder), "peak-bitrate", rate, NULL);
                break;
              case 4:
                g_object_set(G_OBJECT(filter_->encoder), "target-bitrate", rate, NULL);
                break;
            }

            char buf2[1000];
            pthread_mutex_lock(&filter_->lock_scream);
            filter_->screamTx->getShortLog(time, buf2);
            pthread_mutex_unlock(&filter_->lock_scream);

            if (filter_->media_src == 1)
              g_object_set(G_OBJECT(filter_->encoder), "annotation-text", buf2, NULL);

            g_print("%6.3f %s\n",time,buf2);
          }


#ifdef PACKET_PACING_OFF
          guint32 ssrc;
          guint16 sn;
          pthread_mutex_lock(&filter_->lock_scream);
          getTime(filter_,&time,&time_ntp);
          float retVal = filter_->screamTx->isOkToTransmit(time_ntp, ssrc);
          pthread_mutex_unlock(&filter_->lock_scream);

          while (true && retVal == 0) {
            pthread_mutex_lock(&filter_->lock_rtp_queue);
            RtpQueue *rtpQ = (RtpQueue*)filter_->screamTx->getStreamQueue(ssrc);
            pthread_mutex_unlock(&filter_->lock_rtp_queue);

            GstBuffer *buf = rtpQ->pop(sn);
            int size = gst_buffer_get_size(buf);
            gst_pad_push(filter_->srcpad, buf);

            pthread_mutex_lock(&filter_->lock_scream);
            getTime(filter_,&time,&time_ntp);
            bool isMark = false; //XXX TODO: figure out correct value
            filter_->screamTx->addTransmitted(time_ntp, ssrc, size, sn, isMark);
            retVal = filter_->screamTx->isOkToTransmit(time_ntp, ssrc);
            pthread_mutex_unlock(&filter_->lock_scream);
          }
#endif
          //}
          break;
        }
      }
    }
  }
}

}


/* chain function
 * this function does the actual processing
 */

int once = 0;
GstTask *task1;
GRecMutex *mutex1;

static GstFlowReturn
gst_g_scream_tx_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstgScreamTx *filter = GST_GSCREAMTX (parent);

  filter = GST_GSCREAMTX (parent);
  guint8 b0,b1;
  guint16 sn;
  guint32 ssrc;

  //if (scream->silent == FALSE)

  gst_buffer_extract (buf, 0, &b0, 1);
  gst_buffer_extract (buf, 1, &b1, 1);
  int pt = b1;
  gst_buffer_extract (buf, 2, &sn, 2);
  guint16 sn_h = g_ntohs(sn);
  gst_buffer_extract (buf, 8, &ssrc, 4);
  guint32 ssrc_h = g_ntohl(ssrc);

  int size = gst_buffer_get_size (buf);
  float time;
  guint32 time_ntp;
  getTime(filter,&time,&time_ntp);

  //g_print ("%d %d %d %d %d %f\n",
    //  size , pt, sn_h, ssrc_h, time_ntp, time);
  //gst_buffer_extract (buf, 0, pkt, size);
  if (filter->screamTx->getStreamQueue(ssrc_h) == 0) {
    g_print(" New stream, register !\n");

    switch (filter_->media_src) {
      case 0:
        filter->screamTx->registerNewStream(filter->rtpQueue, ssrc_h, 1.0f, 300e3f, 1000e3f, 15e6f, 5e6f, 0.3f, 0.2f, 0.1f, 0.2f);
        break;
      case 1:
        filter->screamTx->registerNewStream(filter->rtpQueue, ssrc_h, 1.0f, 300e3f, 500e3f, 5e6f, 1e6f, 0.1f, 0.2f, 0.1f, 0.2f);
        break;
      case 2:
        filter->screamTx->registerNewStream(filter->rtpQueue, ssrc_h, 1.0f, 300e3f, 1000e3f, 15e6f, 5e6f, 0.3f, 0.2f, 0.1f, 0.2f);
        break;
      case 3:
        filter->screamTx->registerNewStream(filter->rtpQueue, ssrc_h, 1.0f, 300e3f, 1000e3f, 15e6f, 5e6f, 0.3f, 0.2f, 0.1f, 0.2f);
        break;
      case 4:
        filter->screamTx->registerNewStream(filter->rtpQueue, ssrc_h, 1.0f, 300e3f, 1000e3f, 15e6f, 5e6f, 0.3f, 0.2f, 0.1f, 0.2f);
        break;
    }
  }

  pthread_mutex_lock(&filter->lock_rtp_queue);
  RtpQueue *rtpQ = (RtpQueue*)filter->screamTx->getStreamQueue(ssrc_h);
  rtpQ->push(buf,size,sn_h,time);
  pthread_mutex_unlock(&filter->lock_rtp_queue);

  pthread_mutex_lock(&filter->lock_scream);
  getTime(filter,&time,&time_ntp);
  filter->screamTx->newMediaFrame(time_ntp, ssrc_h, size);
  pthread_mutex_unlock(&filter->lock_scream);

  if (false && time-filter->lastRateChangeT > 0.1) {
            int rate = 1000;//1000*(1+ 3*(int(time/10) % 2));

            filter->lastRateChangeT = time;
            g_object_set(G_OBJECT(filter->encoder), "bitrate", rate, NULL);
            char buf2[1000];

            filter_->screamTx->getShortLog(time, buf2);
            g_print("%6.3f %s  %d\n",time,buf2, rate);

  }

#ifdef PACKET_PACING_OFF
  pthread_mutex_lock(&filter->lock_scream);
  getTime(filter,&time,&time_ntp);
  float retVal = filter->screamTx->isOkToTransmit(time_ntp, ssrc_h);
  pthread_mutex_unlock(&filter->lock_scream);

  GstFlowReturn returnValue = GST_FLOW_OK;

  while  (retVal == 0) {

    pthread_mutex_lock(&filter->lock_rtp_queue);
    GstBuffer *buf2 = rtpQ->pop(sn_h);
    pthread_mutex_unlock(&filter->lock_rtp_queue);

    int size = gst_buffer_get_size(buf2);
    returnValue = gst_pad_push(filter->srcpad, buf2);

    pthread_mutex_lock(&filter->lock_scream);
    getTime(filter,&time,&time_ntp);
    bool isMark = false; //XXX TODO: figure out correct value
    filter->screamTx->addTransmitted(time_ntp, ssrc_h, size, sn_h, isMark);
    retVal = filter->screamTx->isOkToTransmit(time_ntp, ssrc_h);
    pthread_mutex_unlock(&filter->lock_scream);

  }
#endif
  //g_print("NO TX \n");
  return GST_FLOW_OK;//returnValue;
  //once++;

  // Detta ^ bör ske i en annan tråd som kontinuerligt tar data ifrån rtpqueue och skickar datat till udpsocket.
}

/* this function handles sink events */
static gboolean
gst_g_scream_tx_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstgScreamTx *filter;
  gboolean ret;

  filter = GST_GSCREAMTX (parent);
  //g_print(" PTR3 %x\n", filter);
  GST_LOG_OBJECT (filter, "Received %s event: %", GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  if (filter->rtpSession == NULL) {
    GstElement *pipe = GST_ELEMENT_PARENT(parent);
    GstElement *rtpbin = gst_bin_get_by_name_recurse_up(GST_BIN(pipe), "rtpbin");
    g_assert(rtpbin);
    g_signal_emit_by_name(rtpbin,"get_internal_session", 0, &(filter->rtpSession));
    //g_print("RTPSESSION\n");
    //g_print("   PTR2 %x \n",  filter);
    filter_ = filter;
    /*
    * Odd feature... the last parameter is not kept .. strange...
    */

    g_object_set((filter->rtpSession), "rtcp-min-interval", 500000000, NULL);

    g_signal_connect_after((filter->rtpSession), "on-receiving-rtcp", G_CALLBACK(on_receiving_rtcp), filter);
    //g_print("CALLBACK\n");
    filter->encoder = gst_bin_get_by_name_recurse_up(GST_BIN(pipe), "video");
    g_assert(filter->encoder);
    
    
    //g_object_set(G_OBJECT(filter->encoder), "bitrate", 200, NULL);

    //filter->encoder = gst_bin_get_by_name_recurse_up(GST_BIN(pipe), "encoder");
    //g_assert(filter->encoder);



    GstClock *clock = gst_pipeline_get_clock((GstPipeline*) pipe);
    filter->clockId = gst_clock_new_periodic_id(clock, gst_clock_get_internal_time(clock), PACE_CLOCK_T_NS);
    g_assert(filter->clockId);
    GstClockReturn t = gst_clock_id_wait_async(filter->clockId, txTimerEvent, (gpointer) filter, NULL);

  }

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


static void task_print_loop(){
  g_print("Im inside task print loop");
}


/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
gscreamtx_init (GstPlugin * gscreamtx)
{
  /* debug category for fltering log messages
   *
   * exchange the string 'Template gscreamtx' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_g_scream_tx_debug, "gscreamtx",
      0, "Template gscreamtx");

  return gst_element_register (gscreamtx, "gscreamtx", GST_RANK_NONE,
      GST_TYPE_GSCREAMTX);
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "myfirstgscreamtx"
#endif

/* gstreamer looks for this structure to register gscreamtxs
 *
 * exchange the string 'Template gscreamtx' with your gscreamtx description
 */
 GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
                   GST_VERSION_MINOR,
                   gscreamtx,
                   "SCReAM sender side",
                   gscreamtx_init,
                   VERSION,
                   "LGPL",
                   "GStreamer",
                   "http://gstreamer.net/")
