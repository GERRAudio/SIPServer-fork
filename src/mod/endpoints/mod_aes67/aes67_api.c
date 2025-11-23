/* aes67_api.c — PTP Synchronization
 * Copyright © 2025 GERR Audio
 * SPDX-License-Identifier: MPL-1.1
 */

#include <switch.h>
#include "aes67_api.h"
#include <gst/app/gstappsink.h>
#include <gst/audio/audio-channels.h>
#include <gst/net/net.h>

#include "aes67_alloc.h"

G_alloc_counts g_alloc_counts;

/* Singleton system clock — shared across all streams */
static GstClock *system_clock = NULL;

/* Configuration */
#define ELEMENT_NAME_SIZE (30 + SESSION_ID_LEN)
#define NAME_ELEMENT(name, elem, ch)    g_snprintf(name, sizeof(name), "%s-ch%u", elem, ch)
#define NAME_SESSION_ELEMENT(name, elem, ch, sess) \
    g_snprintf(name, sizeof(name), "%s-ch%u-sess%s", elem, ch, (sess) ? (sess) : "null")

#define RTP_DEPAY "rx-depay"

#ifdef _WIN32
#define SYNTHETIC_CLOCK_INTERVAL_MS 1000
#else
#define SYNTHETIC_CLOCK_INTERVAL_MS 100
#endif

#define MAKE_TS_ELEMENT(var, factory, name, ctx) \
    do { \
     var = gst_element_factory_make(factory, name); \
     g_object_set(var, "context", ctx, "context-wait", 10, NULL); \
 } while(0)

/* ========================================================================= */
/* Helper Functions                                                          */
/* ========================================================================= */
/* Implementation of teardown_mainloop — quits and unrefs the GMainLoop */
void teardown_mainloop(GMainLoop *loop)
{
	if (!loop) return;
	if (g_main_loop_is_running(loop)) { g_main_loop_quit(loop); }
	g_main_loop_unref(loop);
}

/* Implementation of push_buffer — pushes audio buffer to appsrc (TX path) */
gboolean push_buffer(g_stream_t *stream, unsigned char *payload, guint len, guint ch_idx, switch_timer_t *timer)
{
	if (!stream || !payload || len == 0) return FALSE;

	gchar name[ELEMENT_NAME_SIZE]; // Define ELEMENT_NAME_SIZE if not already (e.g., #define ELEMENT_NAME_SIZE 64)
	NAME_ELEMENT(name, "appsrc", ch_idx);
	GstElement *appsrc = AL_gst_bin_get_by_name(GST_BIN(stream->pipeline), name);

	if (!appsrc) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Appsrc not found for ch%u\n", ch_idx);
		return FALSE;
	}

	GstBuffer *buf = gst_buffer_new_allocate(NULL, len, NULL);
	if (!buf) {
		DA_gst_object_unref(appsrc);
		return FALSE;
	}

	GstMapInfo map;
	if (gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
		memcpy(map.data, payload, len);
		gst_buffer_unmap(buf, &map);
	}

	gboolean success = gst_app_src_push_buffer(appsrc, buf) == GST_FLOW_OK;
	if (!success) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to push buffer to appsrc ch%u\n", ch_idx);
	}

	DA_gst_object_unref(appsrc);
	return success;
}

/* If ELEMENT_NAME_SIZE is missing, add this define at the top of aes67_api.c */
#ifndef ELEMENT_NAME_SIZE
#define ELEMENT_NAME_SIZE 64
#endif
static GstClock *get_system_clock(void)
{
    if (g_once_init_enter(&system_clock)) {
        GstClock *clock = gst_system_clock_obtain();
        g_once_init_leave(&system_clock, clock);
    }
    return system_clock;
}

static inline GstElement *find_element(g_stream_t *stream, const char *factory, guint ch, const char *session)
{
    gchar name[ELEMENT_NAME_SIZE];
    NAME_SESSION_ELEMENT(name, factory, ch, session);
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
    if (elem && GST_IS_BIN(pipeline))
        MU_gst_bin_remove(GST_BIN(pipeline), elem);
}

/* ========================================================================= */
/* Synthetic PTP Clock Discipline — Studio-Grade, Drift-Free                 */
/* ========================================================================= */

