/*
 * vISDN DSSS-1/q.931 signalling library
 *
 * Copyright (C) 2004-2006 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define Q931_PRIVATE

#include <assert.h>

#include <libq931/lib.h>
#include <libq931/intf.h>
#include <libq931/logging.h>
#include <libq931/global.h>
#include <libq931/input.h>
#include <libq931/output.h>
#include <libq931/chanset.h>

#include <libq931/ie_call_state.h>
#include <libq931/ie_cause.h>
#include <libq931/ie_channel_identification.h>
#include <libq931/ie_restart_indicator.h>

#define q931_global_primitive(gc, primitive)		\
		q931_queue_primitive(NULL, (primitive), NULL,\
				(unsigned long)(gc), 0);
#define q931_global_primitive1(gc, primitive, par1)		\
		q931_queue_primitive(NULL, (primitive), NULL,\
				(unsigned long)(gc), (unsigned long)(par1));

static const char *q931_global_state_to_text(enum q931_global_state state)
{
	switch (state) {
	case Q931_GLOBAL_STATE_NULL:
		return "NULL";
	case Q931_GLOBAL_STATE_RESTART_REQUEST:
		return "RESTART REQUEST";
	case Q931_GLOBAL_STATE_RESTART:
		return "RESTART";
	}

	return NULL;
}

void q931_global_set_state(
	struct q931_global_call *gc,
	enum q931_global_state state)
{
	assert(gc);

	report_intf(gc->intf, LOG_DEBUG,
		"Global call changed state from %s to %s\n",
		q931_global_state_to_text(gc->state),
		q931_global_state_to_text(state));

	gc->state = state;
}

void q931_management_restart_request(
	struct q931_global_call *gc,
	struct q931_dlc *dlc,
	struct q931_chanset *chanset,
	const struct q931_ies *user_ies)
{
	report_gc(gc, LOG_DEBUG, "RESTART-REQUEST\n");

	switch(gc->state) {
	case Q931_GLOBAL_STATE_NULL: {

		gc->T317_expired = FALSE;
		gc->restart_retransmit_count = 0;
		gc->restart_responded = FALSE;
		gc->restart_acknowledged = 0;

		Q931_DECLARE_IES(ies);

		if (gc->restart_indicator->restart_class ==
			       		Q931_IE_RI_C_INDICATED) {

			q931_chanset_init(&gc->restart_acked_chans);
			q931_chanset_copy(&gc->restart_reqd_chans, chanset);

			struct q931_ie_channel_identification *ci =
				q931_ie_channel_identification_alloc();
			ci->interface_id_present = Q931_IE_CI_IIP_IMPLICIT;
			ci->interface_type =
				q931_ie_channel_identification_intftype(
								gc->intf);
			ci->preferred_exclusive = Q931_IE_CI_PE_EXCLUSIVE;
			ci->coding_standard = Q931_IE_CI_CS_CCITT;
			q931_chanset_init(&ci->chanset);
			q931_chanset_copy(&ci->chanset,
					&gc->restart_reqd_chans);
			q931_ies_add_put(&ies, &ci->ie);

			struct q931_ie_restart_indicator *ri =
				q931_ie_restart_indicator_alloc();
			ri->restart_class = Q931_IE_RI_C_INDICATED;
			q931_ies_add_put(&ies, &ri->ie);

		} else if (gc->restart_indicator->restart_class ==
			       		Q931_IE_RI_C_SINGLE_INTERFACE) {

			struct q931_ie_restart_indicator *ri =
				q931_ie_restart_indicator_alloc();
			ri->restart_class = Q931_IE_RI_C_SINGLE_INTERFACE;
			q931_ies_add_put(&ies, &ri->ie);

		} else if (gc->restart_indicator->restart_class ==
			       		Q931_IE_RI_C_ALL_INTERFACES) {
			assert(0);
		}

		Q931_UNDECLARE_IES(ies);

		gc->restart_dlc = dlc; // GET/PUT reference TODO

		q931_global_send_message(gc, dlc, Q931_MT_RESTART, &ies);
		q931_global_start_timer(gc, T316);
		q931_global_set_state(gc, Q931_GLOBAL_STATE_RESTART_REQUEST);

		struct q931_call *call;
		list_for_each_entry(call, &gc->intf->calls, calls_node) {
#if 0
			if (q931_chanset_contains(chanset, call->channel)) {
				q931_restart_request(call, user_ies);
				gc->restart_request_count++;

				if (!q931_global_timer_running(gc, T317))
					q931_global_start_timer(gc, T317);
			} else {
				gc->restart_request_count++;
				q931_global_restart_confirm(gc, dlc,
						/*FIXME*/ NULL);
			}
