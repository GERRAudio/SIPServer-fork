/* aes67_api.c — AES67 GStreamer Core
 * 2025 GERR Audio
 * SPDX-License-Identifier: MPL-1.1
 */

#include "aes67_api.h"
#include <gst/app/gstappsink.h>
#include <gst/audio/audio-channels.h>
#include <gst/net/net.h>
#include <switch.h>

#include "aes67_alloc.h"

G_alloc_counts g_alloc_counts;

#define ELEMENT_NAME_SIZE (30 + SESSION_ID_LEN)
#define NAME_ELEMENT(name, elem, ch) g_snprintf(name, sizeof(name), "%s-ch%u", elem, ch)
#define NAME_SESSION_ELEMENT(name, elem, ch, sess)                                                                     \
	g_snprintf(name, sizeof(name), "%s-ch%u-sess%s", elem, ch, (sess) ? (sess) : "null")

#define RTP_DEPAY "rx-depay"

#ifdef _WIN32
#define SYNTHETIC_CLOCK_INTERVAL_MS 1000
#else
#define SYNTHETIC_CLOCK_INTERVAL_MS 100
#endif

#define MAKE_TS_ELEMENT(var, factory, name, ctx)                                                                       \
	do {                                                                                                               \
		var = gst_element_factory_make(factory, name);                                                                 \
		if (var) g_object_set(var, "context", ctx, "context-wait", 10, NULL);                                          \
	} while (0)

/* ========================================================================= */
/* Helper Functions                                                          */
/* ========================================================================= */

static inline GstElement *find_element(g_stream_t *stream, const char *prefix, guint ch, const char *session)
{
	gchar name[ELEMENT_NAME_SIZE];
	NAME_SESSION_ELEMENT(name, prefix, ch, session);
	return AL_gst_bin_get_by_name(GST_BIN(stream->pipeline), name);
}

static inline GstElement *find_tee(g_stream_t *stream, guint ch)
{
	gchar name[ELEMENT_NAME_SIZE];
	NAME_ELEMENT(name, "tee", ch);
	return AL_gst_bin_get_by_name(GST_BIN(stream->pipeline), name);
}

static inline void safe_remove_element(GstElement *pipeline, GstElement *elem)
{
	if (elem && GST_IS_BIN(pipeline)) MU_gst_bin_remove(GST_BIN(pipeline), elem);
}

static void deinterleave_pad_added(GstElement *deinterleave, GstPad *new_pad, gpointer userdata)
{
	GstCaps *caps = gst_pad_get_current_caps(new_pad);
	if (!caps) caps = gst_pad_query_caps(new_pad, NULL);
	if (!caps) return;

	const GstStructure *s = gst_caps_get_structure(caps, 0);
	gint channels = 0, channel = 0;
	if (gst_structure_get_int(s, "channels", &channels) && gst_structure_get_int(s, "channel-position", &channel)) {
		GstElement *pipeline = GST_ELEMENT(AL_gst_element_get_parent(deinterleave));
		GstElement *tee = find_tee((g_stream_t *)userdata, channel);
		if (tee) {
			GstPad *sink = gst_element_get_request_pad(tee, "sink");
			if (sink) {
				gst_pad_link(new_pad, sink);
				gst_object_unref(sink);
			}
		}
		DA_gst_object_unref(tee);
		DA_gst_object_unref(pipeline);
	}
	DA_gst_caps_unref(caps);
}

/* ========================================================================= */
/* Bus & Error Handling                                                      */
/* ========================================================================= */

static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data)
{
	g_stream_t *stream = (g_stream_t *)data;

	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_ERROR: {
		GError *err = NULL;
		gchar *debug = NULL;
		gst_message_parse_error(msg, &err, &debug);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GStreamer error: %s\n", err->message);
		if (stream->error_cb) stream->error_cb(err->message, stream);
		g_error_free(err);
		g_free(debug);
		break;
	}
	case GST_MESSAGE_STATE_CHANGED:
		if (GST_MESSAGE_SRC(msg) == GST_OBJECT(stream->pipeline)) {
			GstState old, new;
			gst_message_parse_state_changed(msg, &old, &new, NULL);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Pipeline %s → %s\n",
							  gst_element_state_get_name(old), gst_element_state_get_name(new));
		}
		break;
	default:
		break;
	}
	return TRUE;
}

/* ========================================================================= */
/* Core API                                                                  */
/* ========================================================================= */

