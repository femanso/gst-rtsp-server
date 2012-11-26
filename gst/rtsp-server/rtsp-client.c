/* GStreamer
 * Copyright (C) 2008 Wim Taymans <wim.taymans at gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <string.h>

#include "rtsp-client.h"
#include "rtsp-sdp.h"
#include "rtsp-params.h"

static GMutex tunnels_lock;
static GHashTable *tunnels;

#define DEFAULT_SESSION_POOL            NULL
#define DEFAULT_MOUNT_POINTS            NULL
#define DEFAULT_USE_CLIENT_SETTINGS     FALSE

enum
{
  PROP_0,
  PROP_SESSION_POOL,
  PROP_MOUNT_POINTS,
  PROP_USE_CLIENT_SETTINGS,
  PROP_LAST
};

enum
{
  SIGNAL_CLOSED,
  SIGNAL_NEW_SESSION,
  SIGNAL_OPTIONS_REQUEST,
  SIGNAL_DESCRIBE_REQUEST,
  SIGNAL_SETUP_REQUEST,
  SIGNAL_PLAY_REQUEST,
  SIGNAL_PAUSE_REQUEST,
  SIGNAL_TEARDOWN_REQUEST,
  SIGNAL_SET_PARAMETER_REQUEST,
  SIGNAL_GET_PARAMETER_REQUEST,
  SIGNAL_LAST
};

GST_DEBUG_CATEGORY_STATIC (rtsp_client_debug);
#define GST_CAT_DEFAULT rtsp_client_debug

static guint gst_rtsp_client_signals[SIGNAL_LAST] = { 0 };

static void gst_rtsp_client_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec);
static void gst_rtsp_client_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec);
static void gst_rtsp_client_finalize (GObject * obj);

static GstSDPMessage *create_sdp (GstRTSPClient * client, GstRTSPMedia * media);
static void client_session_finalized (GstRTSPClient * client,
    GstRTSPSession * session);
static void unlink_session_transports (GstRTSPClient * client,
    GstRTSPSession * session, GstRTSPSessionMedia * media);

G_DEFINE_TYPE (GstRTSPClient, gst_rtsp_client, G_TYPE_OBJECT);

static void
gst_rtsp_client_class_init (GstRTSPClientClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = gst_rtsp_client_get_property;
  gobject_class->set_property = gst_rtsp_client_set_property;
  gobject_class->finalize = gst_rtsp_client_finalize;

  klass->create_sdp = create_sdp;

  g_object_class_install_property (gobject_class, PROP_SESSION_POOL,
      g_param_spec_object ("session-pool", "Session Pool",
          "The session pool to use for client session",
          GST_TYPE_RTSP_SESSION_POOL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MOUNT_POINTS,
      g_param_spec_object ("mount-points", "Mount Points",
          "The mount points to use for client session",
          GST_TYPE_RTSP_MOUNT_POINTS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_USE_CLIENT_SETTINGS,
      g_param_spec_boolean ("use-client-settings", "Use Client Settings",
          "Use client settings for ttl and destination in multicast",
          DEFAULT_USE_CLIENT_SETTINGS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_rtsp_client_signals[SIGNAL_CLOSED] =
      g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPClientClass, closed), NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0, G_TYPE_NONE);

  gst_rtsp_client_signals[SIGNAL_NEW_SESSION] =
      g_signal_new ("new-session", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPClientClass, new_session), NULL, NULL,
      g_cclosure_marshal_VOID__OBJECT, G_TYPE_NONE, 1, GST_TYPE_RTSP_SESSION);

  gst_rtsp_client_signals[SIGNAL_OPTIONS_REQUEST] =
      g_signal_new ("options-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, options_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_DESCRIBE_REQUEST] =
      g_signal_new ("describe-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, describe_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_SETUP_REQUEST] =
      g_signal_new ("setup-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, setup_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_PLAY_REQUEST] =
      g_signal_new ("play-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, play_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_PAUSE_REQUEST] =
      g_signal_new ("pause-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, pause_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_TEARDOWN_REQUEST] =
      g_signal_new ("teardown-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, teardown_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_SET_PARAMETER_REQUEST] =
      g_signal_new ("set-parameter-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass,
          set_parameter_request), NULL, NULL, g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_GET_PARAMETER_REQUEST] =
      g_signal_new ("get-parameter-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass,
          get_parameter_request), NULL, NULL, g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, G_TYPE_POINTER);

  tunnels =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  g_mutex_init (&tunnels_lock);

  GST_DEBUG_CATEGORY_INIT (rtsp_client_debug, "rtspclient", 0, "GstRTSPClient");
}

static void
gst_rtsp_client_init (GstRTSPClient * client)
{
  g_mutex_init (&client->lock);
  client->use_client_settings = DEFAULT_USE_CLIENT_SETTINGS;
  client->close_seq = 0;
}

static void
client_unlink_session (GstRTSPClient * client, GstRTSPSession * session)
{
  /* unlink all media managed in this session */
  while (session->medias) {
    GstRTSPSessionMedia *media = session->medias->data;

    gst_rtsp_session_media_set_state (media, GST_STATE_NULL);
    unlink_session_transports (client, session, media);
    /* unmanage the media in the session. this will modify session->medias */
    gst_rtsp_session_release_media (session, media);
  }
}

static void
client_cleanup_sessions (GstRTSPClient * client)
{
  GList *sessions;

  /* remove weak-ref from sessions */
  for (sessions = client->sessions; sessions; sessions = g_list_next (sessions)) {
    GstRTSPSession *session = (GstRTSPSession *) sessions->data;
    g_object_weak_unref (G_OBJECT (session),
        (GWeakNotify) client_session_finalized, client);
    client_unlink_session (client, session);
  }
  g_list_free (client->sessions);
  client->sessions = NULL;
}

/* A client is finalized when the connection is broken */
static void
gst_rtsp_client_finalize (GObject * obj)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (obj);

  GST_INFO ("finalize client %p", client);

  if (client->watch)
    g_source_destroy ((GSource *) client->watch);

  if (client->send_notify)
    client->send_notify (client->send_data);

  client_cleanup_sessions (client);

  gst_rtsp_connection_free (client->connection);
  if (client->session_pool)
    g_object_unref (client->session_pool);
  if (client->mount_points)
    g_object_unref (client->mount_points);
  if (client->auth)
    g_object_unref (client->auth);

  if (client->uri)
    gst_rtsp_url_free (client->uri);
  if (client->media) {
    gst_rtsp_media_unprepare (client->media);
    g_object_unref (client->media);
  }

  g_free (client->server_ip);
  g_mutex_clear (&client->lock);

  G_OBJECT_CLASS (gst_rtsp_client_parent_class)->finalize (obj);
}