#endif
		}
	}
	break;

	case Q931_GLOBAL_STATE_RESTART_REQUEST:
	break;

	case Q931_GLOBAL_STATE_RESTART:
	break;
	}
}

void q931_global_restart_confirm(
	struct q931_global_call *gc,
	struct q931_dlc *dlc,
	struct q931_channel *chan)
{
	report_gc(gc, LOG_DEBUG, "RESTART-CONFIRM\n");

	switch(gc->state) {
	case Q931_GLOBAL_STATE_RESTART_REQUEST:
		if (chan) {
			q931_chanset_del(&gc->restart_reqd_chans, chan);
			q931_chanset_add(&gc->restart_acked_chans, chan);
		}

		if (q931_chanset_count(&gc->restart_reqd_chans) == 0) {

			q931_global_stop_timer(gc, T317);

			if (gc->restart_acknowledged) {
				struct q931_chanset cs;
				q931_chanset_init(&cs);

				q931_chanset_intersect(
					&gc->restart_reqd_chans,
					&gc->restart_acked_chans);

				q931_global_set_state(gc,
					Q931_GLOBAL_STATE_NULL);
				q931_global_primitive1(gc,
					Q931_CCB_MANAGEMENT_RESTART_CONFIRM,
					&cs);
			} else {
				gc->restart_responded = TRUE;
			}
		}
	break;

	case Q931_GLOBAL_STATE_RESTART:
		if (chan) {
			q931_chanset_del(&gc->restart_reqd_chans, chan);
			q931_chanset_add(&gc->restart_acked_chans, chan);
		}

		if (q931_chanset_count(&gc->restart_reqd_chans) == 0) {

			q931_global_stop_timer(gc, T317);
			q931_global_set_state(gc, Q931_GLOBAL_STATE_NULL);

			Q931_DECLARE_IES(ies);

			if (gc->restart_indicator->restart_class ==
				       		Q931_IE_RI_C_INDICATED) {

				struct q931_ie_channel_identification *ci =
					q931_ie_channel_identification_alloc();

				ci->interface_id_present =
					Q931_IE_CI_IIP_IMPLICIT;
				ci->interface_type =
					q931_ie_channel_identification_intftype(
								gc->intf);
				ci->coding_standard = Q931_IE_CI_CS_CCITT;
				ci->preferred_exclusive =
					Q931_IE_CI_PE_EXCLUSIVE;
				q931_chanset_init(&ci->chanset);
				q931_chanset_copy(&ci->chanset,
					&gc->restart_acked_chans);
				q931_ies_add_put(&ies, &ci->ie);

				struct q931_ie_restart_indicator *ri =
					q931_ie_restart_indicator_alloc();
				ri->restart_class = Q931_IE_RI_C_INDICATED;
				q931_ies_add_put(&ies, &ri->ie);
			} else if (gc->restart_indicator->restart_class ==
				       		Q931_IE_RI_C_SINGLE_INTERFACE) {

				struct q931_ie_restart_indicator *ri =
					q931_ie_restart_indicator_alloc();
				ri->restart_class =
					Q931_IE_RI_C_SINGLE_INTERFACE;
				q931_ies_add_put(&ies, &ri->ie);

			} else if (gc->restart_indicator->restart_class ==
				       		Q931_IE_RI_C_ALL_INTERFACES) {
				assert(0);
			}

			q931_global_send_message(gc, dlc,
				Q931_MT_RESTART_ACKNOWLEDGE, &ies);

			Q931_UNDECLARE_IES(ies);
		}
	break;

	case Q931_GLOBAL_STATE_NULL:
		// Unexpected primitive
	break;
	}
}

static __u8 q931_global_state_to_ie_state(enum q931_global_state state)
{
	switch (state) {
	case Q931_GLOBAL_STATE_NULL:
		return Q931_IE_CS_REST0;
	case Q931_GLOBAL_STATE_RESTART_REQUEST:
		return Q931_IE_CS_REST1;
	case Q931_GLOBAL_STATE_RESTART:
		return Q931_IE_CS_REST2;
	}

	return 0;
}

