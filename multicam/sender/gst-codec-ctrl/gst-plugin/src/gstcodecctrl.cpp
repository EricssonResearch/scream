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
#include "gstcodecctrl.h"
#include <stdio.h>
GST_DEBUG_CATEGORY_STATIC (gst_g_codecctrl_debug);
#define GST_CAT_DEFAULT gst_g_codecctrl_debug

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
  PROP_MEDIA_SRC,
  PROP_CTRL_PORT
};

pthread_t port_control_thread = 0;

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */

/* GObject vmethod implementations */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define BUFSIZE 100
void *readControlPortThread(void *arg) {
  /*
  * Wait for rate commands on control port
  */
  GstCodecCtrl *filter = GST_CODECCTRL (arg);
  guint8 buf[BUFSIZE];
  struct sockaddr_in incoming_cmd_addr;
  int fd_cmd;
  incoming_cmd_addr.sin_family = AF_INET;
  incoming_cmd_addr.sin_port = htons(filter->ctrl_port);
  incoming_cmd_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  socklen_t addrlen_incoming_cmd_addr = sizeof(incoming_cmd_addr);
  if ((fd_cmd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
     g_print("cannot create socket\n");
     return 0;
  }
  if (bind(fd_cmd, (struct sockaddr *)&incoming_cmd_addr, sizeof(incoming_cmd_addr)) < 0) {
      g_print("bind outgoing_rtp_addr failed\n");
      return 0;
  } else{
     g_print("Listen on control port %d\n", filter->ctrl_port);
  }

  int nn=0;
  for (;;) {
	   nn++;
    //int recvlen = recvfrom(fd_cmd, buf, BUFSIZE, 0, (struct sockaddr *)&incoming_cmd_addr, &addrlen_incoming_cmd_addr);
    int recvlen = recv(fd_cmd, buf, BUFSIZE, 0);
    if (recvlen > 0) {
      guint32 rate;
      memcpy(&rate, buf, 4);
      rate = ntohl(rate);
      //g_print("Rate command %d %d\n", filter->media_src, rate);
      switch (filter->media_src) {
        case 2:
        case 4:
        case 5:
		//rate *= 1000;
		break;
		default:
		rate /= 1000;
		break;
	  }
      switch (filter->media_src) {
        case 0:
        case 1:
        case 3:
        case 4:
        case 5:
          g_object_set(G_OBJECT(filter->encoder), "bitrate", rate, NULL);
          //g_object_set(G_OBJECT(filter->encoder), "peak-bitrate", rate, NULL);
          //g_print(" %d \n", rate);
        break;
        case 2:
          g_object_set(G_OBJECT(filter->encoder), "average-bitrate", rate, NULL);
          //g_object_set(G_OBJECT(filter->encoder), "peak-bitrate", int(rate*1.5), NULL);
        break;
      }
      if (true && filter->media_src == 4) {
		  /*
		   * Adaptive qp_min to to avoid large key frames at low bitrates
		   */
		  char s[100];
		  int qp_minI=1;
		  int qp_minP=1;
          int rateI = (rate+500000)/1000000;
		  if (true || rateI <= 12) {
			  /*
			   * Increased qp_min at low rates
			   *  reduces bitrate spikes
			   */
			  qp_minI=53-rateI/2;
			  //qp_minP=53-rateI;
			  if (qp_minI > 51) qp_minI = 51;
			  if (qp_minI < 0) qp_minI = 0;
		  }

		  sprintf(s,"%d,50:%d,50:-1,-1",qp_minP,qp_minI);
		  //g_print("%d %d %d \n", rate, qp_minP, qp_minI);
		  g_object_set(G_OBJECT(filter->encoder), "qp-range", s, NULL);


          if (true && buf[4] == 1 && rate > 3000000) {
			  GstFlowReturn ret;
              g_signal_emit_by_name (G_OBJECT(filter->encoder), "force-IDR", NULL, &ret);
              //g_print("Force IDR %d\n",nn);
		  }
	  }


    }

  }
  return NULL;
}

/* initialize the codecctrl's class */
static void
gst_g_codecctrl_class_init (GstCodecCtrlClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_g_codecctrl_set_property;
  gobject_class->get_property = gst_g_codecctrl_get_property;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output?",
          FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_MEDIA_SRC,
      g_param_spec_uint ("media-src", "Media source",
        "0=x264enc, 1=rpicamsrc, 2=uvch264src, 3=vaapih264enc, 4=omxh264enc, 5=opusenc",
        0, 5, 0,
        G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_CTRL_PORT,
      g_param_spec_uint ("port", "port",
        "",
        0, 65000, 0,
        G_PARAM_READWRITE));

  gst_element_class_set_details_simple(gstelement_class,
    "gCodecCtrl",
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
gst_g_codecctrl_init (GstCodecCtrl * filter)
{
  //g_print("INIT\n");
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  filter->encoder = NULL;
  filter->media_src = 0; // x264enc


  gst_pad_set_event_function (filter->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_g_codecctrl_sink_event));

  gst_pad_set_chain_function (filter->sinkpad,
                              GST_DEBUG_FUNCPTR(gst_g_codecctrl_chain));

  GST_PAD_SET_PROXY_CAPS (filter->sinkpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);


  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  GST_PAD_SET_PROXY_CAPS (filter->srcpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);
  //pthread_create(&port_control_thread,NULL,readControlPortThread,(void*)filter);

  filter->silent = TRUE;

}

static void
gst_g_codecctrl_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstCodecCtrl *filter = GST_CODECCTRL(object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    case PROP_MEDIA_SRC:
      filter->media_src = g_value_get_uint (value);
      break;
    case PROP_CTRL_PORT:
      filter->ctrl_port = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_g_codecctrl_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstCodecCtrl *filter = GST_CODECCTRL (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

}

/* this function handles sink events */
static gboolean
gst_g_codecctrl_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstCodecCtrl *filter;
  gboolean ret;

  filter = GST_CODECCTRL(parent);
  //g_print(" PTR3 %x\n", filter);
  GST_LOG_OBJECT (filter, "Received %s event: %", GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  if (filter->encoder == NULL) {
    GstElement *pipe = GST_ELEMENT_PARENT(parent);
    GstElement *rtpbin = gst_bin_get_by_name_recurse_up(GST_BIN(pipe), "rtpbin");
    g_assert(rtpbin);
    filter->encoder = gst_bin_get_by_name_recurse_up(GST_BIN(pipe), "video");
    g_assert(filter->encoder);
    pthread_create(&port_control_thread,NULL,readControlPortThread,(void*)filter);
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


static GstFlowReturn
gst_g_codecctrl_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstCodecCtrl *filter = GST_CODECCTRL (parent);

  filter = GST_CODECCTRL (parent);
  return gst_pad_push(filter->srcpad, buf);
}


static void task_print_loop(){
  g_print("Im inside task print loop");
}


/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
codecctrl_init (GstPlugin * codecctrl)
{
  /* debug category for fltering log messages
   *
   * exchange the string 'Template codecctrl' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_g_codecctrl_debug, "codecctrl",
      0, "Template codecctrl");

  return gst_element_register (codecctrl, "codecctrl", GST_RANK_NONE,
      GST_TYPE_CODECCTRL);
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "acodecctrl"
#endif

/* gstreamer looks for this structure to register codecctrl
 *
 * exchange the string 'Template codecctrl' with your codecctrl description
 */
 GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
                   GST_VERSION_MINOR,
                   codecctrl,
                   "Codec control",
                   codecctrl_init,
                   VERSION,
                   "LGPL",
                   "GStreamer",
                   "http://gstreamer.net/")