static void
gst_rtsp_client_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (object);

  switch (propid) {
    case PROP_SESSION_POOL:
      g_value_take_object (value, gst_rtsp_client_get_session_pool (client));
      break;
    case PROP_MOUNT_POINTS:
      g_value_take_object (value, gst_rtsp_client_get_mount_points (client));
      break;
    case PROP_USE_CLIENT_SETTINGS:
      g_value_set_boolean (value,
          gst_rtsp_client_get_use_client_settings (client));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
gst_rtsp_client_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (object);

  switch (propid) {
    case PROP_SESSION_POOL:
      gst_rtsp_client_set_session_pool (client, g_value_get_object (value));
      break;
    case PROP_MOUNT_POINTS:
      gst_rtsp_client_set_mount_points (client, g_value_get_object (value));
      break;
    case PROP_USE_CLIENT_SETTINGS:
      gst_rtsp_client_set_use_client_settings (client,
          g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

/**
 * gst_rtsp_client_new:
 *
 * Create a new #GstRTSPClient instance.
 *
 * Returns: a new #GstRTSPClient
 */
GstRTSPClient *
gst_rtsp_client_new (void)
{
  GstRTSPClient *result;

  result = g_object_new (GST_TYPE_RTSP_CLIENT, NULL);

  return result;
}

static void
send_response (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPMessage * response, gboolean close)
{
  gst_rtsp_message_add_header (response, GST_RTSP_HDR_SERVER,
      "GStreamer RTSP server");

  /* remove any previous header */
  gst_rtsp_message_remove_header (response, GST_RTSP_HDR_SESSION, -1);

  /* add the new session header for new session ids */
  if (session) {
    gst_rtsp_message_take_header (response, GST_RTSP_HDR_SESSION,
        gst_rtsp_session_get_header (session));
  }

  if (gst_debug_category_get_threshold (rtsp_client_debug) >= GST_LEVEL_LOG) {
    gst_rtsp_message_dump (response);
  }

  if (close)
    gst_rtsp_message_add_header (response, GST_RTSP_HDR_CONNECTION, "close");

  if (client->send_func)
    client->send_func (client, response, close, client->send_data);

  gst_rtsp_message_unset (response);
}

static void
send_generic_response (GstRTSPClient * client, GstRTSPStatusCode code,
    GstRTSPClientState * state)
{
  gst_rtsp_message_init_response (state->response, code,
      gst_rtsp_status_as_text (code), state->request);

  send_response (client, NULL, state->response, FALSE);
}

static void
handle_unauthorized_request (GstRTSPClient * client, GstRTSPAuth * auth,
    GstRTSPClientState * state)
{
  gst_rtsp_message_init_response (state->response, GST_RTSP_STS_UNAUTHORIZED,
      gst_rtsp_status_as_text (GST_RTSP_STS_UNAUTHORIZED), state->request);

  if (auth) {
    /* and let the authentication manager setup the auth tokens */
    gst_rtsp_auth_setup_auth (auth, client, 0, state);
  }

  send_response (client, state->session, state->response, FALSE);
}


static gboolean
compare_uri (const GstRTSPUrl * uri1, const GstRTSPUrl * uri2)
{
  if (uri1 == NULL || uri2 == NULL)
    return FALSE;

  if (strcmp (uri1->abspath, uri2->abspath))
    return FALSE;

  return TRUE;
}

/* this function is called to initially find the media for the DESCRIBE request
 * but is cached for when the same client (without breaking the connection) is
 * doing a setup for the exact same url. */
static GstRTSPMedia *
find_media (GstRTSPClient * client, GstRTSPClientState * state)
{
  GstRTSPMediaFactory *factory;
  GstRTSPMedia *media;
  GstRTSPAuth *auth;

  if (!compare_uri (client->uri, state->uri)) {
    /* remove any previously cached values before we try to construct a new
     * media for uri */
    if (client->uri)
      gst_rtsp_url_free (client->uri);
    client->uri = NULL;
    if (client->media) {
      gst_rtsp_media_unprepare (client->media);
      g_object_unref (client->media);
    }
    client->media = NULL;

    if (!client->mount_points)
      goto no_mount_points;

    /* find the factory for the uri first */
    if (!(factory =
            gst_rtsp_mount_points_find_factory (client->mount_points,
                state->uri)))
      goto no_factory;

    state->factory = factory;

    /* check if we have access to the factory */
    if ((auth = gst_rtsp_media_factory_get_auth (factory))) {
      if (!gst_rtsp_auth_check (auth, client, 0, state))
        goto not_allowed;

      g_object_unref (auth);
    }

    /* prepare the media and add it to the pipeline */
    if (!(media = gst_rtsp_media_factory_construct (factory, state->uri)))
      goto no_media;

    g_object_unref (factory);
    factory = NULL;
    state->factory = NULL;

    /* set ipv6 on the media before preparing */
    media->is_ipv6 = client->is_ipv6;
    state->media = media;

    /* prepare the media */
    if (!(gst_rtsp_media_prepare (media)))
      goto no_prepare;

    /* now keep track of the uri and the media */
    client->uri = gst_rtsp_url_copy (state->uri);
    client->media = media;
  } else {
    /* we have seen this uri before, used cached media */
    media = client->media;
    state->media = media;
    GST_INFO ("reusing cached media %p", media);
  }

  if (media)
    g_object_ref (media);

  return media;

  /* ERRORS */
no_mount_points:
  {
    GST_ERROR ("client %p: no mount points configured", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, state);
    return NULL;
  }
no_factory:
  {
    GST_ERROR ("client %p: no factory for uri", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, state);
    return NULL;
  }
not_allowed:
  {
    GST_ERROR ("client %p: unauthorized request", client);
    handle_unauthorized_request (client, auth, state);
    g_object_unref (factory);
    g_object_unref (auth);
    return NULL;
  }
no_media:
  {
    GST_ERROR ("client %p: can't create media", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, state);
    g_object_unref (factory);
    return NULL;
  }
no_prepare:
  {
    GST_ERROR ("client %p: can't prepare media", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, state);
    g_object_unref (media);
    return NULL;
  }
}

static gboolean
do_send_data (GstBuffer * buffer, guint8 channel, GstRTSPClient * client)
{
  GstRTSPMessage message = { 0 };
  GstMapInfo map_info;
  guint8 *data;
  guint usize;

  gst_rtsp_message_init_data (&message, channel);

  /* FIXME, need some sort of iovec RTSPMessage here */
  if (!gst_buffer_map (buffer, &map_info, GST_MAP_READ))
    return FALSE;

  gst_rtsp_message_take_body (&message, map_info.data, map_info.size);

  if (client->send_func)
    client->send_func (client, &message, FALSE, client->send_data);

  gst_rtsp_message_steal_body (&message, &data, &usize);
  gst_buffer_unmap (buffer, &map_info);

  gst_rtsp_message_unset (&message);

  return TRUE;
}

static void
link_transport (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPStreamTransport * trans)
{
  GST_DEBUG ("client %p: linking transport %p", client, trans);
  gst_rtsp_stream_transport_set_callbacks (trans,
      (GstRTSPSendFunc) do_send_data,
      (GstRTSPSendFunc) do_send_data, client, NULL);

  client->transports = g_list_prepend (client->transports, trans);

  /* make sure our session can't expire */
  gst_rtsp_session_prevent_expire (session);
}

static void
unlink_transport (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPStreamTransport * trans)
{
  GST_DEBUG ("client %p: unlinking transport %p", client, trans);
  gst_rtsp_stream_transport_set_callbacks (trans, NULL, NULL, NULL, NULL);

  client->transports = g_list_remove (client->transports, trans);

  /* our session can now expire */
  gst_rtsp_session_allow_expire (session);
}

static void
unlink_session_transports (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPSessionMedia * media)
{
  guint n_streams, i;

  n_streams = gst_rtsp_media_n_streams (media->media);
  for (i = 0; i < n_streams; i++) {
    GstRTSPStreamTransport *trans;
    GstRTSPTransport *tr;

    /* get the transport, if there is no transport configured, skip this stream */
    trans = gst_rtsp_session_media_get_transport (media, i);
    if (trans == NULL)
      continue;

    tr = trans->transport;

    if (tr->lower_transport == GST_RTSP_LOWER_TRANS_TCP) {
      /* for TCP, unlink the stream from the TCP connection of the client */
      unlink_transport (client, session, trans);
    }
  }
}

static void
close_connection (GstRTSPClient * client)
{
  const gchar *tunnelid;

  GST_DEBUG ("client %p: closing connection", client);

  if ((tunnelid = gst_rtsp_connection_get_tunnelid (client->connection))) {
    g_mutex_lock (&tunnels_lock);
    /* remove from tunnelids */
    g_hash_table_remove (tunnels, tunnelid);
    g_mutex_unlock (&tunnels_lock);
  }

  gst_rtsp_connection_close (client->connection);
}

static gboolean
handle_teardown_request (GstRTSPClient * client, GstRTSPClientState * state)
{
  GstRTSPSession *session;
  GstRTSPSessionMedia *media;
  GstRTSPStatusCode code;

  if (!state->session)
    goto no_session;

  session = state->session;

  /* get a handle to the configuration of the media in the session */
  media = gst_rtsp_session_get_media (session, state->uri);
  if (!media)
    goto not_found;

  state->sessmedia = media;

  /* unlink the all TCP callbacks */
  unlink_session_transports (client, session, media);

  /* remove the session from the watched sessions */
  g_object_weak_unref (G_OBJECT (session),
      (GWeakNotify) client_session_finalized, client);
  client->sessions = g_list_remove (client->sessions, session);

  gst_rtsp_session_media_set_state (media, GST_STATE_NULL);

  /* unmanage the media in the session, returns false if all media session
   * are torn down. */
  if (!gst_rtsp_session_release_media (session, media)) {
    /* remove the session */
    gst_rtsp_session_pool_remove (client->session_pool, session);
  }
  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (state->response, code,
      gst_rtsp_status_as_text (code), state->request);

  send_response (client, session, state->response, TRUE);

  /* we emit the signal before closing the connection */
  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_TEARDOWN_REQUEST],
      0, state);

  return TRUE;

  /* ERRORS */
no_session:
  {
    GST_ERROR ("client %p: no session", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, state);
    return FALSE;
  }
not_found:
  {
    GST_ERROR ("client %p: no media for uri", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, state);
    return FALSE;
  }
}

static gboolean
handle_get_param_request (GstRTSPClient * client, GstRTSPClientState * state)
{
  GstRTSPResult res;
  guint8 *data;
  guint size;

  res = gst_rtsp_message_get_body (state->request, &data, &size);
  if (res != GST_RTSP_OK)
    goto bad_request;

  if (size == 0) {
    /* no body, keep-alive request */
    send_generic_response (client, GST_RTSP_STS_OK, state);
  } else {
    /* there is a body, handle the params */
    res = gst_rtsp_params_get (client, state);
    if (res != GST_RTSP_OK)
      goto bad_request;

    send_response (client, state->session, state->response, FALSE);
  }

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_GET_PARAMETER_REQUEST],
      0, state);

  return TRUE;

  /* ERRORS */