static void q931_global_handle_status(
	struct q931_global_call *gc,
	struct q931_message *msg)
{
	switch(gc->state) {
	case Q931_GLOBAL_STATE_NULL:
		// Do nothing
	break;

	case Q931_GLOBAL_STATE_RESTART_REQUEST:
	case Q931_GLOBAL_STATE_RESTART: {
		if (q931_gc_decode_ies(gc, msg) < 0)
			break;

		struct q931_ie_call_state *cs = NULL;

		int i;
		for(i=0; i<msg->ies.count; i++) {
			if (msg->ies.ies[i]->cls->id == Q931_IE_CALL_STATE) {
				cs = container_of(msg->ies.ies[i],
					struct q931_ie_call_state, ie);
				break;
			}
		}

		if (cs->value == q931_global_state_to_ie_state(gc->state)) {
			// Implementation dependent
		} else {
			q931_global_primitive(gc,
				Q931_CCB_STATUS_MANAGEMENT_INDICATION);
			// WITH ERROR
		}
	}
	break;
	}
}

static int q931_channel_is_restartable(
	struct q931_channel *chan,
	struct q931_dlc *dlc)
{
	return chan->state != Q931_CHANSTATE_MAINTAINANCE &&
		chan->state != Q931_CHANSTATE_LEASED;
/* 
 * Telecom Italia needs all requested channels to be restarted, even if no
 * calls are active on them
 *
 *		 && (!chan->call || chan->call->dlc == dlc);
*/
}

static void q931_global_handle_restart(
	struct q931_global_call *gc,
	struct q931_message *msg)
{
	switch(gc->state) {
	case Q931_GLOBAL_STATE_NULL:
		if (q931_gc_decode_ies(gc, msg) < 0)
			break;

		gc->T317_expired = FALSE;
		gc->restart_retransmit_count = 0;
		gc->restart_responded = FALSE;
		gc->restart_acknowledged = 0;
		gc->restart_dlc = msg->dlc;
		q931_chanset_init(&gc->restart_reqd_chans);
		q931_chanset_init(&gc->restart_acked_chans);

		struct q931_ie_channel_identification *ci = NULL;
		int i;
		for (i=0; i<msg->ies.count; i++) {
			if (msg->ies.ies[i]->cls->id ==
					Q931_IE_CHANNEL_IDENTIFICATION) {

				ci = container_of(msg->ies.ies[i],
					struct q931_ie_channel_identification,
						ie);

				int j;
				for (j=0; j<q931_chanset_count(&ci->chanset);
									j++) {
					if (q931_channel_is_restartable(
							ci->chanset.chans[j],
							msg->dlc)) {
						q931_chanset_add(
							&gc->restart_reqd_chans,
							ci->chanset.chans[j]);
					}
				}
			} else if (msg->ies.ies[i]->cls->id ==
					Q931_IE_RESTART_INDICATOR) {

				if (gc->restart_indicator) {
					_q931_ie_put(&gc->restart_indicator->ie);
					gc->restart_indicator = NULL;
				}

				gc->restart_indicator =
					container_of(q931_ie_get(
							msg->ies.ies[i]),
					struct q931_ie_restart_indicator,
					ie);
			}
		}

		if (!ci) {
			for (i=0; i<gc->intf->n_channels; i++) {
				if (q931_channel_is_restartable(
					&gc->intf->channels[i], msg->dlc)) {
					q931_chanset_add(
						&gc->restart_reqd_chans,
						&gc->intf->channels[i]);
				}
			}
		}

		// Any indicated channel allowed to be restarted?
		if (q931_chanset_count(&gc->restart_reqd_chans)) {
			q931_global_set_state(gc, Q931_GLOBAL_STATE_RESTART);

			/* Ugly hack to safely delete current within loop */
			struct q931_chanset cs;
			q931_chanset_init(&cs);
			q931_chanset_copy(&cs, &gc->restart_reqd_chans);
			
			for (i=0;i<q931_chanset_count(&cs); i++) {

				if (cs.chans[i]->call) {
					q931_restart_request(
						cs.chans[i]->call,
						&msg->ies);

					if (!q931_global_timer_running(gc,
									T317))
						q931_global_start_timer(gc,
									T317);
				} else {
					q931_global_restart_confirm(gc,
						msg->dlc,
						cs.chans[i]);
				}
			}
		} else {
			Q931_DECLARE_IES(ies);

			struct q931_ie_cause *cause = q931_ie_cause_alloc();
			cause->coding_standard = Q931_IE_C_CS_CCITT;
			cause->location = q931_ie_cause_location_gc(gc);
			cause->value =
				Q931_IE_C_CV_IDENTIFIED_CHANNEL_DOES_NOT_EXIST;
			q931_ies_add_put(&ies, &cause->ie);

			q931_global_send_message(gc, msg->dlc,
				Q931_MT_STATUS, &ies);

			Q931_UNDECLARE_IES(ies);
		}
	break;

	case Q931_GLOBAL_STATE_RESTART_REQUEST:
	case Q931_GLOBAL_STATE_RESTART: {
		Q931_DECLARE_IES(ies);
		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_gc(gc);
		cause->value = Q931_IE_C_CV_INVALID_CALL_REFERENCE_VALUE;
		q931_ies_add_put(&ies, &cause->ie);

		q931_global_send_message(gc, msg->dlc, Q931_MT_STATUS, &ies);

		Q931_UNDECLARE_IES(ies);
	}
	break;
	}
}

