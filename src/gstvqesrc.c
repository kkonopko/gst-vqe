/* GStreamer VQE element
 *
 * Copyright (C) 2012 YouView TV Ltd.
 *
 * Author: William Manley <william.manley@youview.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * or under the terms of the Cisco style BSD license.
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

/**
 * SECTION:element-vqesrc
 *
 * vqesrc is a network source that implements RTP including rapid channel
 * change and RTP retransmission.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 -v vqesrc sdp="xyz" ! decodebin2 ! xvimagesink
 * </refsect2>
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstvqesrc.h"

#include <gst/net/gstnetaddressmeta.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include <string.h>
#include <stdio.h>

GST_DEBUG_CATEGORY_STATIC (vqesrc_debug);
#define GST_CAT_DEFAULT (vqesrc_debug)

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/mpegts"));

#define VQE_DEFAULT_SDP                 ""
#define VQE_DEFAULT_CFG                 ""

static const size_t buffer_size = 4096; // 4KB

enum
{
  PROP_0,

  PROP_SDP,
  PROP_CFG,

  PROP_LAST
};

static GstFlowReturn gst_vqesrc_create (GstPushSrc * psrc, GstBuffer ** buf);

static gboolean gst_vqesrc_start (GstBaseSrc * bsrc);

static gboolean gst_vqesrc_stop (GstBaseSrc * bsrc);

static gboolean gst_vqesrc_unlock (GstBaseSrc * bsrc);

static gboolean gst_vqesrc_unlock_stop (GstBaseSrc * bsrc){ return FALSE; }

static void gst_vqesrc_finalize (GObject * object);

static void gst_vqesrc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_vqesrc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

#define gst_vqesrc_parent_class parent_class
G_DEFINE_TYPE (GstVQESrc, gst_vqesrc, GST_TYPE_PUSH_SRC);

static void
gst_vqesrc_class_init (GstVQESrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpushsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  GST_DEBUG_CATEGORY_INIT (vqesrc_debug, "vqesrc", 0, "VQE src");

  gobject_class->set_property = gst_vqesrc_set_property;
  gobject_class->get_property = gst_vqesrc_get_property;
  gobject_class->finalize = gst_vqesrc_finalize;

  g_object_class_install_property (gobject_class, PROP_SDP,
      g_param_spec_string ("sdp", "SDP",
          "Stream description in SDP format", VQE_DEFAULT_SDP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CFG,
      g_param_spec_string ("cfg", "Server Configuration file",
          "cfg in form of file path /tmp/sample-vqec.config", VQE_DEFAULT_CFG,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "RTP Receiver", "Source/Network",
      "Tune to multicast RTP streams",
      "William Manley <william.manley@youview.com>");

  gstbasesrc_class->start = gst_vqesrc_start;
  gstbasesrc_class->stop = gst_vqesrc_stop;
  gstbasesrc_class->unlock = gst_vqesrc_unlock;
  gstbasesrc_class->unlock_stop = gst_vqesrc_unlock_stop;

  gstpushsrc_class->create = gst_vqesrc_create;
}

static void
gst_vqesrc_init (GstVQESrc * vqesrc)
{
  vqesrc->sdp = g_strdup (VQE_DEFAULT_SDP);
  vqesrc->cfg = g_strdup (VQE_DEFAULT_CFG);

  // mutex used for control VQE worker thread
  g_rec_mutex_init ( &vqesrc->vqe_task_lock);

  /* configure basesrc to be a live source */
  gst_base_src_set_live (GST_BASE_SRC (vqesrc), TRUE);
  /* make basesrc output a segment in time */
  gst_base_src_set_format (GST_BASE_SRC (vqesrc), GST_FORMAT_TIME);
  /* make basesrc set timestamps on outgoing buffers based on the running_time
   * when they were captured */
  gst_base_src_set_do_timestamp (GST_BASE_SRC (vqesrc), TRUE);
}