bad_request:
  {
    GST_ERROR ("client %p: bad request", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, state);
    return FALSE;
  }
}

static gboolean
handle_set_param_request (GstRTSPClient * client, GstRTSPClientState * state)
{
  GstRTSPResult res;
  guint8 *data;
  guint size;

  res = gst_rtsp_message_get_body (state->request, &data, &size);
  if (res != GST_RTSP_OK)
    goto bad_request;

  if (size == 0) {
    /* no body, keep-alive request */
    send_generic_response (client, GST_RTSP_STS_OK, state);
  } else {
    /* there is a body, handle the params */
    res = gst_rtsp_params_set (client, state);
    if (res != GST_RTSP_OK)
      goto bad_request;

    send_response (client, state->session, state->response, FALSE);
  }

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_SET_PARAMETER_REQUEST],
      0, state);

  return TRUE;

  /* ERRORS */
bad_request:
  {
    GST_ERROR ("client %p: bad request", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, state);
    return FALSE;
  }
}

static gboolean
handle_pause_request (GstRTSPClient * client, GstRTSPClientState * state)
{
  GstRTSPSession *session;
  GstRTSPSessionMedia *media;
  GstRTSPStatusCode code;

  if (!(session = state->session))
    goto no_session;

  /* get a handle to the configuration of the media in the session */
  media = gst_rtsp_session_get_media (session, state->uri);
  if (!media)
    goto not_found;

  state->sessmedia = media;

  /* the session state must be playing or recording */
  if (media->state != GST_RTSP_STATE_PLAYING &&
      media->state != GST_RTSP_STATE_RECORDING)
    goto invalid_state;

  /* unlink the all TCP callbacks */
  unlink_session_transports (client, session, media);

  /* then pause sending */
  gst_rtsp_session_media_set_state (media, GST_STATE_PAUSED);

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (state->response, code,
      gst_rtsp_status_as_text (code), state->request);

  send_response (client, session, state->response, FALSE);

  /* the state is now READY */
  media->state = GST_RTSP_STATE_READY;

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_PAUSE_REQUEST],
      0, state);

  return TRUE;

  /* ERRORS */
no_session:
  {
    GST_ERROR ("client %p: no seesion", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, state);
    return FALSE;
  }
not_found:
  {
    GST_ERROR ("client %p: no media for uri", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, state);
    return FALSE;
  }
invalid_state:
  {
    GST_ERROR ("client %p: not PLAYING or RECORDING", client);
    send_generic_response (client, GST_RTSP_STS_METHOD_NOT_VALID_IN_THIS_STATE,
        state);
    return FALSE;
  }
}

static gboolean
handle_play_request (GstRTSPClient * client, GstRTSPClientState * state)
{
  GstRTSPSession *session;
  GstRTSPSessionMedia *media;
  GstRTSPStatusCode code;
  GString *rtpinfo;
  guint n_streams, i, infocount;
  gchar *str;
  GstRTSPTimeRange *range;
  GstRTSPResult res;

  if (!(session = state->session))
    goto no_session;

  /* get a handle to the configuration of the media in the session */
  media = gst_rtsp_session_get_media (session, state->uri);
  if (!media)
    goto not_found;

  state->sessmedia = media;

  /* the session state must be playing or ready */
  if (media->state != GST_RTSP_STATE_PLAYING &&
      media->state != GST_RTSP_STATE_READY)
    goto invalid_state;

  /* parse the range header if we have one */
  res =
      gst_rtsp_message_get_header (state->request, GST_RTSP_HDR_RANGE, &str, 0);
  if (res == GST_RTSP_OK) {
    if (gst_rtsp_range_parse (str, &range) == GST_RTSP_OK) {
      /* we have a range, seek to the position */
      gst_rtsp_media_seek (media->media, range);
      gst_rtsp_range_free (range);
    }
  }

  /* grab RTPInfo from the payloaders now */
  rtpinfo = g_string_new ("");

  n_streams = gst_rtsp_media_n_streams (media->media);
  for (i = 0, infocount = 0; i < n_streams; i++) {
    GstRTSPStreamTransport *trans;
    GstRTSPTransport *tr;
    gchar *uristr;
    guint rtptime, seq;

    /* get the transport, if there is no transport configured, skip this stream */
    trans = gst_rtsp_session_media_get_transport (media, i);
    if (trans == NULL) {
      GST_INFO ("stream %d is not configured", i);
      continue;
    }
    tr = trans->transport;

    if (tr->lower_transport == GST_RTSP_LOWER_TRANS_TCP) {
      /* for TCP, link the stream to the TCP connection of the client */
      link_transport (client, session, trans);
    }

    if (gst_rtsp_stream_get_rtpinfo (trans->stream, &rtptime, &seq)) {
      if (infocount > 0)
        g_string_append (rtpinfo, ", ");

      uristr = gst_rtsp_url_get_request_uri (state->uri);
      g_string_append_printf (rtpinfo, "url=%s/stream=%d;seq=%u;rtptime=%u",
          uristr, i, seq, rtptime);
      g_free (uristr);

      infocount++;
    } else {
      GST_WARNING ("RTP-Info cannot be determined for stream %d", i);
    }
  }

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (state->response, code,
      gst_rtsp_status_as_text (code), state->request);

  /* add the RTP-Info header */
  if (infocount > 0) {
    str = g_string_free (rtpinfo, FALSE);
    gst_rtsp_message_take_header (state->response, GST_RTSP_HDR_RTP_INFO, str);
  } else {
    g_string_free (rtpinfo, TRUE);
  }

  /* add the range */
  str = gst_rtsp_media_get_range_string (media->media, TRUE);
  gst_rtsp_message_take_header (state->response, GST_RTSP_HDR_RANGE, str);

  send_response (client, session, state->response, FALSE);

  /* start playing after sending the request */
  gst_rtsp_session_media_set_state (media, GST_STATE_PLAYING);

  media->state = GST_RTSP_STATE_PLAYING;

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_PLAY_REQUEST],
      0, state);

  return TRUE;

  /* ERRORS */
no_session:
  {
    GST_ERROR ("client %p: no session", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, state);
    return FALSE;
  }
not_found:
  {
    GST_ERROR ("client %p: media not found", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, state);
    return FALSE;
  }