static void q931_global_handle_restart_acknowledge(
	struct q931_global_call *gc,
	struct q931_message *msg)
{
	switch(gc->state) {
	case Q931_GLOBAL_STATE_RESTART_REQUEST:
		if (q931_gc_decode_ies(gc, msg) < 0)
			break;

		q931_global_stop_timer(gc, T316);

		if (gc->restart_responded || gc->T317_expired) {

			struct q931_chanset cs;
			q931_chanset_init(&cs);

			q931_chanset_intersect(
				&gc->restart_reqd_chans,
				&gc->restart_acked_chans);

			q931_global_primitive1(gc,
				Q931_CCB_MANAGEMENT_RESTART_CONFIRM, &cs);

			q931_global_set_state(gc, Q931_GLOBAL_STATE_NULL);
		} else if (!gc->T317_expired) {

			// Store channel id received in restart ack
			int i;
			for (i=0; i<msg->ies.count; i++) {
				if (msg->ies.ies[i]->cls->id ==
					Q931_IE_CHANNEL_IDENTIFICATION) {

					struct q931_ie_channel_identification *ci =
						container_of(msg->ies.ies[i],
						struct q931_ie_channel_identification, ie);

					q931_chanset_merge(
						&gc->restart_acked_chans,
						&ci->chanset);

					break;
				}
			}

			gc->restart_acknowledged = TRUE;
		}
	break;

	case Q931_GLOBAL_STATE_NULL:
	case Q931_GLOBAL_STATE_RESTART: {
		Q931_DECLARE_IES(ies);
		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_gc(gc);
		cause->value = Q931_IE_C_CV_INVALID_CALL_REFERENCE_VALUE;
		q931_ies_add_put(&ies, &cause->ie);

		q931_global_send_message(gc, msg->dlc, Q931_MT_STATUS, &ies);

		Q931_UNDECLARE_IES(ies);
	}
	break;
	}
}

void q931_global_timer_T316(void *data)
{
	struct q931_global_call *gc = data;
	assert(data);

	switch(gc->state) {
	case Q931_GLOBAL_STATE_RESTART_REQUEST:
		gc->restart_retransmit_count++;

		if (gc->restart_retransmit_count >= 2) {
			q931_global_set_state(gc, Q931_GLOBAL_STATE_NULL);

			// Indicate that restart has failed
			q931_global_primitive1(gc,
				Q931_CCB_MANAGEMENT_RESTART_CONFIRM, NULL);
		} else {
			Q931_DECLARE_IES(ies);

			if (gc->restart_indicator->restart_class ==
				       		Q931_IE_RI_C_INDICATED) {

				struct q931_ie_channel_identification *ci =
					q931_ie_channel_identification_alloc();

				ci->interface_id_present =
					Q931_IE_CI_IIP_IMPLICIT;
				ci->interface_type =
					q931_ie_channel_identification_intftype(
						gc->intf);
				ci->preferred_exclusive =
					Q931_IE_CI_PE_EXCLUSIVE;
				ci->coding_standard = Q931_IE_CI_CS_CCITT;
				q931_chanset_init(&ci->chanset);
				q931_chanset_copy(&ci->chanset,
					&gc->restart_reqd_chans);
				q931_ies_add_put(&ies, &ci->ie);

				struct q931_ie_restart_indicator *ri =
					q931_ie_restart_indicator_alloc();
				ri->restart_class = Q931_IE_RI_C_INDICATED;
				q931_ies_add_put(&ies, &ri->ie);

			} else if (gc->restart_indicator->restart_class ==
				       		Q931_IE_RI_C_SINGLE_INTERFACE) {

				struct q931_ie_restart_indicator *ri =
					q931_ie_restart_indicator_alloc();
				ri->restart_class =
					Q931_IE_RI_C_SINGLE_INTERFACE;
				q931_ies_add_put(&ies, &ri->ie);

			} else if (gc->restart_indicator->restart_class ==
				       		Q931_IE_RI_C_ALL_INTERFACES) {
			} else {
				assert(0);
			}

			assert(gc->restart_dlc);
			q931_global_send_message(gc, gc->restart_dlc,
				Q931_MT_RESTART, &ies);

			q931_global_start_timer(gc, T316);

			Q931_UNDECLARE_IES(ies);
		}
	break;

	case Q931_GLOBAL_STATE_NULL:
	case Q931_GLOBAL_STATE_RESTART:
		// Unexpected timer
	break;
	}
}