g_stream_t *create_pipeline(pipeline_data_t *data, event_callback_t *error_cb)
{
	g_stream_t *stream = NULL;
	GstElement *pipeline = NULL;
	GstBus *bus = NULL;
	gchar *pipeline_name = NULL;

	if (!data) goto error;

	pipeline_name = data->name && strlen(data->name) ? g_strdup(data->name) : g_strdup("aes67-stream");
	if (!pipeline_name) goto error;

	stream = g_malloc0(sizeof(g_stream_t));
	if (!stream) goto error;

	stream->ts_ctx = strlen(data->ts_context_name) ? data->ts_context_name : "ts";
	stream->error_cb = error_cb;
	stream->sample_rate = data->sample_rate;
	stream->txdrop = data->txdrop;

	pipeline = AL_gst_pipeline_new(pipeline_name);
	if (!pipeline) goto error;

	stream->pipeline = GST_PIPELINE(pipeline);

	/* ==================== RX PATH ==================== */
	if (data->direction & DIRECTION_RX) {
		GstElement *src = NULL, *jitterbuf = NULL, *depay = NULL;
		GstElement *split = NULL, *conv = NULL, *capsf = NULL, *deinterleave = NULL;
		GstCaps *caps = NULL;

#ifndef ENABLE_THREADSHARE
		src = gst_element_factory_make("udpsrc", "rx-src");
#else
		MAKE_TS_ELEMENT(src, "ts-udpsrc", "rx-src", stream->ts_ctx);
#endif
		g_object_set(src, "address", data->rx_ip_addr, "port", data->rx_port, "multicast-iface", data->rtp_iface,
					 "retrieve-sender-address", FALSE, "buffer-size", 1048576, NULL);

		jitterbuf = gst_element_factory_make("rtpjitterbuffer", "rx-jitbuf");
		g_object_set(jitterbuf, "latency", data->rtp_jitbuf_latency, "mode", 0, NULL);

		depay = data->rx_codec == L16 ? gst_element_factory_make("rtpL16depay", RTP_DEPAY)
									  : gst_element_factory_make("rtpL24depay", RTP_DEPAY);

		caps = gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "audio", "clock-rate", G_TYPE_INT,
								   data->sample_rate, "encoding-name", G_TYPE_STRING,
								   data->rx_codec == L16 ? "L16" : "L24", "channels", G_TYPE_INT, data->channels,
								   "channel-order", G_TYPE_STRING, "unpositioned", NULL);
		g_object_set(src, "caps", caps, NULL);
		DA_gst_caps_unref(caps);

		split = gst_element_factory_make("audiobuffersplit", "rx-split");
		g_object_set(split, "output-buffer-duration", data->codec_ms, 1000, NULL);

		conv = gst_element_factory_make("audioconvert", "rx-aconv");
		g_object_set(conv, "dithering", 0, NULL);

		capsf = gst_element_factory_make("capsfilter", "rx-capsf");
		caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "layout", G_TYPE_STRING,
								   "interleaved", "channels", G_TYPE_INT, data->channels, NULL);
		g_object_set(capsf, "caps", caps, NULL);
		DA_gst_caps_unref(caps);

		deinterleave = gst_element_factory_make("deinterleave", "rx-deinterleave");
		g_signal_connect(deinterleave, "pad-added", G_CALLBACK(deinterleave_pad_added), stream);

		/* Create per-channel tees */
		for (guint i = 0; i < data->channels; i++) {
			gchar name[ELEMENT_NAME_SIZE];
			NAME_ELEMENT(name, "tee", i);
			GstElement *tee = gst_element_factory_make("tee", name);
			if (tee) {
				g_object_set(tee, "allow-not-linked", TRUE, NULL);
				MU_gst_bin_add(GST_BIN(pipeline), tee);
			}
		}

		MU_gst_bin_add_many(GST_BIN(pipeline), src, jitterbuf, depay, split, conv, capsf, deinterleave, NULL);
		gst_element_link_many(src, jitterbuf, depay, split, conv, capsf, deinterleave, NULL);
	}

	/* ==================== TX PATH ==================== */
	if (data->direction & DIRECTION_TX) {
		GstElement *interleave = NULL, *valve = NULL, *conv = NULL, *capsf = NULL, *pay = NULL, *sink = NULL;
		GstCaps *caps = NULL;

		interleave = gst_element_factory_make("audiointerleave", "tx-interleave");
		g_object_set(interleave, "start-time-selection", 1, NULL);

		valve = gst_element_factory_make("valve", "tx-valve");
		g_object_set(valve, "drop", data->txdrop, NULL);

		conv = gst_element_factory_make("audioconvert", "tx-aconv");
		g_object_set(conv, "dithering", 0, NULL);

		capsf = gst_element_factory_make("capsfilter", "tx-capsf");
		caps =
			gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate", G_TYPE_INT, data->sample_rate,
								"channels", G_TYPE_INT, data->channels, "layout", G_TYPE_STRING, "interleaved", NULL);
		g_object_set(capsf, "caps", caps, NULL);
		DA_gst_caps_unref(caps);

		pay = data->tx_codec == L16 ? gst_element_factory_make("rtpL16pay", "tx-pay")
									: gst_element_factory_make("rtpL24pay", "tx-pay");
		g_object_set(pay, "pt", data->rtp_payload_type, NULL);

		sink = gst_element_factory_make("udpsink", "tx-sink");
		g_object_set(sink, "host", data->tx_ip_addr, "port", data->tx_port, "multicast-iface", data->rtp_iface, "sync",
					 TRUE, "async", FALSE, "qos", TRUE, "qos-dscp", 34, NULL);

		/* appsrc per channel */
		for (guint i = 0; i < data->channels; i++) {
			gchar name[ELEMENT_NAME_SIZE], pad[32];
			g_snprintf(pad, sizeof(pad), "sink_%u", i);
			NAME_ELEMENT(name, "appsrc", i);
			GstElement *src = gst_element_factory_make("appsrc", name);
			if (src) {
				caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate", G_TYPE_INT,
										   data->sample_rate, "channels", G_TYPE_INT, 1, "layout", G_TYPE_STRING,
										   "interleaved", NULL);
				g_object_set(src, "is-live", TRUE, "do-timestamp", TRUE, "format", GST_FORMAT_TIME, "max-bytes",
							 (data->codec_ms * data->sample_rate * 2 * 3) / 1000, "caps", caps, NULL);
				DA_gst_caps_unref(caps);
				MU_gst_bin_add(GST_BIN(pipeline), src);
				gst_element_link_pads(src, "src", interleave, pad);
			}
		}

		MU_gst_bin_add_many(GST_BIN(pipeline), interleave, valve, capsf, conv, pay, sink, NULL);
		gst_element_link_many(interleave, valve, capsf, conv, pay, sink, NULL);
	}

	/* Final setup */
	bus = AL_gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	gst_bus_add_watch(bus, bus_callback, stream);
	DA_gst_object_unref(bus);

	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	stream->mainloop = g_main_loop_new(NULL, FALSE);
	stream->thread = g_thread_new(pipeline_name, start_pipeline, stream);

	g_free(pipeline_name);
	return stream;