invalid_state:
  {
    GST_ERROR ("client %p: not PLAYING or READY", client);
    send_generic_response (client, GST_RTSP_STS_METHOD_NOT_VALID_IN_THIS_STATE,
        state);
    return FALSE;
  }
}

static void
do_keepalive (GstRTSPSession * session)
{
  GST_INFO ("keep session %p alive", session);
  gst_rtsp_session_touch (session);
}

/* parse @transport and return a valid transport in @tr. only transports
 * from @supported are returned. Returns FALSE if no valid transport
 * was found. */
static gboolean
parse_transport (const char *transport, GstRTSPLowerTrans supported,
    GstRTSPTransport * tr)
{
  gint i;
  gboolean res;
  gchar **transports;

  res = FALSE;
  gst_rtsp_transport_init (tr);

  GST_DEBUG ("parsing transports %s", transport);

  transports = g_strsplit (transport, ",", 0);

  /* loop through the transports, try to parse */
  for (i = 0; transports[i]; i++) {
    res = gst_rtsp_transport_parse (transports[i], tr);
    if (res != GST_RTSP_OK) {
      /* no valid transport, search some more */
      GST_WARNING ("could not parse transport %s", transports[i]);
      goto next;
    }

    /* we have a transport, see if it's RTP/AVP */
    if (tr->trans != GST_RTSP_TRANS_RTP || tr->profile != GST_RTSP_PROFILE_AVP) {
      GST_WARNING ("invalid transport %s", transports[i]);
      goto next;
    }

    if (!(tr->lower_transport & supported)) {
      GST_WARNING ("unsupported transport %s", transports[i]);
      goto next;
    }

    /* we have a valid transport */
    GST_INFO ("found valid transport %s", transports[i]);
    res = TRUE;
    break;

  next:
    gst_rtsp_transport_init (tr);
  }
  g_strfreev (transports);

  return res;
}

static gboolean
handle_blocksize (GstRTSPMedia * media, GstRTSPStream * stream,
    GstRTSPMessage * request)
{
  gchar *blocksize_str;
  gboolean ret = TRUE;

  if (gst_rtsp_message_get_header (request, GST_RTSP_HDR_BLOCKSIZE,
          &blocksize_str, 0) == GST_RTSP_OK) {
    guint64 blocksize;
    gchar *end;

    blocksize = g_ascii_strtoull (blocksize_str, &end, 10);
    if (end == blocksize_str) {
      GST_ERROR ("failed to parse blocksize");
      ret = FALSE;
    } else {
      /* we don't want to change the mtu when this media
       * can be shared because it impacts other clients */
      if (gst_rtsp_media_is_shared (media))
        return TRUE;

      if (blocksize > G_MAXUINT)
        blocksize = G_MAXUINT;
      gst_rtsp_stream_set_mtu (stream, blocksize);
    }
  }
  return ret;
}

static gboolean
configure_client_transport (GstRTSPClient * client, GstRTSPClientState * state,
    GstRTSPTransport * ct)
{
  /* we have a valid transport now, set the destination of the client. */
  if (ct->lower_transport == GST_RTSP_LOWER_TRANS_UDP_MCAST) {
    if (ct->destination == NULL || !client->use_client_settings) {
      GstRTSPAddress *addr;

      addr = gst_rtsp_stream_get_address (state->stream);
      if (addr == NULL)
        goto no_address;

      g_free (ct->destination);
      ct->destination = g_strdup (addr->address);
      ct->port.min = addr->port;
      ct->port.max = addr->port + addr->n_ports - 1;
      ct->ttl = addr->ttl;
    }
  } else {
    GstRTSPUrl *url;

    url = gst_rtsp_connection_get_url (client->connection);
    g_free (ct->destination);
    ct->destination = g_strdup (url->host);

    if (ct->lower_transport & GST_RTSP_LOWER_TRANS_TCP) {
      /* check if the client selected channels for TCP */
      if (ct->interleaved.min == -1 || ct->interleaved.max == -1) {
        gst_rtsp_session_media_alloc_channels (state->sessmedia,
            &ct->interleaved);
      }
    }
  }
  return TRUE;

  /* ERRORS */
no_address:
  {
    GST_ERROR_OBJECT (client, "failed to acquire address for stream");
    return FALSE;
  }
}

static GstRTSPTransport *
make_server_transport (GstRTSPClient * client, GstRTSPClientState * state,
    GstRTSPTransport * ct)
{
  GstRTSPTransport *st;

  /* prepare the server transport */
  gst_rtsp_transport_new (&st);

  st->trans = ct->trans;
  st->profile = ct->profile;
  st->lower_transport = ct->lower_transport;

  switch (st->lower_transport) {
    case GST_RTSP_LOWER_TRANS_UDP:
      st->client_port = ct->client_port;
      st->server_port = state->stream->server_port;
      break;
    case GST_RTSP_LOWER_TRANS_UDP_MCAST:
      st->port = ct->port;
      st->destination = g_strdup (ct->destination);
      st->ttl = ct->ttl;
      break;
    case GST_RTSP_LOWER_TRANS_TCP:
      st->interleaved = ct->interleaved;
    default:
      break;
  }

  if (state->stream->session)
    g_object_get (state->stream->session, "internal-ssrc", &st->ssrc, NULL);

  return st;
}

static gboolean
handle_setup_request (GstRTSPClient * client, GstRTSPClientState * state)
{
  GstRTSPResult res;
  GstRTSPUrl *uri;
  gchar *transport;
  GstRTSPTransport *ct, *st;
  GstRTSPLowerTrans supported;
  GstRTSPStatusCode code;
  GstRTSPSession *session;
  GstRTSPStreamTransport *trans;
  gchar *trans_str, *pos;
  guint streamid;
  GstRTSPSessionMedia *sessmedia;
  GstRTSPMedia *media;
  GstRTSPStream *stream;

  uri = state->uri;

  /* the uri contains the stream number we added in the SDP config, which is
   * always /stream=%d so we need to strip that off
   * parse the stream we need to configure, look for the stream in the abspath
   * first and then in the query. */
  if (uri->abspath == NULL || !(pos = strstr (uri->abspath, "/stream="))) {
    if (uri->query == NULL || !(pos = strstr (uri->query, "/stream=")))
      goto bad_request;
  }

  /* we can mofify the parsed uri in place */
  *pos = '\0';

  pos += strlen ("/stream=");
  if (sscanf (pos, "%u", &streamid) != 1)
    goto bad_request;

  /* parse the transport */
  res =
      gst_rtsp_message_get_header (state->request, GST_RTSP_HDR_TRANSPORT,
      &transport, 0);
  if (res != GST_RTSP_OK)
    goto no_transport;

  gst_rtsp_transport_new (&ct);

  /* our supported transports */
  supported = GST_RTSP_LOWER_TRANS_UDP |
      GST_RTSP_LOWER_TRANS_UDP_MCAST | GST_RTSP_LOWER_TRANS_TCP;

  /* parse and find a usable supported transport */
  if (!parse_transport (transport, supported, ct))
    goto unsupported_transports;

  /* we create the session after parsing stuff so that we don't make
   * a session for malformed requests */
  if (client->session_pool == NULL)
    goto no_pool;

  session = state->session;

  if (session) {
    g_object_ref (session);
    /* get a handle to the configuration of the media in the session, this can
     * return NULL if this is a new url to manage in this session. */
    sessmedia = gst_rtsp_session_get_media (session, uri);
  } else {
    /* create a session if this fails we probably reached our session limit or
     * something. */
    if (!(session = gst_rtsp_session_pool_create (client->session_pool)))
      goto service_unavailable;

    state->session = session;

    /* we need a new media configuration in this session */
    sessmedia = NULL;
  }

  /* we have no media, find one and manage it */
  if (sessmedia == NULL) {
    /* get a handle to the configuration of the media in the session */
    if ((media = find_media (client, state))) {
      /* manage the media in our session now */
      sessmedia = gst_rtsp_session_manage_media (session, uri, media);
    }
  }

  /* if we stil have no media, error */
  if (sessmedia == NULL)
    goto not_found;

  state->sessmedia = sessmedia;
  state->media = media = sessmedia->media;

  /* now get the stream */
  stream = gst_rtsp_media_get_stream (media, streamid);
  if (stream == NULL)
    goto not_found;

  state->stream = stream;

  /* set blocksize on this stream */
  if (!handle_blocksize (media, stream, state->request))
    goto invalid_blocksize;

  /* update the client transport */
  if (!configure_client_transport (client, state, ct))
    goto unsupported_client_transport;

  /* set in the session media transport */
  trans = gst_rtsp_session_media_set_transport (sessmedia, stream, ct);

  /* configure keepalive for this transport */
  gst_rtsp_stream_transport_set_keepalive (trans,
      (GstRTSPKeepAliveFunc) do_keepalive, session, NULL);

  /* create and serialize the server transport */
  st = make_server_transport (client, state, ct);
  trans_str = gst_rtsp_transport_as_text (st);
  gst_rtsp_transport_free (st);

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (state->response, code,
      gst_rtsp_status_as_text (code), state->request);

  gst_rtsp_message_add_header (state->response, GST_RTSP_HDR_TRANSPORT,
      trans_str);
  g_free (trans_str);

  send_response (client, session, state->response, FALSE);

  /* update the state */
  switch (sessmedia->state) {
    case GST_RTSP_STATE_PLAYING:
    case GST_RTSP_STATE_RECORDING:
    case GST_RTSP_STATE_READY:
      /* no state change */
      break;
    default:
      sessmedia->state = GST_RTSP_STATE_READY;
      break;
  }
  g_object_unref (session);

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_SETUP_REQUEST],
      0, state);

  return TRUE;

  /* ERRORS */