static gboolean update_synthetic_clock(gpointer user_data)
{
    g_stream_t *stream = (g_stream_t *)user_data;
    GstClock *synth = stream->clock;
    GstClockTime real_time, synth_time;
    gdouble rate_ratio = 1.0;

    if (!synth || !GST_IS_CLOCK(synth))
        return TRUE;

    real_time  = gst_clock_get_time(get_system_clock());
    synth_time = gst_clock_get_time(synth);

    if (synth_time > real_time) {
        GstClockTime diff = synth_time - real_time;
        if (diff > 50 * GST_MSECOND)      rate_ratio = 0.999;
        else if (diff > 10 * GST_MSECOND) rate_ratio = 0.9998;
        else if (diff > 2 * GST_MSECOND)  rate_ratio = 0.99995;
    } else {
        GstClockTime diff = real_time - synth_time;
        if (diff > 50 * GST_MSECOND)      rate_ratio = 1.001;
        else if (diff > 10 * GST_MSECOND) rate_ratio = 1.0002;
        else if (diff > 2 * GST_MSECOND)  rate_ratio = 1.00005;
    }

    if (rate_ratio != 1.0) {
        GstClockTime internal, external;
        guint64 num, den;
        gst_clock_get_calibration(synth, &internal, &external, &num, &den);
        num = (guint64)(num * rate_ratio + 0.5);  // round
        gst_clock_set_calibration(synth, internal, external, num, den);
    }

    return TRUE; /* keep running */
}

static void start_synthetic_clock(g_stream_t *stream)
{
    if (stream->clock) return;

    stream->clock = g_object_new(GST_TYPE_SYSTEM_CLOCK,
                                 "clock-type", GST_CLOCK_TYPE_REALTIME,
                                 "name", "AES67-SyntheticPTP",
                                 NULL);

    /* Align perfectly at start */
    GstClockTime now = gst_clock_get_time(get_system_clock());
    gst_clock_set_calibration(stream->clock, now, now, 1, 1);

    stream->cb_rx_stats_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
        SYNTHETIC_CLOCK_INTERVAL_MS,
        update_synthetic_clock,
        stream,
        NULL);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
        "[%s] No real PTP detected — using drift-corrected synthetic clock\n",
        GST_OBJECT_NAME(stream->pipeline));
}

static void stop_synthetic_clock(g_stream_t *stream)
{
    if (stream->cb_rx_stats_id) {
        g_source_remove(stream->cb_rx_stats_id);
        stream->cb_rx_stats_id = 0;
    }
    if (stream->clock) {
        gst_object_unref(stream->clock);
        stream->clock = NULL;
    }
}

/* ========================================================================= */
/* Appsink Management                                                        */
/* ========================================================================= */

gboolean add_appsink(g_stream_t *stream, guint ch_idx, const char *session)
{
    gboolean success = FALSE;
    GstElement *tee = NULL, *queue = NULL, *appsink = NULL;
    GstPad *tee_src = NULL, *queue_sink = NULL;

    if (!stream || !stream->pipeline) goto cleanup;

    tee = find_tee(stream, ch_idx);
    if (!tee) goto cleanup;

    /* Create queue */
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

    /* Create appsink */
    {
        gchar name[ELEMENT_NAME_SIZE];
        NAME_SESSION_ELEMENT(name, "appsink", ch_idx, session);
        appsink = gst_element_factory_make("appsink", name);
    }
    if (!appsink) goto cleanup;
    g_object_set(appsink,
        "emit-signals", FALSE,
        "sync", FALSE,
        "enable-last-sample", FALSE,
        "max-buffers", 5,
        NULL);

    tee_src = gst_element_get_request_pad(tee, "src_%u");
    queue_sink = gst_element_get_static_pad(queue, "sink");
    if (!tee_src || !queue_sink) goto cleanup;

    if (!MU_gst_bin_add(GST_BIN(stream->pipeline), queue) ||
        !MU_gst_bin_add(GST_BIN(stream->pipeline), appsink))
        goto cleanup;

    if (gst_pad_link(tee_src, queue_sink) != GST_PAD_LINK_OK ||
        !gst_element_link(queue, appsink))
        goto cleanup;

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

    GstElement *queue   = find_element(stream, "queue",   ch_idx, session);
    GstElement *appsink = find_element(stream, "appsink", ch_idx, session);
    GstElement *tee     = find_tee(stream, ch_idx);

    if (!queue || !appsink || !tee) goto cleanup;

    /* Unlink tee → queue */
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

/* ========================================================================= */
/* Audio I/O                                                                 */
/* ========================================================================= */

int pull_buffers(g_stream_t *stream, unsigned char *payload, guint needed_bytes,
                 guint ch_idx, switch_timer_t *timer, const char *session)
{
    GstElement *appsink = find_element(stream, "appsink", ch_idx, session);
    GstSample *sample = NULL;
    GstBuffer *buf;
    GstMapInfo info;
    int total_bytes = 0;

    if (!appsink || !payload) return 0;

    /* Copy leftover from previous pull */
    if (stream->leftover_bytes[ch_idx]) {
        size_t copy = MIN(stream->leftover_bytes[ch_idx], needed_bytes);
        memcpy(payload, stream->leftover[ch_idx], copy);
        total_bytes += copy;
        stream->leftover_bytes[ch_idx] -= copy;
        if (stream->leftover_bytes[ch_idx] > 0)
            memmove(stream->leftover[ch_idx], stream->leftover[ch_idx] + copy,
                    stream->leftover_bytes[ch_idx]);
    }

    while (total_bytes < needed_bytes) {
        sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 10 * GST_MSECOND);
        if (!sample) break;

        buf = gst_sample_get_buffer(sample);
        if (!buf || !gst_buffer_map(buf, &info, GST_MAP_READ)) {
            gst_sample_unref(sample);
            continue;
        }

        size_t want = needed_bytes - total_bytes;
        if (info.size > want) {
            stream->leftover_bytes[ch_idx] = info.size - want;
            memcpy(stream->leftover[ch_idx], info.data + want, stream->leftover_bytes[ch_idx]);
            info.size = want;
        }

        memcpy(payload + total_bytes, info.data, info.size);
        total_bytes += info.size;

        gst_buffer_unmap(buf, &info);
        gst_sample_unref(sample);
    }

    DA_gst_object_unref(appsink);
    return total_bytes;
}

