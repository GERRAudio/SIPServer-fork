#ifndef __GSTREAMER_API__
#define __GSTREAMER_API__

#include <gst/gst.h>
#include <switch.h>

#define DIRECTION_TX 1 << 0
#define DIRECTION_RX 1 << 1

#define IP_ADDR_MAX_LEN 20
#define AUDIO_FMT_STR_LEN 10

#define MAX_IO_CHANNELS 256

#define TS_CONTEXT_NAME_LEN 100
#define SESSION_ID_LEN 20

typedef enum
{ L16, L24 } aes67_codec_t;

typedef struct g_stream g_stream_t;

typedef void event_callback_t (gchar * error_msg, g_stream_t * stream);
typedef struct
{
  char rx_ip_addr[IP_ADDR_MAX_LEN];
  int rx_port;
  char tx_ip_addr[IP_ADDR_MAX_LEN];
  int tx_port;
  int direction;
  int sample_rate;
  char bit_depth[AUDIO_FMT_STR_LEN];
  int channels;
  aes67_codec_t tx_codec;
  aes67_codec_t rx_codec;
  int codec_ms;
  char *name;
  double ptime_ms;
  GstClock *clock;
  gint synthetic_ptp;
  double rtp_ts_offset;
  char *rtp_iface;
  int rtp_payload_type;
  int rtp_jitbuf_latency;
  gboolean txdrop;
  char *ts_context_name;
  gboolean is_backup_sender;
  int backup_sender_idle_wait_ms;
} pipeline_data_t;

struct g_stream
{
  GstPipeline *pipeline;
  GMainLoop *mainloop;
  GThread *thread;
  unsigned char leftover[MAX_IO_CHANNELS][SWITCH_RECOMMENDED_BUFFER_SIZE];
  size_t leftover_bytes[MAX_IO_CHANNELS];
  event_callback_t *error_cb;
  guint cb_rx_stats_id;
  volatile gint clock_sync;
  GstClock *clock;
  gint sample_rate;
  char *ts_ctx;
  gboolean pause_backup_sender;
  gboolean txdrop;
  guint backup_sender_idle_timer;
  int backup_sender_idle_wait_ms;
};

g_stream_t *create_pipeline (pipeline_data_t *data, event_callback_t * error_cb);
void *start_pipeline (void *data);
void stop_pipeline (g_stream_t * pipeline);
void teardown_mainloop (GMainLoop * loop);
void start_mainloop (GMainLoop * loop);

gboolean push_buffer (g_stream_t *stream, unsigned char *payload, guint len,
    guint ch_idx, switch_timer_t * timer);
int pull_buffers (g_stream_t * stream, unsigned char *payload, guint buflen,
    guint ch_idx, switch_timer_t * timer, gchar *session);
void drop_input_buffers (gboolean drop, g_stream_t * stream, guint32 ch_idx);
gchar *get_rtp_stats (g_stream_t *stream);
void drop_output_buffers (gboolean drop, g_stream_t * stream);
gboolean add_appsink(g_stream_t *stream, guint ch_idx, gchar *session);
gboolean remove_appsink(g_stream_t *stream, guint ch_idx, gchar *session);
void use_ptp_clock(g_stream_t *stream, GstClock *ptp_clock);
void dump_pipeline (GstPipeline *pipe, const char *name);

#endif /*__GSTREAMER_API__*/