bad_request:
  {
    GST_ERROR ("client %p: bad request", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, state);
    return FALSE;
  }
not_found:
  {
    GST_ERROR ("client %p: media not found", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, state);
    g_object_unref (session);
    gst_rtsp_transport_free (ct);
    return FALSE;
  }
invalid_blocksize:
  {
    GST_ERROR ("client %p: invalid blocksize", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, state);
    g_object_unref (session);
    gst_rtsp_transport_free (ct);
    return FALSE;
  }
unsupported_client_transport:
  {
    GST_ERROR ("client %p: unsupported client transport", client);
    send_generic_response (client, GST_RTSP_STS_UNSUPPORTED_TRANSPORT, state);
    g_object_unref (session);
    gst_rtsp_transport_free (ct);
    return FALSE;
  }
no_transport:
  {
    GST_ERROR ("client %p: no transport", client);
    send_generic_response (client, GST_RTSP_STS_UNSUPPORTED_TRANSPORT, state);
    return FALSE;
  }
unsupported_transports:
  {
    GST_ERROR ("client %p: unsupported transports", client);
    send_generic_response (client, GST_RTSP_STS_UNSUPPORTED_TRANSPORT, state);
    gst_rtsp_transport_free (ct);
    return FALSE;
  }
no_pool:
  {
    GST_ERROR ("client %p: no session pool configured", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, state);
    gst_rtsp_transport_free (ct);
    return FALSE;
  }
service_unavailable:
  {
    GST_ERROR ("client %p: can't create session", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, state);
    gst_rtsp_transport_free (ct);
    return FALSE;
  }
}

static GstSDPMessage *
create_sdp (GstRTSPClient * client, GstRTSPMedia * media)
{
  GstSDPMessage *sdp;
  GstSDPInfo info;
  const gchar *proto;

  gst_sdp_message_new (&sdp);

  /* some standard things first */
  gst_sdp_message_set_version (sdp, "0");

  if (client->is_ipv6)
    proto = "IP6";
  else
    proto = "IP4";

  gst_sdp_message_set_origin (sdp, "-", "1188340656180883", "1", "IN", proto,
      client->server_ip);

  gst_sdp_message_set_session_name (sdp, "Session streamed with GStreamer");
  gst_sdp_message_set_information (sdp, "rtsp-server");
  gst_sdp_message_add_time (sdp, "0", "0", NULL);
  gst_sdp_message_add_attribute (sdp, "tool", "GStreamer");
  gst_sdp_message_add_attribute (sdp, "type", "broadcast");
  gst_sdp_message_add_attribute (sdp, "control", "*");

  info.server_proto = proto;
  info.server_ip = g_strdup (client->server_ip);

  /* create an SDP for the media object */
  if (!gst_rtsp_sdp_from_media (sdp, &info, media))
    goto no_sdp;

  g_free (info.server_ip);

  return sdp;

  /* ERRORS */
no_sdp:
  {
    GST_ERROR ("client %p: could not create SDP", client);
    g_free (info.server_ip);
    gst_sdp_message_free (sdp);
    return NULL;
  }
}

/* for the describe we must generate an SDP */
static gboolean
handle_describe_request (GstRTSPClient * client, GstRTSPClientState * state)
{
  GstRTSPResult res;
  GstSDPMessage *sdp;
  guint i, str_len;
  gchar *str, *content_base;
  GstRTSPMedia *media;
  GstRTSPClientClass *klass;

  klass = GST_RTSP_CLIENT_GET_CLASS (client);

  /* check what kind of format is accepted, we don't really do anything with it
   * and always return SDP for now. */
  for (i = 0; i++;) {
    gchar *accept;

    res =
        gst_rtsp_message_get_header (state->request, GST_RTSP_HDR_ACCEPT,
        &accept, i);
    if (res == GST_RTSP_ENOTIMPL)
      break;

    if (g_ascii_strcasecmp (accept, "application/sdp") == 0)
      break;
  }

  /* find the media object for the uri */
  if (!(media = find_media (client, state)))
    goto no_media;

  /* create an SDP for the media object on this client */
  if (!(sdp = klass->create_sdp (client, media)))
    goto no_sdp;

  g_object_unref (media);

  gst_rtsp_message_init_response (state->response, GST_RTSP_STS_OK,
      gst_rtsp_status_as_text (GST_RTSP_STS_OK), state->request);

  gst_rtsp_message_add_header (state->response, GST_RTSP_HDR_CONTENT_TYPE,
      "application/sdp");

  /* content base for some clients that might screw up creating the setup uri */
  str = gst_rtsp_url_get_request_uri (state->uri);
  str_len = strlen (str);

  /* check for trailing '/' and append one */
  if (str[str_len - 1] != '/') {
    content_base = g_malloc (str_len + 2);
    memcpy (content_base, str, str_len);
    content_base[str_len] = '/';
    content_base[str_len + 1] = '\0';
    g_free (str);
  } else {
    content_base = str;
  }

  GST_INFO ("adding content-base: %s", content_base);

  gst_rtsp_message_add_header (state->response, GST_RTSP_HDR_CONTENT_BASE,
      content_base);
  g_free (content_base);

  /* add SDP to the response body */
  str = gst_sdp_message_as_text (sdp);
  gst_rtsp_message_take_body (state->response, (guint8 *) str, strlen (str));
  gst_sdp_message_free (sdp);

  send_response (client, state->session, state->response, FALSE);

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_DESCRIBE_REQUEST],
      0, state);

  return TRUE;

  /* ERRORS */
no_media:
  {
    GST_ERROR ("client %p: no media", client);
    /* error reply is already sent */
    return FALSE;
  }
no_sdp:
  {
    GST_ERROR ("client %p: can't create SDP", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, state);
    g_object_unref (media);
    return FALSE;
  }
}

static gboolean
handle_options_request (GstRTSPClient * client, GstRTSPClientState * state)
{
  GstRTSPMethod options;
  gchar *str;

  options = GST_RTSP_DESCRIBE |
      GST_RTSP_OPTIONS |
      GST_RTSP_PAUSE |
      GST_RTSP_PLAY |
      GST_RTSP_SETUP |
      GST_RTSP_GET_PARAMETER | GST_RTSP_SET_PARAMETER | GST_RTSP_TEARDOWN;

  str = gst_rtsp_options_as_text (options);

  gst_rtsp_message_init_response (state->response, GST_RTSP_STS_OK,
      gst_rtsp_status_as_text (GST_RTSP_STS_OK), state->request);

  gst_rtsp_message_add_header (state->response, GST_RTSP_HDR_PUBLIC, str);
  g_free (str);

  send_response (client, state->session, state->response, FALSE);

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_OPTIONS_REQUEST],
      0, state);

  return TRUE;
}