static void
gst_vqesrc_finalize (GObject * object)
{
  GstVQESrc *vqesrc;

  vqesrc = GST_VQESRC (object);

  g_free (vqesrc->sdp);
  vqesrc->sdp = NULL;

  g_free (vqesrc->cfg);
  vqesrc->cfg = NULL;

  g_rec_mutex_clear ( &vqesrc->vqe_task_lock );

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstFlowReturn
gst_vqesrc_create (GstPushSrc * psrc, GstBuffer ** buf)
{
  GstVQESrc *vqesrc;
  GstBuffer *outbuf;

  vqesrc = GST_VQESRC_CAST (psrc);

  /* TODO: deal with cancellation somehow... Probably need to return
     GST_FLOW_FLUSHING */
  int32_t bytes_read = 0;
  vqec_iobuf_t buflist[1];
  memset(buflist, 0, sizeof(buflist));

  /* I know mallocing and freeing in the loop is a really bad idea from
     a performance perspective but I want to make this as easy to port to
     gstreamer as possible */
  buflist[0].buf_ptr = g_malloc(buffer_size);
  buflist[0].buf_len = buffer_size;
  if (!buflist[0].buf_ptr) {
    GST_ELEMENT_ERROR(GST_ELEMENT(vqesrc), RESOURCE,
                      FAILED, (NULL),
                      ("g_malloc(%i) failed", buffer_size));
    goto error;
  }

  vqec_error_t err = vqec_ifclient_tuner_recvmsg(vqesrc->tuner, buflist, 1, &bytes_read, 1000000);
  if (err) {
    GST_ELEMENT_ERROR(GST_ELEMENT(vqesrc), RESOURCE,
                      READ, (NULL),
                      ("Error receiving data from VQE: %s", vqec_err2str(err)));
    goto buf_error;
  }

  outbuf = gst_buffer_new_wrapped_full(0, buflist[0].buf_ptr, buffer_size, 0,
                                       bytes_read, buflist[0].buf_ptr, g_free);
  if (!outbuf) {
    GST_ELEMENT_ERROR(GST_ELEMENT(vqesrc), RESOURCE,
                      FAILED, (NULL),
                      ("gst_buffer_new_wrapped_full failed!"));
    goto buf_error;
  }

  /* Perhaps this is also a good idea: */
#if 0
  /* use buffer metadata so receivers can also track the address */
  if (saddr) {
    gst_buffer_add_net_address_meta (outbuf, saddr);
    g_object_unref (saddr);
  }
  saddr = NULL;
#endif

  *buf = outbuf;
  return GST_FLOW_OK;
buf_error:
    g_free(buflist[0].buf_ptr);
error:
    return GST_FLOW_ERROR;
}

static gboolean
gst_vqesrc_set_sdp (GstVQESrc * src, const gchar * sdp, GError ** error)
{
  /* Won't have any affect until the source is stopped and restarted. */
  /* TODO: A bit of preliminary validation of the SDP contents */
  g_free(src->sdp);
  src->sdp = g_strdup(sdp);
  return TRUE;
}

static gboolean
gst_vqesrc_set_cfg (GstVQESrc * src, const gchar * cfg, GError ** error)
{
  /* Won't have any affect until the source is stopped and restarted. */
  g_free(src->cfg);
  src->cfg = g_strdup(cfg);
  return TRUE;
}

static void
gst_vqesrc_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstVQESrc *vqesrc = GST_VQESRC (object);

  switch (prop_id) {
    case PROP_SDP:
      gst_vqesrc_set_sdp (vqesrc, g_value_get_string (value), NULL);
      break;
    case PROP_CFG:
      gst_vqesrc_set_cfg (vqesrc, g_value_get_string (value), NULL);
      break;
    default:
      break;
  }
}

