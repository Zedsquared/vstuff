/*
 * vISDN DSSS-1/q.931 signalling library
 *
 * Copyright (C) 2004-2005 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

#ifndef _LIBQ931_CALL_H
#define _LIBQ931_CALL_H

#include <string.h>

#include <libq931/callref.h>
#include <libq931/timer.h>
#include <libq931/list.h>
#include <libq931/ie.h>
#include <libq931/message.h>
#include <libq931/ies.h>

enum q931_call_direction
{
	Q931_CALL_DIRECTION_OUTBOUND	= 0x0,
	Q931_CALL_DIRECTION_INBOUND	= 0x1,
};

enum q931_call_state
{
	U0_NULL_STATE,
	U1_CALL_INITIATED,
	U2_OVERLAP_SENDING,
	U3_OUTGOING_CALL_PROCEEDING,
	U4_CALL_DELIVERED,
	U6_CALL_PRESENT,
	U7_CALL_RECEIVED,
	U8_CONNECT_REQUEST,
	U9_INCOMING_CALL_PROCEEDING,
	U10_ACTIVE,
	U11_DISCONNECT_REQUEST,
	U12_DISCONNECT_INDICATION,
	U15_SUSPEND_REQUEST,
	U17_RESUME_REQUEST,
	U19_RELEASE_REQUEST,
	U25_OVERLAP_RECEIVING,

	N0_NULL_STATE,
	N1_CALL_INITIATED,
	N2_OVERLAP_SENDING,
	N3_OUTGOING_CALL_PROCEEDING,
	N4_CALL_DELIVERED,
	N6_CALL_PRESENT,
	N7_CALL_RECEIVED,
	N8_CONNECT_REQUEST,
	N9_INCOMING_CALL_PROCEEDING,
	N10_ACTIVE,
	N11_DISCONNECT_REQUEST,
	N12_DISCONNECT_INDICATION,
	N15_SUSPEND_REQUEST,
	N17_RESUME_REQUEST,
	N19_RELEASE_REQUEST,
	N22_CALL_ABORT,
	N25_OVERLAP_RECEIVING
};

enum q931_setup_confirm_status
{
	Q931_SETUP_CONFIRM_OK,
	Q931_SETUP_CONFIRM_ERROR,
};

enum q931_setup_complete_indication_status
{
	Q931_SETUP_COMPLETE_INDICATION_OK,
	Q931_SETUP_COMPLETE_INDICATION_ERROR,
};

enum q931_suspend_confirm_status
{
	Q931_SUSPEND_CONFIRM_OK,
	Q931_SUSPEND_CONFIRM_ERROR,
	Q931_SUSPEND_CONFIRM_TIMEOUT,
};

enum q931_release_confirm_status
{
	Q931_RELEASE_CONFIRM_OK,
	Q931_RELEASE_CONFIRM_ERROR,
};

enum q931_resume_confirm_status
{
	Q931_RESUME_CONFIRM_OK,
	Q931_RESUME_CONFIRM_ERROR,
	Q931_RESUME_CONFIRM_TIMEOUT,
};

enum q931_status_indication_status
{
	Q931_STATUS_INDICATION_OK,
	Q931_STATUS_INDICATION_ERROR,
};

#define Q931_MAX_DIGITS 20

struct q931_call
{
	struct list_head calls_node;
	int refcnt;

	struct q931_interface *intf;

	// TODO: Use a HASH for improved scalability
	struct list_head ces;
	struct q931_ces *preselected_ces;
	struct q931_ces *selected_ces;
	struct q931_dlc *dlc;

	enum q931_call_direction direction;
	q931_callref call_reference;

	enum q931_call_state state;

	int broadcast_setup;
	int tones_option;

	int T303_fired;
	int T308_fired;

	int senq_status_97_98_received;
	int senq_cnt;

	int disconnect_indication_sent;
	int release_complete_received;
	int digits_dialled;

	struct q931_ies setup_ies;

	struct q931_ies disconnect_cause;
	struct q931_ies saved_cause;
	struct q931_ies release_cause;

	// Maybe we should have a channel mask, instead
	struct q931_channel *channel;

	void *pvt;

	struct q931_timer T301;
	struct q931_timer T302;
	struct q931_timer T303;
	struct q931_timer T304;
	struct q931_timer T305;
	struct q931_timer T306;
	struct q931_timer T308;
	struct q931_timer T309;
	struct q931_timer T310;
	struct q931_timer T312;
	struct q931_timer T313;
	struct q931_timer T314;
	struct q931_timer T316;
	struct q931_timer T318;
	struct q931_timer T319;
	struct q931_timer T320;
	struct q931_timer T321;
	struct q931_timer T322;
};

struct q931_call *q931_call_alloc_out(
	struct q931_interface *intf);

const char *q931_call_state_to_text(enum q931_call_state state);

struct q931_call *q931_get_call_by_reference(
	struct q931_interface *intf,
	enum q931_call_direction direction,
	q931_callref call_reference);

#define q931_call_get(call) _q931_call_get((call), __FILE__,  __LINE__)
#define q931_call_put_nonull(call) _q931_call_put((call), __FILE__,  __LINE__)
#define q931_call_put(call) do { q931_call_put_nonull(call); call = NULL; } while(0)

struct q931_call *_q931_call_get(struct q931_call *call, const char *file, int line);
void _q931_call_put(struct q931_call *call, const char *prefix, int line);

void q931_call_release_reference(struct q931_call *call);

#ifdef Q931_PRIVATE

struct q931_call *q931_call_alloc(struct q931_interface *intf);

#define report_call(call, lvl, format, arg...)				\
	q931_report((lvl),						\
		"%s:CALL[%u.%c]: "					\
		format,							\
		(call)->intf->name,					\
		(call)->call_reference,					\
		((call)->direction ==					\
			Q931_CALL_DIRECTION_OUTBOUND ? 'O' : 'I'),	\
		## arg)

struct q931_call *q931_call_alloc_in(
	struct q931_interface *intf,
	struct q931_dlc *dlc,
	int call_reference);

void _q931_call_start_timer(
	struct q931_call *call,
	struct q931_timer *timer,
	int delta);

#define q931_call_start_timer(call, timer)			\
	do {							\
		_q931_call_start_timer(call,			\
			&(call)->timer,				\
			(call)->intf->timer);			\
		report_call(call, LOG_DEBUG,			\
			"%s:%d Timer %s started (%lld us)\n",	\
			__FILE__,__LINE__,			\
			#timer,					\
			(call)->intf->timer);			\
	} while(0)

#define q931_call_restart_timer(call, timer)			\
	do {							\
		_q931_call_restart_timer(call,			\
			&(call)->timer,				\
			(call)->intf->timer);			\
		report_call(call, LOG_DEBUG,			\
			"%s:%d Timer %s restarted (%lld us)\n",	\
			__FILE__,__LINE__,			\
			#timer,					\
			(call)->intf->timer);			\
	} while(0)

void _q931_call_stop_timer(
	struct q931_call *call,
	struct q931_timer *timer);

#define q931_call_stop_timer(call, timer)			\
	do {							\
		_q931_call_stop_timer(call, &(call)->timer);	\
		report_call(call, LOG_DEBUG,			\
			"%s:%d Timer %s stopped\n",		\
			__FILE__,__LINE__,			\
			#timer);				\
	} while(0)

#define q931_call_timer_running(call, timer)		\
		q931_timer_pending(&(call)->timer)	\

#define q931_call_primitive(call, primitive, ies)	\
		q931_queue_primitive(call, primitive, ies, 0, 0)

#define q931_call_primitive1(call, primitive, ies, par1)	\
		q931_queue_primitive(call, primitive, ies, par1, 0)

#define q931_call_primitive2(call, primitive, ies, par1, par2)	\
		q931_queue_primitive(call, primitive, ies, par1, par2)


void q931_dispatch_message(
	struct q931_call *call,
	struct q931_message *msg);

void q931_call_set_state(
	struct q931_call *call,
	enum q931_call_state state);

void q931_call_dl_establish_indication(struct q931_call *call);
void q931_call_dl_establish_confirm(struct q931_call *call);
void q931_call_dl_release_indication(struct q931_call *call);
void q931_call_dl_release_confirm(struct q931_call *call);

void q931_call_stop_any_timer(struct q931_call *call);

void q931_int_alerting_indication(
	struct q931_call *call,
	struct q931_ces *ces,
	const struct q931_ies *ies);
void q931_int_connect_indication(
	struct q931_call *call,
	struct q931_ces *ces,
	const struct q931_ies *ies);
void q931_int_call_proceeding_indication(
	struct q931_call *call,
	struct q931_ces *ces,
	const struct q931_ies *ies);
void q931_int_release_complete_indication(
	struct q931_call *call,
	struct q931_ces *ces,
	const struct q931_ies *ies);
void q931_int_info_indication(
	struct q931_call *call,
	struct q931_ces *ces,
	const struct q931_ies *ies);
void q931_int_progress_indication(
	struct q931_call *call,
	struct q931_ces *ces,
	const struct q931_ies *ies);
void q931_int_release_indication( 
	struct q931_call *call,
	struct q931_ces *ces,
	const struct q931_ies *ies);

void q931_alerting_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_disconnect_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_info_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_more_info_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_notify_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_proceeding_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_progress_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_reject_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_release_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_resume_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_resume_reject_request(
	struct q931_call *call,
	const struct q931_ies *ies);

void q931_resume_response(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_setup_complete_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_setup_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_setup_response(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_status_enquiry_request(
	struct q931_call *call,
	struct q931_ces *ces,
	const struct q931_ies *ies);
void q931_suspend_reject_request(
	struct q931_call *call,
	const struct q931_ies *ies);

void q931_suspend_response(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_suspend_request(
	struct q931_call *call,
	const struct q931_ies *ies);
void q931_restart_request(
	struct q931_call *call,
	const struct q931_ies *ies);

#endif
#endif