/* remove duplicate and trailing '/' */
static void
sanitize_uri (GstRTSPUrl * uri)
{
  gint i, len;
  gchar *s, *d;
  gboolean have_slash, prev_slash;

  s = d = uri->abspath;
  len = strlen (uri->abspath);

  prev_slash = FALSE;

  for (i = 0; i < len; i++) {
    have_slash = s[i] == '/';
    *d = s[i];
    if (!have_slash || !prev_slash)
      d++;
    prev_slash = have_slash;
  }
  len = d - uri->abspath;
  /* don't remove the first slash if that's the only thing left */
  if (len > 1 && *(d - 1) == '/')
    d--;
  *d = '\0';
}

static void
client_session_finalized (GstRTSPClient * client, GstRTSPSession * session)
{
  GST_INFO ("client %p: session %p finished", client, session);

  /* unlink all media managed in this session */
  client_unlink_session (client, session);

  /* remove the session */
  if (!(client->sessions = g_list_remove (client->sessions, session))) {
    GST_INFO ("client %p: all sessions finalized, close the connection",
        client);
    close_connection (client);
  }
}

static void
client_watch_session (GstRTSPClient * client, GstRTSPSession * session)
{
  GList *walk;

  for (walk = client->sessions; walk; walk = g_list_next (walk)) {
    GstRTSPSession *msession = (GstRTSPSession *) walk->data;

    /* we already know about this session */
    if (msession == session)
      return;
  }

  GST_INFO ("watching session %p", session);

  g_object_weak_ref (G_OBJECT (session), (GWeakNotify) client_session_finalized,
      client);
  client->sessions = g_list_prepend (client->sessions, session);

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_NEW_SESSION], 0,
      session);
}

static void
handle_request (GstRTSPClient * client, GstRTSPMessage * request)
{
  GstRTSPMethod method;
  const gchar *uristr;
  GstRTSPUrl *uri;
  GstRTSPVersion version;
  GstRTSPResult res;
  GstRTSPSession *session;
  GstRTSPClientState state = { NULL };
  GstRTSPMessage response = { 0 };
  gchar *sessid;

  state.request = request;
  state.response = &response;

  if (gst_debug_category_get_threshold (rtsp_client_debug) >= GST_LEVEL_LOG) {
    gst_rtsp_message_dump (request);
  }

  GST_INFO ("client %p: received a request", client);

  gst_rtsp_message_parse_request (request, &method, &uristr, &version);

  if (version != GST_RTSP_VERSION_1_0) {
    /* we can only handle 1.0 requests */
    send_generic_response (client, GST_RTSP_STS_RTSP_VERSION_NOT_SUPPORTED,
        &state);
    return;
  }
  state.method = method;

  /* we always try to parse the url first */
  if (gst_rtsp_url_parse (uristr, &uri) != GST_RTSP_OK) {
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, &state);
    return;
  }

  /* sanitize the uri */
  sanitize_uri (uri);
  state.uri = uri;

  /* get the session if there is any */
  res = gst_rtsp_message_get_header (request, GST_RTSP_HDR_SESSION, &sessid, 0);
  if (res == GST_RTSP_OK) {
    if (client->session_pool == NULL)
      goto no_pool;

    /* we had a session in the request, find it again */
    if (!(session = gst_rtsp_session_pool_find (client->session_pool, sessid)))
      goto session_not_found;

    /* we add the session to the client list of watched sessions. When a session
     * disappears because it times out, we will be notified. If all sessions are
     * gone, we will close the connection */
    client_watch_session (client, session);
  } else
    session = NULL;

  state.session = session;

  if (client->auth) {
    if (!gst_rtsp_auth_check (client->auth, client, 0, &state))
      goto not_authorized;
  }

  /* now see what is asked and dispatch to a dedicated handler */
  switch (method) {
    case GST_RTSP_OPTIONS:
      handle_options_request (client, &state);
      break;
    case GST_RTSP_DESCRIBE:
      handle_describe_request (client, &state);
      break;
    case GST_RTSP_SETUP:
      handle_setup_request (client, &state);
      break;
    case GST_RTSP_PLAY:
      handle_play_request (client, &state);
      break;
    case GST_RTSP_PAUSE:
      handle_pause_request (client, &state);
      break;
    case GST_RTSP_TEARDOWN:
      handle_teardown_request (client, &state);
      break;
    case GST_RTSP_SET_PARAMETER:
      handle_set_param_request (client, &state);
      break;
    case GST_RTSP_GET_PARAMETER:
      handle_get_param_request (client, &state);
      break;
    case GST_RTSP_ANNOUNCE:
    case GST_RTSP_RECORD:
    case GST_RTSP_REDIRECT:
      send_generic_response (client, GST_RTSP_STS_NOT_IMPLEMENTED, &state);
      break;
    case GST_RTSP_INVALID:
    default:
      send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, &state);
      break;
  }
  if (session)
    g_object_unref (session);

  gst_rtsp_url_free (uri);
  return;

  /* ERRORS */
no_pool:
  {
    GST_ERROR ("client %p: no pool configured", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, &state);
    return;
  }
session_not_found:
  {
    GST_ERROR ("client %p: session not found", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, &state);
    return;
  }
not_authorized:
  {
    GST_ERROR ("client %p: not allowed", client);
    handle_unauthorized_request (client, client->auth, &state);
    return;
  }
}

static void
handle_data (GstRTSPClient * client, GstRTSPMessage * message)
{
  GstRTSPResult res;
  guint8 channel;
  GList *walk;
  guint8 *data;
  guint size;
  GstBuffer *buffer;
  gboolean handled;

  /* find the stream for this message */
  res = gst_rtsp_message_parse_data (message, &channel);
  if (res != GST_RTSP_OK)
    return;

  gst_rtsp_message_steal_body (message, &data, &size);

  buffer = gst_buffer_new_wrapped (data, size);

  handled = FALSE;
  for (walk = client->transports; walk; walk = g_list_next (walk)) {
    GstRTSPStreamTransport *trans;
    GstRTSPStream *stream;
    GstRTSPTransport *tr;

    trans = walk->data;

    /* we only add clients with a transport to the list */
    tr = trans->transport;
    stream = trans->stream;

    /* check for TCP transport */
    if (tr->lower_transport == GST_RTSP_LOWER_TRANS_TCP) {
      /* dispatch to the stream based on the channel number */
      if (tr->interleaved.min == channel) {
        gst_rtsp_stream_recv_rtp (stream, buffer);
        handled = TRUE;
        break;
      } else if (tr->interleaved.max == channel) {
        gst_rtsp_stream_recv_rtcp (stream, buffer);
        handled = TRUE;
        break;
      }
    }
  }
  if (!handled)
    gst_buffer_unref (buffer);
}

/**
 * gst_rtsp_client_set_session_pool:
 * @client: a #GstRTSPClient
 * @pool: a #GstRTSPSessionPool
 *
 * Set @pool as the sessionpool for @client which it will use to find
 * or allocate sessions. the sessionpool is usually inherited from the server
 * that created the client but can be overridden later.
 */
void
gst_rtsp_client_set_session_pool (GstRTSPClient * client,
    GstRTSPSessionPool * pool)
{
  GstRTSPSessionPool *old;

  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  if (pool)
    g_object_ref (pool);

  g_mutex_lock (&client->lock);
  old = client->session_pool;
  client->session_pool = pool;
  g_mutex_unlock (&client->lock);

  if (old)
    g_object_unref (old);
}

/**
 * gst_rtsp_client_get_session_pool:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPSessionPool object that @client uses to manage its sessions.
 *
 * Returns: (transfer full): a #GstRTSPSessionPool, unref after usage.
 */
GstRTSPSessionPool *
gst_rtsp_client_get_session_pool (GstRTSPClient * client)
{
  GstRTSPSessionPool *result;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), NULL);

  g_mutex_lock (&client->lock);
  if ((result = client->session_pool))
    g_object_ref (result);
  g_mutex_unlock (&client->lock);

  return result;
}

