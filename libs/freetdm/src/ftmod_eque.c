/*
 * ftmod_eque.c -- FreeTDM bare-bearer always-up signaling stub
 *
 * "Eque" (from Latin aequus, "level / constant") presents every B-channel on
 * a span as a permanently connected bearer circuit.  There is no D-channel,
 * no CAS, and no call-setup handshake.  Each channel fires SIGEVENT_START as
 * soon as the span is started and is automatically re-offered to FreeSWITCH
 * whenever it falls back to the DOWN state (e.g. after a hangup).
 *
 * Typical use-case: dedicated T1/E1 leased-line audio circuits or any
 * bearer-only TDM span that carries raw PCM with no associated signaling.
 *
 * freetdm.conf snippet
 * --------------------
 *   [span wanpipe myspan]
 *   trunk_type = T1
 *   b-channel  = 1-23
 *
 * freetdm.conf.xml / mod_freetdm signaling block
 * ------------------------------------------------
 *   <param name="signaling"          value="eque"/>
 *   <param name="reconnect-delay-ms" value="2000"/>
 *
 * Copyright (c) 2024, GERRAudio / networkedaudio
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the authors nor the names of contributors may be used
 *    to endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "private/ftdm_core.h"

/* -------------------------------------------------------------------------
 * Module-private span state
 * ---------------------------------------------------------------------- */

#define EQUE_DEFAULT_RECONNECT_MS  2000u   /* delay before re-offering a channel */
#define EQUE_POLL_INTERVAL_MS        100u  /* main-loop wakeup resolution        */

typedef struct {
	uint32_t reconnect_ms;   /* configurable re-offer delay               */
	uint32_t poll_ticks;     /* accumulator for reconnect_ms / poll step  */
} eque_span_data_t;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void eque_send_sigstatus(ftdm_span_t *span, ftdm_signaling_status_t status)
{
	uint32_t i;
	ftdm_sigmsg_t msg;

	for (i = 1; i <= span->chan_count; i++) {
		ftdm_channel_t *fchan = span->channels[i];
		if (!fchan) {
			continue;
		}
		memset(&msg, 0, sizeof(msg));
		msg.event_id                  = FTDM_SIGEVENT_SIGSTATUS_CHANGED;
		msg.channel                   = fchan;
		msg.span_id                   = fchan->span_id;
		msg.chan_id                   = fchan->chan_id;
		msg.ev_data.sigstatus.status  = status;
		ftdm_span_send_signal(span, &msg);
	}
}

static void eque_offer_channel(ftdm_channel_t *fchan)
{
	ftdm_sigmsg_t msg;
	memset(&msg, 0, sizeof(msg));
	msg.event_id = FTDM_SIGEVENT_START;
	msg.channel  = fchan;
	msg.span_id  = fchan->span_id;
	msg.chan_id  = fchan->chan_id;

	/* Synthesise bare caller data: no number, bearer capability = speech */
	memset(&fchan->caller_data, 0, sizeof(fchan->caller_data));
	fchan->caller_data.bearer_capability = FTDM_BEARER_CAP_SPEECH;

	ftdm_log_chan(fchan, FTDM_LOG_DEBUG, "mod_eque: offering channel s%dc%d\n",
				  fchan->span_id, fchan->chan_id);
	ftdm_span_send_signal(fchan->span, &msg);
}

/* Returns non-zero if the channel is idle (DOWN and not claimed) */
static int eque_chan_is_idle(ftdm_channel_t *fchan)
{
	int idle;
	ftdm_channel_lock(fchan);
	idle = (fchan->state == FTDM_CHANNEL_STATE_DOWN)
		&& !ftdm_test_flag(fchan, FTDM_CHANNEL_INUSE)
		&& !ftdm_test_flag(fchan, FTDM_CHANNEL_CALL_STARTED);
	ftdm_channel_unlock(fchan);
	return idle;
}

/* -------------------------------------------------------------------------
 * Span worker thread
 * ---------------------------------------------------------------------- */