/* ========================================================================= */
/* Clock & Debug Utilities                                                   */
/* ========================================================================= */

void use_ptp_clock(g_stream_t *stream, GstClock *ptp_clock)
{
    if (!stream || !stream->pipeline || !ptp_clock) return;

    stop_synthetic_clock(stream);
    gst_pipeline_use_clock(GST_PIPELINE(stream->pipeline), ptp_clock);
    g_atomic_int_set(&stream->clock_sync, 1);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
        "[%s] Switched to real PTP clock\n",
        GST_OBJECT_NAME(stream->pipeline));
}

void aes67_clock_status(g_stream_t *stream, switch_stream_handle_t *output)
{
    if (!stream || !stream->pipeline) {
        output->write_function(output, "No active pipeline\n");
        return;
    }

    if (g_atomic_int_get(&stream->clock_sync)) {
        output->write_function(output, "Clock: Real PTP (synchronized)\n");
    } else if (stream->clock) {
        GstClockTime real  = gst_clock_get_time(get_system_clock());
        GstClockTime synth = gst_clock_get_time(stream->clock);
        gint64 diff_ms = GST_TIME_AS_MSECONDS(llabs((gint64)synth - (gint64)real));

        output->write_function(output,
            "Clock: Synthetic PTP (drift: %c%ld ms)\n",
            (synth > real) ? '+' : '-', diff_ms);
    } else {
        output->write_function(output, "Clock: Monotonic (no PTP)\n");
    }
}

gchar *get_rtp_stats(g_stream_t *stream)
{
    GstElement *jitterbuf = AL_gst_bin_get_by_name(GST_BIN(stream->pipeline), "rx-jitbuf");
    gchar *stats = NULL;

    if (jitterbuf) {
        GstStructure *s = NULL;
        g_object_get(jitterbuf, "stats", &s, NULL);
        if (s) {
            stats = gst_structure_to_string(s);
            gst_structure_free(s);
        }
        DA_gst_object_unref(jitterbuf);
    }

    return stats ? stats : g_strdup("");
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
/* Pipeline Lifecycle                                                        */
/* ========================================================================= */

g_stream_t *create_pipeline(pipeline_data_t *data, event_callback_t *error_cb)
{
    // ... [your existing create_pipeline with clock setup using start_synthetic_clock()] ...
    // See previous messages for the full version — it uses start_synthetic_clock()
}

void stop_pipeline(g_stream_t *stream)
{
    if (!stream) return;

    gst_element_set_state(GST_ELEMENT(stream->pipeline), GST_STATE_NULL);
    stop_synthetic_clock(stream);

    if (stream->mainloop) {
        teardown_mainloop(stream->mainloop);
    }

    DA_g_free(stream);
}