/**
 * gst_rtsp_client_set_mount_points:
 * @client: a #GstRTSPClient
 * @mounts: a #GstRTSPMountPoints
 *
 * Set @mounts as the mount points for @client which it will use to map urls
 * to media streams. These mount points are usually inherited from the server that
 * created the client but can be overriden later.
 */
void
gst_rtsp_client_set_mount_points (GstRTSPClient * client,
    GstRTSPMountPoints * mounts)
{
  GstRTSPMountPoints *old;

  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  if (mounts)
    g_object_ref (mounts);

  g_mutex_lock (&client->lock);
  old = client->mount_points;
  client->mount_points = mounts;
  g_mutex_unlock (&client->lock);

  if (old)
    g_object_unref (old);
}

/**
 * gst_rtsp_client_get_mount_points:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPMountPoints object that @client uses to manage its sessions.
 *
 * Returns: (transfer full): a #GstRTSPMountPoints, unref after usage.
 */
GstRTSPMountPoints *
gst_rtsp_client_get_mount_points (GstRTSPClient * client)
{
  GstRTSPMountPoints *result;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), NULL);

  g_mutex_lock (&client->lock);
  if ((result = client->mount_points))
    g_object_ref (result);
  g_mutex_unlock (&client->lock);

  return result;
}

/**
 * gst_rtsp_client_set_use_client_settings:
 * @client: a #GstRTSPClient
 * @use_client_settings: whether to use client settings for multicast
 *
 * Use client transport settings (destination and ttl) for multicast.
 * When @use_client_settings is %FALSE, the server settings will be
 * used.
 */
void
gst_rtsp_client_set_use_client_settings (GstRTSPClient * client,
    gboolean use_client_settings)
{
  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  g_mutex_lock (&client->lock);
  client->use_client_settings = use_client_settings;
  g_mutex_unlock (&client->lock);
}

/**
 * gst_rtsp_client_get_use_client_settings:
 * @client: a #GstRTSPClient
 *
 * Check if client transport settings (destination and ttl) for multicast
 * will be used.
 */
gboolean
gst_rtsp_client_get_use_client_settings (GstRTSPClient * client)
{
  gboolean res;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), FALSE);

  g_mutex_lock (&client->lock);
  res = client->use_client_settings;
  g_mutex_unlock (&client->lock);

  return res;
}

/**
 * gst_rtsp_client_set_auth:
 * @client: a #GstRTSPClient
 * @auth: a #GstRTSPAuth
 *
 * configure @auth to be used as the authentication manager of @client.
 */
void
gst_rtsp_client_set_auth (GstRTSPClient * client, GstRTSPAuth * auth)
{
  GstRTSPAuth *old;

  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  if (auth)
    g_object_ref (auth);

  g_mutex_lock (&client->lock);
  old = client->auth;
  client->auth = auth;
  g_mutex_unlock (&client->lock);

  if (old)
    g_object_unref (old);
}


/**
 * gst_rtsp_client_get_auth:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPAuth used as the authentication manager of @client.
 *
 * Returns: (transfer full): the #GstRTSPAuth of @client. g_object_unref() after
 * usage.
 */
GstRTSPAuth *
gst_rtsp_client_get_auth (GstRTSPClient * client)
{
  GstRTSPAuth *result;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), NULL);

  g_mutex_lock (&client->lock);
  if ((result = client->auth))
    g_object_ref (result);
  g_mutex_unlock (&client->lock);

  return result;
}

/**
 * gst_rtsp_client_set_send_func:
 * @client: a #GstRTSPClient
 * @func: a #GstRTSPClientSendFunc
 * @user_data: user data passed to @func
 * @notify: called when @user_data is no longer in use
 *
 * Set @func as the callback that will be called when a new message needs to be
 * sent to the client. @user_data is passed to @func and @notify is called when
 * @user_data is no longer in use.
 */
void
gst_rtsp_client_set_send_func (GstRTSPClient * client,
    GstRTSPClientSendFunc func, gpointer user_data, GDestroyNotify notify)
{
  GDestroyNotify old_func;
  gpointer old_data;

  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  g_mutex_lock (&client->lock);
  client->send_func = func;
  old_func = client->send_notify;
  old_data = client->send_data;
  client->send_notify = notify;
  client->send_data = user_data;
  g_mutex_unlock (&client->lock);

  if (old_func)
    old_func (old_data);
}

/**
 * gst_rtsp_client_handle_message:
 * @client: a #GstRTSPClient
 * @message: an #GstRTSPMessage
 *
 * Let the client handle @message.
 *
 * Returns: a #GstRTSPResult.
 */
GstRTSPResult
gst_rtsp_client_handle_message (GstRTSPClient * client,
    GstRTSPMessage * message)
{
  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), GST_RTSP_EINVAL);
  g_return_val_if_fail (message != NULL, GST_RTSP_EINVAL);

  switch (message->type) {
    case GST_RTSP_MESSAGE_REQUEST:
      handle_request (client, message);
      break;
    case GST_RTSP_MESSAGE_RESPONSE:
      break;
    case GST_RTSP_MESSAGE_DATA:
      handle_data (client, message);
      break;
    default:
      break;
  }
  return GST_RTSP_OK;
}

static GstRTSPResult
do_send_message (GstRTSPClient * client, GstRTSPMessage * message,
    gboolean close, gpointer user_data)
{
  /* send the response and store the seq number so we can wait until it's
   * written to the client to close the connection */
  return gst_rtsp_watch_send_message (client->watch, message, close ?
      &client->close_seq : NULL);
}

static GstRTSPResult
message_received (GstRTSPWatch * watch, GstRTSPMessage * message,
    gpointer user_data)
{
  return gst_rtsp_client_handle_message (GST_RTSP_CLIENT (user_data), message);
}

static GstRTSPResult
message_sent (GstRTSPWatch * watch, guint cseq, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);

  if (client->close_seq && client->close_seq == cseq) {
    client->close_seq = 0;
    close_connection (client);
  }

  return GST_RTSP_OK;
}

static GstRTSPResult
closed (GstRTSPWatch * watch, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  const gchar *tunnelid;

  GST_INFO ("client %p: connection closed", client);

  if ((tunnelid = gst_rtsp_connection_get_tunnelid (client->connection))) {
    g_mutex_lock (&tunnels_lock);
    /* remove from tunnelids */
    g_hash_table_remove (tunnels, tunnelid);
    g_mutex_unlock (&tunnels_lock);
  }

  return GST_RTSP_OK;
}

static GstRTSPResult
error (GstRTSPWatch * watch, GstRTSPResult result, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  gchar *str;

  str = gst_rtsp_strresult (result);
  GST_INFO ("client %p: received an error %s", client, str);
  g_free (str);

  return GST_RTSP_OK;
}

static GstRTSPResult
error_full (GstRTSPWatch * watch, GstRTSPResult result,
    GstRTSPMessage * message, guint id, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  gchar *str;

  str = gst_rtsp_strresult (result);
  GST_INFO
      ("client %p: received an error %s when handling message %p with id %d",
      client, str, message, id);
  g_free (str);

  return GST_RTSP_OK;
}

static gboolean
remember_tunnel (GstRTSPClient * client)
{
  const gchar *tunnelid;

  /* store client in the pending tunnels */
  tunnelid = gst_rtsp_connection_get_tunnelid (client->connection);
  if (tunnelid == NULL)
    goto no_tunnelid;

  GST_INFO ("client %p: inserting tunnel session %s", client, tunnelid);

  /* we can't have two clients connecting with the same tunnelid */
  g_mutex_lock (&tunnels_lock);
  if (g_hash_table_lookup (tunnels, tunnelid))
    goto tunnel_existed;

  g_hash_table_insert (tunnels, g_strdup (tunnelid), g_object_ref (client));
  g_mutex_unlock (&tunnels_lock);

  return TRUE;

  /* ERRORS */
no_tunnelid:
  {
    GST_ERROR ("client %p: no tunnelid provided", client);
    return FALSE;
  }
tunnel_existed:
  {
    g_mutex_unlock (&tunnels_lock);
    GST_ERROR ("client %p: tunnel session %s already existed", client,
        tunnelid);
    return FALSE;
  }
}