static void *eque_run(ftdm_thread_t *me, void *obj)
{
	ftdm_span_t      *span = (ftdm_span_t *)obj;
	eque_span_data_t *sd   = (eque_span_data_t *)span->signal_data;
	uint32_t          i;
	uint32_t          ticks = 0;   /* counts EQUE_POLL_INTERVAL_MS steps   */
	uint32_t          reconnect_ticks;

	ftdm_unused_arg(me);

	ftdm_set_flag(span, FTDM_SPAN_IN_THREAD);
	reconnect_ticks = sd->reconnect_ms / EQUE_POLL_INTERVAL_MS;
	if (reconnect_ticks == 0) {
		reconnect_ticks = 1;
	}

	ftdm_log(FTDM_LOG_INFO, "mod_eque: span '%s' worker started "
			 "(reconnect_ms=%u)\n", span->name, sd->reconnect_ms);

	/* Advertise the signaling layer as UP */
	eque_send_sigstatus(span, FTDM_SIG_STATE_UP);

	/* Initial pass: offer every B-channel immediately */
	for (i = 1; i <= span->chan_count; i++) {
		ftdm_channel_t *fchan = span->channels[i];
		if (fchan && fchan->type == FTDM_CHAN_TYPE_B) {
			eque_offer_channel(fchan);
		}
	}

	/* Main loop: re-offer channels that have fallen idle */
	while (ftdm_running() && !ftdm_test_flag(span, FTDM_SPAN_STOP_THREAD)) {
		ftdm_sleep(EQUE_POLL_INTERVAL_MS);
		ticks++;

		if (ticks < reconnect_ticks) {
			continue;
		}
		ticks = 0;

		for (i = 1; i <= span->chan_count; i++) {
			ftdm_channel_t *fchan = span->channels[i];
			if (!fchan || fchan->type != FTDM_CHAN_TYPE_B) {
				continue;
			}
			if (eque_chan_is_idle(fchan)) {
				eque_offer_channel(fchan);
			}
		}
	}

	/* Advertise the signaling layer as DOWN before exiting */
	eque_send_sigstatus(span, FTDM_SIG_STATE_DOWN);

	ftdm_log(FTDM_LOG_INFO, "mod_eque: span '%s' worker exited\n", span->name);
	ftdm_clear_flag(span, FTDM_SPAN_IN_THREAD);
	return NULL;
}

/* -------------------------------------------------------------------------
 * Span life-cycle callbacks (set on span->start / stop / destroy)
 * ---------------------------------------------------------------------- */

static FIO_SPAN_START_FUNCTION(eque_start)
{
	ftdm_clear_flag(span, FTDM_SPAN_STOP_THREAD);
	return ftdm_thread_create_detached(eque_run, span);
}

static FIO_SPAN_STOP_FUNCTION(eque_stop)
{
	ftdm_set_flag(span, FTDM_SPAN_STOP_THREAD);
	/* Block until the worker thread exits */
	while (ftdm_test_flag(span, FTDM_SPAN_IN_THREAD)) {
		ftdm_log(FTDM_LOG_DEBUG,
				 "mod_eque: waiting for span '%s' thread to stop\n",
				 span->name);
		ftdm_sleep(EQUE_POLL_INTERVAL_MS);
	}
	return FTDM_SUCCESS;
}