error:
	if (pipeline) {
		gst_element_set_state(pipeline, GST_STATE_NULL);
		DA_gst_object_unref(pipeline);
	}
	if (stream) {
		if (stream->mainloop) g_main_loop_unref(stream->mainloop);
		if (stream->thread) g_thread_join(stream->thread);
		DA_g_free(stream);
	}
	g_free(pipeline_name);
	return NULL;
}

void *start_pipeline(void *data)
{
	g_main_loop_run(((g_stream_t *)data)->mainloop);
	return NULL;
}

void stop_pipeline(g_stream_t *stream)
{
	if (!stream) return;
	if (stream->pipeline) {
		gst_element_set_state(GST_ELEMENT(stream->pipeline), GST_STATE_NULL);
		DA_gst_object_unref(stream->pipeline);
	}
	if (stream->mainloop) {
		g_main_loop_quit(stream->mainloop);
		g_main_loop_unref(stream->mainloop);
	}
	if (stream->thread) g_thread_join(stream->thread);
	DA_g_free(stream);
}

void teardown_mainloop(GMainLoop *loop)
{
	if (loop) g_main_loop_quit(loop);
}
void start_mainloop(GMainLoop *loop)
{
	if (loop) g_main_loop_run(loop);
}

gboolean push_buffer(g_stream_t *stream, unsigned char *payload, guint len, guint ch_idx, switch_timer_t *timer)
{
	gchar name[ELEMENT_NAME_SIZE];
	NAME_ELEMENT(name, "appsrc", ch_idx);
	GstElement *src = AL_gst_bin_get_by_name(GST_BIN(stream->pipeline), name);
	if (!src) return FALSE;

	GstBuffer *buf = gst_buffer_new_allocate(NULL, len, NULL);
	gst_buffer_fill(buf, 0, payload, len);
	gst_app_src_push_buffer(src,buf);
	DA_gst_object_unref(src);
	return TRUE;
}