static GstRTSPStatusCode
tunnel_start (GstRTSPWatch * watch, gpointer user_data)
{
  GstRTSPClient *client;

  client = GST_RTSP_CLIENT (user_data);

  GST_INFO ("client %p: tunnel start (connection %p)", client,
      client->connection);

  if (!remember_tunnel (client))
    goto tunnel_error;

  return GST_RTSP_STS_OK;

  /* ERRORS */
tunnel_error:
  {
    GST_ERROR ("client %p: error starting tunnel", client);
    return GST_RTSP_STS_SERVICE_UNAVAILABLE;
  }
}

static GstRTSPResult
tunnel_lost (GstRTSPWatch * watch, gpointer user_data)
{
  GstRTSPClient *client;

  client = GST_RTSP_CLIENT (user_data);

  GST_WARNING ("client %p: tunnel lost (connection %p)", client,
      client->connection);

  /* ignore error, it'll only be a problem when the client does a POST again */
  remember_tunnel (client);

  return GST_RTSP_OK;
}

static GstRTSPResult
tunnel_complete (GstRTSPWatch * watch, gpointer user_data)
{
  const gchar *tunnelid;
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  GstRTSPClient *oclient;

  GST_INFO ("client %p: tunnel complete", client);

  /* find previous tunnel */
  tunnelid = gst_rtsp_connection_get_tunnelid (client->connection);
  if (tunnelid == NULL)
    goto no_tunnelid;

  g_mutex_lock (&tunnels_lock);
  if (!(oclient = g_hash_table_lookup (tunnels, tunnelid)))
    goto no_tunnel;

  /* remove the old client from the table. ref before because removing it will
   * remove the ref to it. */
  g_object_ref (oclient);
  g_hash_table_remove (tunnels, tunnelid);

  if (oclient->watch == NULL)
    goto tunnel_closed;
  g_mutex_unlock (&tunnels_lock);

  GST_INFO ("client %p: found tunnel %p (old %p, new %p)", client, oclient,
      oclient->connection, client->connection);

  /* merge the tunnels into the first client */
  gst_rtsp_connection_do_tunnel (oclient->connection, client->connection);
  gst_rtsp_watch_reset (oclient->watch);
  g_object_unref (oclient);

  return GST_RTSP_OK;

  /* ERRORS */
no_tunnelid:
  {
    GST_ERROR ("client %p: no tunnelid provided", client);
    return GST_RTSP_ERROR;
  }
no_tunnel:
  {
    g_mutex_unlock (&tunnels_lock);
    GST_ERROR ("client %p: tunnel session %s not found", client, tunnelid);
    return GST_RTSP_ERROR;
  }
tunnel_closed:
  {
    g_mutex_unlock (&tunnels_lock);
    GST_ERROR ("client %p: tunnel session %s was closed", client, tunnelid);
    g_object_unref (oclient);
    return GST_RTSP_ERROR;
  }
}

static GstRTSPWatchFuncs watch_funcs = {
  message_received,
  message_sent,
  closed,
  error,
  tunnel_start,
  tunnel_complete,
  error_full,
  tunnel_lost
};

static void
client_watch_notify (GstRTSPClient * client)
{
  GST_INFO ("client %p: watch destroyed", client);
  client->watch = NULL;
  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_CLOSED], 0, NULL);
  g_object_unref (client);
}

static gboolean
setup_client (GstRTSPClient * client, GSocket * socket,
    GstRTSPConnection * conn, GError ** error)
{
  GSocket *read_socket;
  GSocketAddress *address;
  GstRTSPUrl *url;

  read_socket = gst_rtsp_connection_get_read_socket (conn);
  client->is_ipv6 = g_socket_get_family (socket) == G_SOCKET_FAMILY_IPV6;

  if (!(address = g_socket_get_remote_address (read_socket, error)))
    goto no_address;

  g_free (client->server_ip);
  /* keep the original ip that the client connected to */
  if (G_IS_INET_SOCKET_ADDRESS (address)) {
    GInetAddress *iaddr;

    iaddr = g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (address));

    client->server_ip = g_inet_address_to_string (iaddr);
    g_object_unref (address);
  } else {
    client->server_ip = g_strdup ("unknown");
  }

  GST_INFO ("client %p connected to server ip %s, ipv6 = %d", client,
      client->server_ip, client->is_ipv6);

  url = gst_rtsp_connection_get_url (conn);
  GST_INFO ("added new client %p ip %s:%d", client, url->host, url->port);

  client->connection = conn;

  return TRUE;

  /* ERRORS */
no_address:
  {
    GST_ERROR ("could not get remote address %s", (*error)->message);
    return FALSE;
  }
}

/**
 * gst_rtsp_client_use_socket:
 * @client: a #GstRTSPClient
 * @socket: a #GSocket
 * @ip: the IP address of the remote client
 * @port: the port used by the other end
 * @initial_buffer: any zero terminated initial data that was already read from
 *     the socket
 * @error: a #GError
 *
 * Take an existing network socket and use it for an RTSP connection.
 *
 * Returns: %TRUE on success.
 */
gboolean
gst_rtsp_client_use_socket (GstRTSPClient * client, GSocket * socket,
    const gchar * ip, gint port, const gchar * initial_buffer, GError ** error)
{
  GstRTSPConnection *conn;
  GstRTSPResult res;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), FALSE);
  g_return_val_if_fail (G_IS_SOCKET (socket), FALSE);

  GST_RTSP_CHECK (gst_rtsp_connection_create_from_socket (socket, ip, port,
          initial_buffer, &conn), no_connection);

  return setup_client (client, socket, conn, error);

  /* ERRORS */
no_connection:
  {
    gchar *str = gst_rtsp_strresult (res);

    GST_ERROR ("could not create connection from socket %p: %s", socket, str);
    g_free (str);
    return FALSE;
  }
}

/**
 * gst_rtsp_client_accept:
 * @client: a #GstRTSPClient
 * @socket: a #GSocket
 * @context: the context to run in
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Accept a new connection for @client on @socket.
 *
 * Returns: %TRUE if the client could be accepted.
 */
gboolean
gst_rtsp_client_accept (GstRTSPClient * client, GSocket * socket,
    GCancellable * cancellable, GError ** error)
{
  GstRTSPConnection *conn;
  GstRTSPResult res;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), FALSE);
  g_return_val_if_fail (G_IS_SOCKET (socket), FALSE);

  /* a new client connected. */
  GST_RTSP_CHECK (gst_rtsp_connection_accept (socket, &conn, cancellable),
      accept_failed);

  return setup_client (client, socket, conn, error);

  /* ERRORS */
accept_failed:
  {
    gchar *str = gst_rtsp_strresult (res);

    GST_ERROR ("Could not accept client on server socket %p: %s", socket, str);
    g_free (str);
    return FALSE;
  }
}

/**
 * gst_rtsp_client_attach:
 * @client: a #GstRTSPClient
 * @context: (allow-none): a #GMainContext
 *
 * Attaches @client to @context. When the mainloop for @context is run, the
 * client will be dispatched. When @context is NULL, the default context will be
 * used).
 *
 * This function should be called when the client properties and urls are fully
 * configured and the client is ready to start.
 *
 * Returns: the ID (greater than 0) for the source within the GMainContext.
 */
guint
gst_rtsp_client_attach (GstRTSPClient * client, GMainContext * context)
{
  guint res;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), 0);
  g_return_val_if_fail (client->watch == NULL, 0);

  /* create watch for the connection and attach */
  client->watch = gst_rtsp_watch_new (client->connection, &watch_funcs,
      g_object_ref (client), (GDestroyNotify) client_watch_notify);
  gst_rtsp_client_set_send_func (client, do_send_message, NULL, NULL);

  GST_INFO ("attaching to context %p", context);
  res = gst_rtsp_watch_attach (client->watch, context);
  gst_rtsp_watch_unref (client->watch);

  return res;
}