static ftdm_status_t eque_destroy(ftdm_span_t *span)
{
	ftdm_safe_free(span->signal_data);
	return FTDM_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Channel indication callback
 *
 * For a bare bearer we auto-acknowledge all indications.  ANSWER causes an
 * immediate SIGEVENT_UP so FreeSWITCH knows audio is flowing.
 * ---------------------------------------------------------------------- */

static FIO_CHANNEL_INDICATE_FUNCTION(eque_indicate)
{
	if (indication == FTDM_CHANNEL_INDICATE_ANSWER) {
		ftdm_sigmsg_t msg;
		memset(&msg, 0, sizeof(msg));
		msg.event_id = FTDM_SIGEVENT_UP;
		msg.channel  = ftdmchan;
		msg.span_id  = ftdmchan->span_id;
		msg.chan_id  = ftdmchan->chan_id;
		ftdm_span_send_signal(ftdmchan->span, &msg);
	}
	ftdm_ack_indication(ftdmchan, indication, FTDM_SUCCESS);
	return FTDM_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Outgoing call
 *
 * An outgoing call to a bare-bearer channel is immediately considered
 * connected; the caller will receive SIGEVENT_UP shortly after.
 * ---------------------------------------------------------------------- */

static FIO_CHANNEL_OUTGOING_CALL_FUNCTION(eque_outgoing_call)
{
	ftdm_sigmsg_t msg;
	memset(&msg, 0, sizeof(msg));
	msg.event_id = FTDM_SIGEVENT_UP;
	msg.channel  = ftdmchan;
	msg.span_id  = ftdmchan->span_id;
	msg.chan_id  = ftdmchan->chan_id;
	ftdm_span_send_signal(ftdmchan->span, &msg);
	return FTDM_SUCCESS;
}

/* -------------------------------------------------------------------------
 * configure_span_signaling  (the main entry-point called by
 * ftdm_configure_span_signaling when type == "eque")
 * ---------------------------------------------------------------------- */

static FIO_CONFIGURE_SPAN_SIGNALING_FUNCTION(eque_configure_span_signaling)
{
	eque_span_data_t *sd = NULL;
	uint32_t          reconnect_ms = EQUE_DEFAULT_RECONNECT_MS;
	int               i;

	for (i = 0; parameters[i].var; i++) {
		if (!strcasecmp(parameters[i].var, "reconnect-delay-ms")) {
			int v = atoi(parameters[i].val);
			if (v > 0) {
				reconnect_ms = (uint32_t)v;
			} else {
				ftdm_log(FTDM_LOG_WARNING,
						 "mod_eque: invalid reconnect-delay-ms '%s', "
						 "using default %u\n",
						 parameters[i].val, EQUE_DEFAULT_RECONNECT_MS);
			}
		} else {
			ftdm_log(FTDM_LOG_WARNING,
					 "mod_eque: unknown parameter '%s' (ignored)\n",
					 parameters[i].var);
		}
	}

	sd = (eque_span_data_t *)ftdm_calloc(1, sizeof(*sd));
	if (!sd) {
		ftdm_log(FTDM_LOG_CRIT, "mod_eque: out of memory\n");
		return FTDM_FAIL;
	}
	sd->reconnect_ms = reconnect_ms;

	span->signal_data  = sd;
	span->signal_type  = FTDM_SIGTYPE_ANALOG;  /* non-NONE so ftdm_span_start calls span->start */
	span->signal_cb    = sig_cb;
	span->start        = eque_start;
	span->stop         = eque_stop;
	span->destroy      = eque_destroy;
	span->indicate     = eque_indicate;
	span->outgoing_call = eque_outgoing_call;

	/* Use the pending-channels queue for state-change delivery */
	ftdm_set_flag(span, FTDM_SPAN_USE_CHAN_QUEUE);
	/* Allow direct jump to UP without PROGRESS/PROGRESS_MEDIA intermediaries */
	ftdm_set_flag(span, FTDM_SPAN_USE_SKIP_STATES);

	ftdm_log(FTDM_LOG_INFO,
			 "mod_eque: configured span '%s' "
			 "(channels=%u, reconnect_ms=%u)\n",
			 span->name, span->chan_count, reconnect_ms);

	return FTDM_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Module load / unload
 * ---------------------------------------------------------------------- */

static FIO_SIG_LOAD_FUNCTION(eque_sig_load)
{
	ftdm_log(FTDM_LOG_INFO, "mod_eque: signaling module loaded\n");
	return FTDM_SUCCESS;
}

static FIO_SIG_UNLOAD_FUNCTION(eque_sig_unload)
{
	ftdm_log(FTDM_LOG_INFO, "mod_eque: signaling module unloaded\n");
	return FTDM_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Module descriptor  (exported symbol located by ftdm_load_module)
 * ---------------------------------------------------------------------- */

ftdm_module_t ftdm_module = {
	"eque",              /* name: used as the "signaling" type string */
	NULL,                /* io_load   — no I/O component               */
	NULL,                /* io_unload                                  */
	eque_sig_load,       /* sig_load                                   */
	NULL,                /* sig_configure  (deprecated va_list form)   */
	eque_sig_unload,     /* sig_unload                                 */
	eque_configure_span_signaling  /* configure_span_signaling          */
};

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