static void
gst_vqesrc_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVQESrc *vqesrc = GST_VQESRC (object);

  switch (prop_id) {
    case PROP_SDP:
      g_value_set_string (value, vqesrc->sdp);
      break;
    case PROP_CFG:
      g_value_set_string (value, vqesrc->cfg);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
vqe_worker(void* pVqeSrc)
{
  /* Guaranteed to block until we call vqec_ifclient_stop */
  vqec_ifclient_start();

  // gst_task's repeated call worker function until stop is called
  // we want single shot behaviour.  Stop here so can use vqec_ifclient_stop
  // throughout code base without worrying about race conditions
  GstVQESrc* src = (GstVQESrc*) (pVqeSrc);
  GST_OBJECT_LOCK (src);
  gst_task_stop (src->vqe_task);
  GST_OBJECT_UNLOCK (src);
}

static gboolean
gst_vqesrc_tune (GstVQESrc * src, gchar* sdp)
{
  gboolean success = FALSE;
  vqec_chan_cfg_t cfg;
  /* bind params probably correspond to gstreamer properties?: */
  vqec_bind_params_t *bp = NULL;
  vqec_error_t err = 0;
  uint8_t res;

  bp = vqec_ifclient_bind_params_create();
  if (!bp) {
    GST_ELEMENT_ERROR(GST_ELEMENT(src), STREAM, FAILED, (NULL),
                      ("Failed to create bind params"));
    goto out;
  }
  res = vqec_ifclient_chan_cfg_parse_sdp(&cfg, sdp,
                                         VQEC_CHAN_TYPE_LINEAR);
  if (!res) {
    GST_ELEMENT_ERROR(GST_ELEMENT(src), STREAM, FAILED, (NULL),
                      ("Failed to parse SDP file:\n===BEGIN SDP===\n%s\n===END SDP===", sdp));
    goto out;
  }
  err = vqec_ifclient_tuner_bind_chan_cfg(src->tuner, &cfg, bp);
  if (err) {
    GST_ELEMENT_ERROR(GST_ELEMENT(src), STREAM, FAILED, (NULL),
                      ("Failed to bind channel: %s", vqec_err2str(err)));
    goto out;
  }
  success = TRUE;
out:
  if (bp) {
    vqec_ifclient_bind_params_destroy(bp);
  }
  return success;
}

/* create a socket for sending to remote machine */
static gboolean
gst_vqesrc_start (GstBaseSrc * bsrc)
{
  GstVQESrc *src;
  vqec_error_t err = 0;

  src = GST_VQESRC (bsrc);

  err = vqec_ifclient_init(src->cfg);
  if (err) {
    fprintf(stderr, "Failed to initialise VQE-C: %s\n", vqec_err2str(err));
    goto err;
  }

  /* TODO: Create vqetuner elements rather than vqesrc or vqebin */
  err = vqec_ifclient_tuner_create(&src->tuner, "mytuner");
  if (err) {
    fprintf(stderr, "Failed to create tuner: %s\n", vqec_err2str(err));
    goto err_inited;
  }
  gst_vqesrc_tune(src, src->sdp);

  GST_OBJECT_LOCK (src);

  src->vqe_task = gst_task_new ((GstTaskFunction) vqe_worker, src, NULL);
  if (src->vqe_task == NULL) {
	  GST_OBJECT_UNLOCK (src);
	  goto task_error;
  }
  else
  {
	  gst_task_set_lock (src->vqe_task, &src->vqe_task_lock );

	  gboolean started = gst_task_start (src->vqe_task);
	  GST_OBJECT_UNLOCK (src);

	  if( !started )
		  goto task_error;
  }

  return TRUE;

/* error handling
  vqec_ifclient_stop();
  gst_task_join(src->vqe_task);
  g_object_unref(G_OBJECT(src->vqe_task));
  src->vqe_task = NULL;*/

task_error:
//err_tuner:
  vqec_ifclient_tuner_destroy(src->tuner);
err_inited:
  vqec_ifclient_deinit();
err:
  return FALSE;
}

static gboolean
gst_vqesrc_unlock (GstBaseSrc * bsrc)
{
  GstVQESrc *src;

  src = GST_VQESRC (bsrc);

  GST_LOG_OBJECT (src, "Flushing");
  vqec_ifclient_stop();

  return TRUE;
}

static gboolean
gst_vqesrc_stop (GstBaseSrc * bsrc)
{
  GstVQESrc *src = GST_VQESRC (bsrc);

  // shutdown vqe worker thread
  GST_OBJECT_LOCK (src);
  if ( NULL != src->vqe_task )
  {
	  GST_OBJECT_UNLOCK (src);

	  // only call vqec_ifclient_stop, gst_task_stop is called behind the scenes
	  // src->vqe_task is needed by worker thread
	  vqec_ifclient_stop();
	  gst_task_join(src->vqe_task);
	  g_object_unref(G_OBJECT(src->vqe_task));
	  src->vqe_task = NULL;

  }
  else
	  GST_OBJECT_UNLOCK (src);


  vqec_ifclient_tuner_destroy(src->tuner);
  vqec_ifclient_deinit();
  return TRUE;
}