gboolean add_appsink(g_stream_t *stream, guint ch_idx, const char *session)
{
	gboolean success = FALSE;
	GstElement *tee = NULL, *queue = NULL, *appsink = NULL;
	GstPad *tee_src = NULL, *queue_sink = NULL;

	if (!stream || !stream->pipeline) goto cleanup;

	tee = find_tee(stream, ch_idx);
	if (!tee) goto cleanup;

		/* queue */
#ifndef ENABLE_THREADSHARE
	queue = gst_element_factory_make("queue", NULL);
#else
	{
		gchar name[ELEMENT_NAME_SIZE];
		NAME_SESSION_ELEMENT(name, "queue", ch_idx, session);
		MAKE_TS_ELEMENT(queue, "ts-queue", name, stream->ts_ctx);
	}
#endif
	if (!queue) goto cleanup;
	g_object_set(queue, "leaky", 2, "max-size-buffers", 5, NULL);

	/* appsink */
	{
		gchar name[ELEMENT_NAME_SIZE];
		NAME_SESSION_ELEMENT(name, "appsink", ch_idx, session);
		appsink = gst_element_factory_make("appsink", name);
	}
	if (!appsink) goto cleanup;
	g_object_set(appsink, "emit-signals", FALSE, "sync", FALSE, "enable-last-sample", FALSE, "max-buffers", 5, NULL);

	tee_src = gst_element_get_request_pad(tee, "src_%u");
	queue_sink = gst_element_get_static_pad(queue, "sink");
	if (!tee_src || !queue_sink) goto cleanup;

	if (!MU_gst_bin_add(GST_BIN(stream->pipeline), queue) || !MU_gst_bin_add(GST_BIN(stream->pipeline), appsink))
		goto cleanup;

	if (gst_pad_link(tee_src, queue_sink) != GST_PAD_LINK_OK || !gst_element_link(queue, appsink)) goto cleanup;

	gst_element_sync_state_with_parent(queue);
	gst_element_sync_state_with_parent(appsink);

	dump_pipeline(stream->pipeline, "appsink-added");
	success = TRUE;

cleanup:
	if (!success) {
		safe_remove_element(stream->pipeline, queue);
		safe_remove_element(stream->pipeline, appsink);
	}
	DA_gst_object_unref(tee_src);
	DA_gst_object_unref(queue_sink);
	DA_gst_object_unref(tee);
	return success;
}

gboolean remove_appsink(g_stream_t *stream, guint ch_idx, const char *session)
{
	if (!stream || !stream->pipeline) return FALSE;

	GstElement *queue = find_element(stream, "queue", ch_idx, session);
	GstElement *appsink = find_element(stream, "appsink", ch_idx, session);
	GstElement *tee = find_tee(stream, ch_idx);

	if (!queue || !appsink || !tee) goto cleanup;

	GstPad *sink_pad = gst_element_get_static_pad(queue, "sink");
	if (sink_pad) {
		GstPad *src_pad = gst_pad_get_peer(sink_pad);
		if (src_pad) {
			gst_pad_unlink(src_pad, sink_pad);
			gst_element_release_request_pad(tee, src_pad);
			DA_gst_object_unref(src_pad);
		}
		DA_gst_object_unref(sink_pad);
	}

	gst_element_unlink(queue, appsink);
	gst_element_set_state(queue, GST_STATE_NULL);
	gst_element_set_state(appsink, GST_STATE_NULL);
	MU_gst_bin_remove(GST_BIN(stream->pipeline), queue);
	MU_gst_bin_remove(GST_BIN(stream->pipeline), appsink);

	dump_pipeline(stream->pipeline, "appsink-removed");

cleanup:
	DA_gst_object_unref(queue);
	DA_gst_object_unref(appsink);
	DA_gst_object_unref(tee);
	return TRUE;
}

int pull_buffers(g_stream_t *stream, unsigned char *payload, guint needed, guint ch_idx, switch_timer_t *timer,
				 const char *session)
{
	GstElement *appsink = find_element(stream, "appsink", ch_idx, session);
	if (!appsink) return 0;

	int total = 0;
	if (stream->leftover_bytes[ch_idx]) {
		size_t copy = MIN(stream->leftover_bytes[ch_idx], needed);
		memcpy(payload, stream->leftover[ch_idx], copy);
		total += copy;
		stream->leftover_bytes[ch_idx] -= copy;
		if (stream->leftover_bytes[ch_idx])
			memmove(stream->leftover[ch_idx], stream->leftover[ch_idx] + copy, stream->leftover_bytes[ch_idx]);
	}

	while (total < needed) {
		GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 10 * GST_MSECOND);
		if (!sample) break;

		GstBuffer *buf = gst_sample_get_buffer(sample);
		if (buf) {
			GstMapInfo map;
			if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
				size_t want = needed - total;
				if (map.size > want) {
					stream->leftover_bytes[ch_idx] = map.size - want;
					memcpy(stream->leftover[ch_idx], map.data + want, stream->leftover_bytes[ch_idx]);
					map.size = want;
				}
				memcpy(payload + total, map.data, map.size);
				total += map.size;
				gst_buffer_unmap(buf, &map);
			}
		}
		gst_sample_unref(sample);
	}

	DA_gst_object_unref(appsink);
	return total;
}