void q931_global_timer_T317(void *data)
{
	struct q931_global_call *gc = data;
	assert(data);

	switch(gc->state) {
	case Q931_GLOBAL_STATE_NULL:
		// Unexpected timer
	break;

	case Q931_GLOBAL_STATE_RESTART_REQUEST:
		if (gc->restart_acknowledged) {
			struct q931_chanset cs;
			q931_chanset_init(&cs);

			q931_chanset_intersect(
				&gc->restart_reqd_chans,
				&gc->restart_acked_chans);

			q931_global_set_state(gc, Q931_GLOBAL_STATE_NULL);

			q931_global_primitive1(gc,
				Q931_CCB_MANAGEMENT_RESTART_CONFIRM, &cs);
		} else {
			gc->T317_expired = TRUE;
		}
	break;

	case Q931_GLOBAL_STATE_RESTART:
		// Any channel restarted?
		if (q931_chanset_count(&gc->restart_acked_chans)) {
			Q931_DECLARE_IES(ies);

			struct q931_ie_channel_identification *ci =
				q931_ie_channel_identification_alloc();
			ci->interface_id_present = Q931_IE_CI_IIP_IMPLICIT;
			ci->interface_type =
				q931_ie_channel_identification_intftype(
							gc->intf);
			ci->preferred_exclusive = Q931_IE_CI_PE_EXCLUSIVE;
			ci->coding_standard = Q931_IE_CI_CS_CCITT;
			q931_chanset_init(&ci->chanset);
			q931_chanset_copy(&ci->chanset,
				&gc->restart_acked_chans);
			q931_ies_add_put(&ies, &ci->ie);

			struct q931_ie_restart_indicator *ri =
				q931_ie_restart_indicator_alloc();
			ri->restart_class = Q931_IE_RI_C_INDICATED;
			q931_ies_add_put(&ies, &ri->ie);

			assert(gc->restart_dlc);
			q931_global_send_message(gc, gc->restart_dlc,
				Q931_MT_RESTART, &ies);

			Q931_UNDECLARE_IES(ies);
		}

		q931_global_set_state(gc, Q931_GLOBAL_STATE_NULL);
		q931_global_primitive(gc,
			Q931_CCB_TIMEOUT_MANAGEMENT_INDICATION);
	break;
	}
}

void q931_dispatch_global_message(
	struct q931_global_call *gc,
	struct q931_message *msg)
{
	switch(msg->message_type) {
	case Q931_MT_STATUS:
		q931_global_handle_status(gc, msg);
	break;

	case Q931_MT_RESTART:
		q931_global_handle_restart(gc, msg);
	break;

	case Q931_MT_RESTART_ACKNOWLEDGE:
		q931_global_handle_restart_acknowledge(gc, msg);
	break;

	default: {
		Q931_DECLARE_IES(ies);
		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_gc(gc);
		cause->value = Q931_IE_C_CV_INVALID_CALL_REFERENCE_VALUE;
		q931_ies_add_put(&ies, &cause->ie);

		struct q931_ie_call_state *cs = q931_ie_call_state_alloc();
		cs->coding_standard = Q931_IE_CS_CS_CCITT;
		cs->value = q931_global_state_to_ie_state(gc->state);
		q931_ies_add_put(&ies, &cs->ie);

		q931_global_send_message(gc, msg->dlc, Q931_MT_STATUS, &ies);

		Q931_UNDECLARE_IES(ies);
	}
	break;
	}
}

void q931_global_init(
	struct q931_global_call *gc,
	struct q931_interface *intf)
{
	gc->intf = intf;

	q931_init_timer(&gc->T316, "T316", q931_global_timer_T316, gc);
	q931_init_timer(&gc->T317, "T317", q931_global_timer_T317, gc);

}

void q931_global_destroy(struct q931_global_call *gc)
{
	if (gc->restart_indicator) {
		_q931_ie_put(&gc->restart_indicator->ie);
		gc->restart_indicator = NULL;
	}
}