gchar *get_rtp_stats(g_stream_t *stream)
{
	GstElement *jb = AL_gst_bin_get_by_name(GST_BIN(stream->pipeline), "rx-jitbuf");
	gchar *stats = g_strdup("");
	if (jb) {
		GstStructure *s = NULL;
		g_object_get(jb, "stats", &s, NULL);
		if (s) {
			gchar *tmp = gst_structure_to_string(s);
			g_free(stats);
			stats = tmp;
			gst_structure_free(s);
		}
		DA_gst_object_unref(jb);
	}
	return stats;
}

void drop_output_buffers(gboolean drop, g_stream_t *stream)
{
	GstElement *valve = AL_gst_bin_get_by_name(GST_BIN(stream->pipeline), "tx-valve");
	if (valve) {
		g_object_set(valve, "drop", drop, NULL);
		DA_gst_object_unref(valve);
	}
}

void drop_input_buffers(gboolean drop, g_stream_t *stream, guint32 ch_idx)
{
	gchar name[ELEMENT_NAME_SIZE];
	NAME_ELEMENT(name, "valve", ch_idx);
	GstElement *valve = AL_gst_bin_get_by_name(GST_BIN(stream->pipeline), name);
	if (valve) {
		g_object_set(valve, "drop", drop, NULL);
		DA_gst_object_unref(valve);
	}
}

void dump_pipeline(GstPipeline *pipe, const char *name)
{
	gchar *filename = g_strdup_printf("%s-%s", GST_OBJECT_NAME(pipe), name);
	GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipe), GST_DEBUG_GRAPH_SHOW_ALL, filename);
	g_free(filename);
}
/* ========================================================================= */
/* PTP Clock Integration                                                     */
/* ========================================================================= */

void use_ptp_clock(g_stream_t *stream, GstClock *ptp_clock)
{
	if (!stream || !stream->pipeline || !ptp_clock) return;

	/* Use the PTP clock for both rendering and as pipeline clock */
	MU_gst_pipeline_use_clock(GST_PIPELINE(stream->pipeline), ptp_clock);

	if (!gst_pipeline_set_clock(GST_PIPELINE(stream->pipeline), ptp_clock)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Failed to set PTP clock on pipeline '%s'\n",
						  GST_OBJECT_NAME(stream->pipeline));
		return;
	}

	gst_element_set_base_time(GST_ELEMENT(stream->pipeline), 0);
	gst_element_set_start_time(GST_ELEMENT(stream->pipeline), GST_CLOCK_TIME_NONE);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Pipeline '%s' successfully using external PTP clock\n",
					  GST_OBJECT_NAME(stream->pipeline));
}

/* ========================================================================= */
/* Backup sender idle timer (optional — only if you use backup sender)      */
/* ========================================================================= */

static gboolean backup_sender_idle_timer_cb(gpointer user_data)
{
	g_stream_t *stream = (g_stream_t *)user_data;
	if (!stream) return FALSE;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Backup sender idle timeout — pausing transmission\n");

	drop_output_buffers(TRUE, stream); /* Drop TX packets */
	stream->pause_backup_sender = TRUE;

	/* Remove the timer source */
	stream->backup_sender_idle_timer = 0;
	return FALSE; /* don't repeat */
}

void reset_backup_sender_timer(g_stream_t *stream)
{
	if (!stream) return;

	/* Remove existing timer if any */
	if (stream->backup_sender_idle_timer) {
		g_source_remove(stream->backup_sender_idle_timer);
		stream->backup_sender_idle_timer = 0;
	}

	if (stream->backup_sender_idle_wait_ms <= 0) return;

	stream->pause_backup_sender = FALSE;
	drop_output_buffers(FALSE, stream); /* Resume TX */

	stream->backup_sender_idle_timer =
		g_timeout_add(stream->backup_sender_idle_wait_ms, backup_sender_idle_timer_cb, stream);
}


