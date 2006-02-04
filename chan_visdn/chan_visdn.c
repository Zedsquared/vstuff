/*
 * vISDN channel driver for Asterisk
 *
 * Copyright (C) 2004-2006 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

//#include <asterisk/astmm.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <ctype.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <net/if_arp.h>

#include "../config.h"

#include <asterisk/lock.h>
#include <asterisk/channel.h>
#include <asterisk/config.h>
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <asterisk/pbx.h>
#include <asterisk/options.h>
#include <asterisk/utils.h>
#include <asterisk/callerid.h>
#include <asterisk/indications.h>
#include <asterisk/cli.h>
#include <asterisk/musiconhold.h>
#include <asterisk/causes.h>
#include <asterisk/dsp.h>
#include <asterisk/version.h>

#include <linux/lapd.h>
#include <linux/visdn/netdev.h>
#include <linux/visdn/streamport.h>
#include <linux/visdn/cxc.h>
#include <linux/visdn/ec.h>
#include <linux/visdn/router.h>

#include <libq931/q931.h>

#include "chan_visdn.h"
#include "util.h"
#include "huntgroup.h"
#include "overlap.h"

static pthread_t visdn_q931_thread = AST_PTHREADT_NULL;

struct visdn_state visdn = {
	.usecnt = 0,
#ifdef DEBUG_DEFAULTS
	.debug = TRUE,
	.debug_q921 = FALSE,
	.debug_q931 = TRUE,
#else
	.debug = FALSE,
	.debug_q921 = FALSE,
	.debug_q931 = FALSE,
#endif

	.default_intf = {
		.network_role = Q931_INTF_NET_PRIVATE,
		.outbound_called_ton = VISDN_TYPE_OF_NUMBER_UNKNOWN,
		.force_outbound_cli = "",
		.force_outbound_cli_ton = VISDN_TYPE_OF_NUMBER_UNSET,
		.tones_option = TRUE,
		.context = "visdn",
		.clip_enabled = TRUE,
		.clip_override = FALSE,
		.clip_default_name = "",
		.clip_default_number = "",
		.clip_numbers_list =
			LIST_HEAD_INIT(visdn.default_intf.clip_numbers_list),
		.clip_special_arrangement = FALSE,
		.clir_mode = VISDN_CLIR_MODE_DEFAULT_OFF,
		.overlap_sending = TRUE,
		.overlap_receiving = FALSE,
		.call_bumping = FALSE, 
		.cli_rewriting = FALSE,
		.national_prefix = "",
		.international_prefix = "",
		.network_specific_prefix = "",
		.subscriber_prefix = "",
		.abbreviated_prefix = "",
		.dlc_autorelease_time = 10,
		.echocancel = FALSE,
		.echocancel_taps = 256,
		.T307 = 180,
	}
};

static void visdn_set_socket_debug(int on)
{
	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {
		if (!intf->q931_intf)
			continue;

		if (intf->q931_intf->role == LAPD_INTF_ROLE_NT) {
			setsockopt(intf->q931_intf->master_socket,
				SOL_SOCKET, SO_DEBUG,
				&on, sizeof(on));
		} else {
			setsockopt(intf->q931_intf->dlc.socket,
				SOL_SOCKET, SO_DEBUG,
				&on, sizeof(on));
		}

		struct q931_dlc *dlc;
		list_for_each_entry(dlc, &intf->q931_intf->dlcs, intf_node) {
			setsockopt(dlc->socket,
				SOL_SOCKET, SO_DEBUG,
				&on, sizeof(on));
		}
	}

}

void refresh_polls_list()
{
	ast_mutex_lock(&visdn.lock);

	visdn.npolls = 0;

	visdn.polls[visdn.npolls].fd = visdn.q931_ccb_queue_pipe_read;
	visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
	visdn.poll_infos[visdn.npolls].type = POLL_INFO_TYPE_Q931_CCB;
	visdn.poll_infos[visdn.npolls].interface = NULL;
	(visdn.npolls)++;

	visdn.polls[visdn.npolls].fd = visdn.ccb_q931_queue_pipe_read;
	visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
	visdn.poll_infos[visdn.npolls].type = POLL_INFO_TYPE_CCB_Q931;
	visdn.poll_infos[visdn.npolls].interface = NULL;
	(visdn.npolls)++;

	visdn.polls[visdn.npolls].fd = visdn.netlink_socket;
	visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
	visdn.poll_infos[visdn.npolls].type = POLL_INFO_TYPE_NETLINK;
	visdn.poll_infos[visdn.npolls].interface = NULL;
	(visdn.npolls)++;

	visdn.open_pending = FALSE;

	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {
		if (intf->open_pending)
			visdn.open_pending = TRUE;
			visdn.open_pending_nextcheck = 0;

		if (!intf->q931_intf)
			continue;

		if (intf->q931_intf->role == LAPD_INTF_ROLE_NT) {
			visdn.polls[visdn.npolls].fd =
					intf->q931_intf->master_socket;
			visdn.polls[visdn.npolls].events =
					POLLIN | POLLERR;
			visdn.poll_infos[visdn.npolls].type =
					POLL_INFO_TYPE_INTERFACE;
			visdn.poll_infos[visdn.npolls].interface =
					intf->q931_intf;
			visdn.npolls++;
		} else {
			visdn.polls[visdn.npolls].fd =
					intf->q931_intf->dlc.socket;
			visdn.polls[visdn.npolls].events =
					POLLIN | POLLERR;
			visdn.poll_infos[visdn.npolls].type =
					POLL_INFO_TYPE_DLC;
			visdn.poll_infos[visdn.npolls].dlc =
					&intf->q931_intf->dlc;
			visdn.npolls++;
		}

		struct q931_dlc *dlc;
		list_for_each_entry(dlc, &intf->q931_intf->dlcs, intf_node) {
			visdn.polls[visdn.npolls].fd = dlc->socket;
			visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
			visdn.poll_infos[visdn.npolls].type =
							POLL_INFO_TYPE_DLC;
			visdn.poll_infos[visdn.npolls].dlc = dlc;
			(visdn.npolls)++;
		}
	}

	ast_mutex_unlock(&visdn.lock);
}

// Must be called with visdn.lock acquired
static void visdn_accept(
	struct q931_interface *intf,
	int accept_socket)
{
	struct q931_dlc *newdlc;

	newdlc = q931_accept(intf, accept_socket);
	if (!newdlc)
		return;

	visdn_debug("New DLC (TEI=%d) accepted on interface %s\n",
			newdlc->tei,
			intf->name);

	refresh_polls_list();
}

static void visdn_reload_config(void)
{
	struct ast_config *cfg;
	cfg = ast_config_load(VISDN_CONFIG_FILE);
	if (!cfg) {
		ast_log(LOG_WARNING,
			"Unable to load config %s, VISDN disabled\n",
			VISDN_CONFIG_FILE);

		return;
	}

	visdn_intf_reload(cfg);
	visdn_hg_reload(cfg);

	ast_config_destroy(cfg);
}

static enum q931_ie_called_party_number_type_of_number
	visdn_type_of_number_to_cdpn(enum visdn_type_of_number type_of_number)
{
	switch(type_of_number) {
	case VISDN_TYPE_OF_NUMBER_UNSET:
		assert(0);
	case VISDN_TYPE_OF_NUMBER_UNKNOWN:
		return Q931_IE_CDPN_TON_UNKNOWN;
	case VISDN_TYPE_OF_NUMBER_INTERNATIONAL:
		return Q931_IE_CDPN_TON_INTERNATIONAL;
	case VISDN_TYPE_OF_NUMBER_NATIONAL:
		return Q931_IE_CDPN_TON_NATIONAL;
	case VISDN_TYPE_OF_NUMBER_NETWORK_SPECIFIC:
		return Q931_IE_CDPN_TON_NETWORK_SPECIFIC;
	case VISDN_TYPE_OF_NUMBER_SUBSCRIBER:
		return Q931_IE_CDPN_TON_SUBSCRIBER;
	case VISDN_TYPE_OF_NUMBER_ABBREVIATED:
		return Q931_IE_CDPN_TON_ABBREVIATED;
	}

	assert(0);
}

static enum q931_ie_calling_party_number_type_of_number
	visdn_type_of_number_to_cgpn(enum visdn_type_of_number type_of_number)
{
	switch(type_of_number) {
	case VISDN_TYPE_OF_NUMBER_UNSET:
		assert(0);
	case VISDN_TYPE_OF_NUMBER_UNKNOWN:
		return Q931_IE_CGPN_TON_UNKNOWN;
	case VISDN_TYPE_OF_NUMBER_INTERNATIONAL:
		return Q931_IE_CDPN_TON_INTERNATIONAL;
	case VISDN_TYPE_OF_NUMBER_NATIONAL:
		return Q931_IE_CGPN_TON_NATIONAL;
	case VISDN_TYPE_OF_NUMBER_NETWORK_SPECIFIC:
		return Q931_IE_CGPN_TON_NETWORK_SPECIFIC;
	case VISDN_TYPE_OF_NUMBER_SUBSCRIBER:
		return Q931_IE_CGPN_TON_SUBSCRIBER;
	case VISDN_TYPE_OF_NUMBER_ABBREVIATED:
		return Q931_IE_CGPN_TON_ABBREVIATED;
	}

	assert(0);
	return 0;
}

void q931_send_primitive(
	struct q931_call *call,
	enum q931_primitive primitive,
	struct q931_ies *ies)
{
	struct q931_ccb_message *msg;
	msg = malloc(sizeof(*msg));
	if (!msg)
		return;

	msg->call = call;
	msg->primitive = primitive;

	q931_ies_init(&msg->ies);

	if (ies)
		q931_ies_copy(&msg->ies, ies);

	ast_mutex_lock(&visdn.ccb_q931_queue_lock);
	list_add_tail(&msg->node, &visdn.ccb_q931_queue);
	ast_mutex_unlock(&visdn.ccb_q931_queue_lock);

	if (write(visdn.ccb_q931_queue_pipe_write, " ", 1) < 0) {
		ast_log(LOG_WARNING,
			"Cannot write on ccb_q931_pipe_write\n");
	}
}

void visdn_queue_primitive(
	struct q931_call *call,
	enum q931_primitive primitive,
	const struct q931_ies *ies,
	unsigned long par1,
	unsigned long par2)
{
	struct q931_ccb_message *msg;
	msg = malloc(sizeof(*msg));
	if (!msg)
		return;

	msg->call = call;
	msg->primitive = primitive;
	msg->par1 = par1;
	msg->par2 = par2;

	q931_ies_init(&msg->ies);

	if (ies)
		q931_ies_copy(&msg->ies, ies);

	ast_mutex_lock(&visdn.q931_ccb_queue_lock);
	list_add_tail(&msg->node, &visdn.q931_ccb_queue);
	ast_mutex_unlock(&visdn.q931_ccb_queue_lock);

	if (write(visdn.q931_ccb_queue_pipe_write, " ", 1) < 0) {
		ast_log(LOG_WARNING,
			"Cannot write on q931_ccb_pipe_write\n");
	}
}

static void visdn_pres_to_pi_si(
	int pres,
	enum q931_ie_calling_party_number_presentation_indicator *pi,
	enum q931_ie_calling_party_number_screening_indicator *si)
{
	switch(pres) {
	case AST_PRES_ALLOWED_USER_NUMBER_NOT_SCREENED:
		*pi = Q931_IE_CGPN_PI_PRESENTATION_ALLOWED;
		*si = Q931_IE_CGPN_SI_USER_PROVIDED_NOT_SCREENED;
	break;

	case AST_PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN:
		*pi = Q931_IE_CGPN_PI_PRESENTATION_ALLOWED;
		*si = Q931_IE_CGPN_SI_USER_PROVIDED_VERIFIED_AND_PASSED;
	break;

	case AST_PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN:
		*pi = Q931_IE_CGPN_PI_PRESENTATION_ALLOWED;
		*si = Q931_IE_CGPN_SI_USER_PROVIDED_VERIFIED_AND_FAILED;
	break;

	case AST_PRES_ALLOWED_NETWORK_NUMBER:
		*pi = Q931_IE_CGPN_PI_PRESENTATION_ALLOWED;
		*si = Q931_IE_CGPN_SI_NETWORK_PROVIDED;
	break;

	case AST_PRES_PROHIB_USER_NUMBER_NOT_SCREENED:
		*pi = Q931_IE_CGPN_PI_PRESENTATION_RESTRICTED;
		*si = Q931_IE_CGPN_SI_USER_PROVIDED_NOT_SCREENED;
	break;

	case AST_PRES_PROHIB_USER_NUMBER_PASSED_SCREEN:
		*pi = Q931_IE_CGPN_PI_PRESENTATION_RESTRICTED;
		*si = Q931_IE_CGPN_SI_USER_PROVIDED_VERIFIED_AND_PASSED;
	break;

	case AST_PRES_PROHIB_USER_NUMBER_FAILED_SCREEN:
		*pi = Q931_IE_CGPN_PI_PRESENTATION_RESTRICTED;
		*si = Q931_IE_CGPN_SI_USER_PROVIDED_VERIFIED_AND_FAILED;
	break;

	case AST_PRES_PROHIB_NETWORK_NUMBER:
		*pi = Q931_IE_CGPN_PI_PRESENTATION_RESTRICTED;
		*si = Q931_IE_CGPN_SI_NETWORK_PROVIDED;
	break;

	case AST_PRES_NUMBER_NOT_AVAILABLE:
		*pi = Q931_IE_CGPN_PI_NOT_AVAILABLE;
		*si = Q931_IE_CGPN_SI_USER_PROVIDED_NOT_SCREENED;
	break;

	default:
		*pi = Q931_IE_CGPN_PI_PRESENTATION_ALLOWED;
		*si = Q931_IE_CGPN_SI_USER_PROVIDED_NOT_SCREENED;
	}
}

static enum q931_ie_calling_party_number_type_of_number
	visdn_ast_ton_to_cgpn(int ton)
{
	return ton; /* Ahhrgh */
}

static void visdn_defer_dtmf_in(
	struct visdn_chan *visdn_chan)
{

	ast_mutex_lock(&visdn_chan->ast_chan->lock);
	visdn_chan->dtmf_deferred = TRUE;
	ast_mutex_unlock(&visdn_chan->ast_chan->lock);
}

static void visdn_undefer_dtmf_in(
	struct visdn_chan *visdn_chan)
{
	struct q931_call *q931_call = visdn_chan->q931_call;
	struct visdn_interface *intf = q931_call->intf->pvt;

	ast_mutex_lock(&visdn_chan->ast_chan->lock);
	visdn_chan->dtmf_deferred = FALSE;

	/* Flush queue */
	if (strlen(visdn_chan->dtmf_queue)) {
		Q931_DECLARE_IES(ies);

		struct q931_ie_called_party_number *cdpn =
			q931_ie_called_party_number_alloc();
		cdpn->type_of_number = visdn_type_of_number_to_cdpn(
						intf->outbound_called_ton);
		cdpn->numbering_plan_identificator =
			Q931_IE_CDPN_NPI_ISDN_TELEPHONY;

		strncpy(cdpn->number, visdn_chan->dtmf_queue,
			sizeof(cdpn->number));
		q931_ies_add_put(&ies, &cdpn->ie);

		q931_send_primitive(visdn_chan->q931_call,
			Q931_CCB_INFO_REQUEST, &ies);

		visdn_chan->dtmf_queue[0] = '\0';

		Q931_UNDECLARE_IES(ies);
	}

	ast_mutex_unlock(&visdn_chan->ast_chan->lock);
}

static void visdn_queue_dtmf(
	struct visdn_chan *visdn_chan,
	char digit)
{
	ast_mutex_lock(&visdn_chan->ast_chan->lock);

	int len = strlen(visdn_chan->dtmf_queue);

	if (len >= sizeof(visdn_chan->dtmf_queue) - 1) {
		ast_log(LOG_WARNING, "DTMF queue is full, dropping digit\n");
		ast_mutex_unlock(&visdn_chan->ast_chan->lock);
		return;
	}

	visdn_chan->dtmf_queue[len] = digit;
	visdn_chan->dtmf_queue[len + 1] = '\0';

	ast_mutex_unlock(&visdn_chan->ast_chan->lock);
}

static int visdn_request_call(
	struct ast_channel *ast_chan,
	struct visdn_interface *intf,
	const char *number,
	const char *options)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	int err;

	visdn_debug("Calling on interface '%s'\n", intf->name);

	visdn_chan->intf = visdn_intf_get(intf);

	Q931_DECLARE_IES(ies);

	/* ------------- Bearer Capability ---------------- */
	char *raw_bc = pbx_builtin_getvar_helper(ast_chan, "BEARERCAP_RAW");
	if (raw_bc) {
		visdn_debug("Taking bearer capability from bridged channel\n");

		struct q931_ie_bearer_capability *bc;
		bc = q931_ie_bearer_capability_alloc();
		char buf[20];
		int len = strlen(raw_bc);

		if (len > sizeof(buf) * 2) {
			ast_log(LOG_WARNING, "BEARERCAP_RAW is too long\n");
			goto bc_failure;
		}

		if (!q931_ie_bearer_capability_read_from_buf(&bc->ie,
					buf, len, NULL, NULL)) {
			ast_log(LOG_WARNING, "BEARERCAP_RAW is not valid\n");
			goto bc_failure;
		}

		q931_ies_add_put(&ies, &bc->ie);

	} else {
bc_failure:;
		struct q931_ie_bearer_capability *bc;
		bc = q931_ie_bearer_capability_alloc();

		bc->coding_standard = Q931_IE_BC_CS_CCITT;
		bc->information_transfer_capability = Q931_IE_BC_ITC_SPEECH;
		bc->transfer_mode = Q931_IE_BC_TM_CIRCUIT;
		bc->information_transfer_rate = Q931_IE_BC_ITR_64;
		bc->user_information_layer_1_protocol =
			Q931_IE_BC_UIL1P_G711_ALAW;
		bc->user_information_layer_2_protocol = Q931_IE_BC_UIL2P_UNUSED;
		bc->user_information_layer_3_protocol = Q931_IE_BC_UIL3P_UNUSED;

		visdn_chan->is_voice = TRUE;

		if (options) {
			if (strchr(options, 'D')) {
				bc->information_transfer_capability =
					Q931_IE_BC_ITC_UNRESTRICTED_DIGITAL;
				bc->user_information_layer_1_protocol =
					Q931_IE_BC_UIL1P_UNUSED;

				visdn_chan->is_voice = FALSE;
			}
		}

		q931_ies_add_put(&ies, &bc->ie);
	}

	/* ------------- END Bearer Capability ---------------- */

	struct q931_call *q931_call;
	q931_call = q931_call_alloc_out(intf->q931_intf);
	if (!q931_call) {
		ast_log(LOG_WARNING, "Cannot allocate outbound call\n");
		err = -1;
		goto err_call_alloc;
	}

	q931_call->pvt = ast_chan;
	visdn_chan->q931_call = q931_call_get(q931_call);

	char newname[40];
	snprintf(newname, sizeof(newname), "VISDN/%s/%d.%c",
		q931_call->intf->name,
		q931_call->call_reference,
		q931_call->direction ==
			Q931_CALL_DIRECTION_INBOUND ? 'I' : 'O');

	ast_change_name(ast_chan, newname);

	ast_setstate(ast_chan, AST_STATE_DIALING);


	struct q931_ie_called_party_number *cdpn =
		q931_ie_called_party_number_alloc();
	cdpn->type_of_number =
		visdn_type_of_number_to_cdpn(intf->outbound_called_ton);
	cdpn->numbering_plan_identificator = Q931_IE_CDPN_NPI_ISDN_TELEPHONY;
	snprintf(cdpn->number, sizeof(cdpn->number), "%s", number);
	q931_ies_add_put(&ies, &cdpn->ie);

	if (intf->q931_intf->role == LAPD_INTF_ROLE_NT &&
	    !intf->overlap_receiving) {
		struct q931_ie_sending_complete *sc =
			q931_ie_sending_complete_alloc();

		q931_ies_add_put(&ies, &sc->ie);
	}

	if (intf->clip_enabled) {
		struct q931_ie_calling_party_number *cgpn =
			q931_ie_calling_party_number_alloc();

		if (ast_chan->cid.cid_num &&
		    strlen(ast_chan->cid.cid_num)) {

			if ((ast_chan->cid.cid_pres & AST_PRES_RESTRICTION) ==
					AST_PRES_ALLOWED ||
			    intf->clip_override) {

				/* Send subaddress if provided */

				visdn_pres_to_pi_si(ast_chan->cid.cid_pres,
					&cgpn->presentation_indicator,
					&cgpn->screening_indicator);

				if (intf->force_outbound_cli_ton !=
						VISDN_TYPE_OF_NUMBER_UNSET) {
					cgpn->type_of_number =
						visdn_type_of_number_to_cgpn(
						intf->force_outbound_cli_ton);
				} else {
					cgpn->type_of_number =
						visdn_ast_ton_to_cgpn(
							ast_chan->cid.cid_ton);
				}

				cgpn->numbering_plan_identificator =
					Q931_IE_CGPN_NPI_ISDN_TELEPHONY;

				if (strlen(intf->force_outbound_cli))
					strncpy(cgpn->number,
					intf->force_outbound_cli,
						sizeof(cgpn->number));
				else {
					strncpy(cgpn->number,
						ast_chan->cid.cid_num,
						sizeof(cgpn->number));
				}
			} else {
				cgpn->type_of_number =
					Q931_IE_CGPN_TON_UNKNOWN;
				cgpn->numbering_plan_identificator =
					Q931_IE_CGPN_NPI_UNKNOWN;
				cgpn->presentation_indicator =
					Q931_IE_CGPN_PI_PRESENTATION_RESTRICTED;
				cgpn->screening_indicator =
					Q931_IE_CGPN_SI_NETWORK_PROVIDED;
			}
		} else {
			cgpn->type_of_number =
				Q931_IE_CGPN_TON_UNKNOWN;
			cgpn->numbering_plan_identificator =
				Q931_IE_CGPN_NPI_UNKNOWN;
			cgpn->presentation_indicator =
				Q931_IE_CGPN_PI_NOT_AVAILABLE;
			cgpn->screening_indicator =
				Q931_IE_CGPN_SI_NETWORK_PROVIDED;
		}

		q931_ies_add_put(&ies, &cgpn->ie);

		/* NOTE: There is no provision for sending a second CGPN
		 * if the caller is using the special arrangements, since
		 * Asterisk does not support more than one CID
		 */
	}

	struct q931_ie_high_layer_compatibility *hlc =
		q931_ie_high_layer_compatibility_alloc();

	hlc->coding_standard = Q931_IE_HLC_CS_CCITT;
	hlc->interpretation = Q931_IE_HLC_P_FIRST;
	hlc->presentation_method = Q931_IE_HLC_PM_HIGH_LAYER_PROTOCOL_PROFILE;
	hlc->characteristics_identification = Q931_IE_HLC_CI_TELEPHONY;

	visdn_defer_dtmf_in(visdn_chan);

	q931_send_primitive(q931_call, Q931_CCB_SETUP_REQUEST, &ies);

	q931_call_put(q931_call);

	Q931_UNDECLARE_IES(ies);

	return 0;

	visdn_intf_put(visdn_chan->intf);

	q931_call_release_reference(q931_call);
	q931_call_put(q931_call);
err_call_alloc:

	Q931_UNDECLARE_IES(ies);

	return err;
}

static int visdn_call(
	struct ast_channel *ast_chan,
	char *orig_dest,
	int timeout)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	int err;

	if ((ast_chan->_state != AST_STATE_DOWN) &&
	    (ast_chan->_state != AST_STATE_RESERVED)) {
		ast_log(LOG_WARNING,
			"visdn_call called on %s,"
			" neither down nor reserved\n",
			ast_chan->name);

		err = -1;
		goto err_channel_not_down;
	}

	// Parse destination and obtain interface name + number
	char dest[64];
	char *dest_pos = dest;
	const char *token;

	strncpy(dest, orig_dest, sizeof(dest));

	const char *intf_name = strsep(&dest_pos, "/");
	if (!intf_name) {
		ast_log(LOG_WARNING,
			"Invalid destination '%s' format (interface/number)\n",
			dest);

		err = -1;
		goto err_invalid_destination;
	}

	token = strsep(&dest_pos, "/");
	if (!token) {
		ast_log(LOG_WARNING,
			"Invalid destination '%s' format (interface/number)\n",
			dest);

		err = -1;
		goto err_invalid_format;
	}

	strncpy(visdn_chan->number, token, sizeof(visdn_chan->number));

	token = strsep(&dest_pos, "/");
	if (token)
		strncpy(visdn_chan->options, token,
			sizeof(visdn_chan->options));

	visdn_debug("Calling %s on %s\n",
			dest, ast_chan->name);

	struct visdn_interface *intf = NULL;
	if (!strncasecmp(intf_name, VISDN_HUNTGROUP_PREFIX,
			strlen(VISDN_HUNTGROUP_PREFIX))) {

		ast_mutex_lock(&visdn.lock);

		const char *hg_name = intf_name +
					strlen(VISDN_HUNTGROUP_PREFIX);
		struct visdn_huntgroup *hg;
		hg = visdn_hg_get_by_name(hg_name);
		if (!hg) {
			ast_log(LOG_ERROR, "Cannot find huntgroup '%s'\n",
				hg_name);

			ast_chan->hangupcause = AST_CAUSE_BUSY;

			err = -1;
			ast_mutex_unlock(&visdn.lock);
			goto err_huntgroup_not_found;
		}

		intf = visdn_hg_hunt(hg, NULL, NULL);
		if (!intf) {
			visdn_debug("Cannot hunt in huntgroup %s\n", hg_name);

			err = -1;
			ast_mutex_unlock(&visdn.lock);
			goto err_no_channel_available;
		}

		visdn_chan->hg_first_intf = visdn_intf_get(intf);
		visdn_chan->huntgroup = visdn_hg_get(hg);

		ast_mutex_unlock(&visdn.lock);
	} else {
		intf = visdn_intf_get_by_name(intf_name);

		if (!intf) {
			ast_log(LOG_WARNING,
				"Interface %s not found\n",
				intf_name);
			err = -1;
			goto err_intf_not_found;
		}

		if (!intf->q931_intf) {
			ast_log(LOG_WARNING,
				"Interface %s not active\n",
				intf_name);
			err = -1;
			goto err_intf_not_found;
		}

		visdn_chan->hg_first_intf = NULL;
		visdn_chan->huntgroup = NULL;
	}

	visdn_request_call(ast_chan, intf,
		visdn_chan->number,
		visdn_chan->options);

	visdn_intf_put(intf);
	intf = NULL;

	return 0;

err_intf_not_found:
err_no_channel_available:
err_huntgroup_not_found:
err_invalid_format:
err_invalid_destination:
err_channel_not_down:

	return err;
}

static int visdn_answer(struct ast_channel *ast_chan)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	FUNC_DEBUG();

	ast_indicate(ast_chan, -1);

	if (!visdn_chan) {
		ast_log(LOG_ERROR, "NO VISDN_CHAN!!\n");
		return -1;
	}

	if (visdn_chan->q931_call->state == U6_CALL_PRESENT ||
	    visdn_chan->q931_call->state == U7_CALL_RECEIVED ||
	    visdn_chan->q931_call->state == U9_INCOMING_CALL_PROCEEDING ||
	    visdn_chan->q931_call->state == U25_OVERLAP_RECEIVING ||
	    visdn_chan->q931_call->state == N2_OVERLAP_SENDING ||
	    visdn_chan->q931_call->state == N3_OUTGOING_CALL_PROCEEDING ||
	    visdn_chan->q931_call->state == N4_CALL_DELIVERED) {
		q931_send_primitive(visdn_chan->q931_call,
			Q931_CCB_SETUP_RESPONSE, NULL);
	}

	return 0;
}

static int visdn_bridge(
	struct ast_channel *c0,
	struct ast_channel *c1,
	int flags, struct ast_frame **fo,
	struct ast_channel **rc,
	int timeoutms)
{
	return AST_BRIDGE_FAILED_NOWARN;

#if 0
	/* if need DTMF, cant native bridge (at least not yet...) */
	if (flags & (AST_BRIDGE_DTMF_CHANNEL_0 | AST_BRIDGE_DTMF_CHANNEL_1))
		return AST_BRIDGE_FAILED;

	struct visdn_chan *visdn_chan1 = to_visdn_chan(c0);
	struct visdn_chan *visdn_chan2 = to_visdn_chan(c1);

	char path[100], dest1[100], dest2[100];

	snprintf(path, sizeof(path),
		"/sys/class/net/%s/visdn_channel/connected/../B%d",
		visdn_chan1->q931_call->intf->name,
		visdn_chan1->q931_call->channel->id+1);

	memset(dest1, 0, sizeof(dest1));
	if (readlink(path, dest1, sizeof(dest1) - 1) < 0) {
		ast_log(LOG_ERROR, "readlink(%s): %s\n", path, strerror(errno));
		return AST_BRIDGE_FAILED;
	}

	char *chanid1 = strrchr(dest1, '/');
	if (!chanid1 || !strlen(chanid1 + 1)) {
		ast_log(LOG_ERROR,
			"Invalid chanid found in symlink %s\n",
			dest1);
		return AST_BRIDGE_FAILED;
	}

	chanid1++;

	snprintf(path, sizeof(path),
		"/sys/class/net/%s/visdn_channel/connected/../B%d",
		visdn_chan2->q931_call->intf->name,
		visdn_chan2->q931_call->channel->id+1);

	memset(dest2, 0, sizeof(dest2));
	if (readlink(path, dest2, sizeof(dest2) - 1) < 0) {
		ast_log(LOG_ERROR, "readlink(%s): %s\n", path, strerror(errno));
		return AST_BRIDGE_FAILED;
	}

	char *chanid2 = strrchr(dest2, '/');
	if (!chanid2 || !strlen(chanid2 + 1)) {
		ast_log(LOG_ERROR,
			"Invalid chanid found in symlink %s\n",
			dest2);
		return AST_BRIDGE_FAILED;
	}

	chanid2++;

	visdn_debug("Connecting chan %s to chan %s\n", chanid1, chanid2);

	int fd = open("/sys/visdn_tdm/internal-cxc/connect", O_WRONLY);
	if (fd < 0) {
		ast_log(LOG_ERROR,
			"Cannot open /sys/visdn_tdm/internal-cxc/connect: %s\n",
			strerror(errno));
		return AST_BRIDGE_FAILED;
	}

	if (ioctl(visdn_chan1->sp_fd,
			VISDN_IOC_DISCONNECT, NULL) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_IOC_DISCONNECT): %s\n",
			strerror(errno));
	}

	close(visdn_chan1->sp_fd);
	visdn_chan1->sp_fd = -1;

	if (ioctl(visdn_chan2->sp_fd,
			VISDN_IOC_DISCONNECT, NULL) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_IOC_DISCONNECT): %s\n",
			strerror(errno));
	}

	close(visdn_chan2->sp_fd);
	visdn_chan2->sp_fd = -1;

	char command[256];
	snprintf(command, sizeof(command),
		"%s\n%s\n",
		chanid1,
		chanid2);

	if (write(fd, command, strlen(command)) < 0) {
		ast_log(LOG_ERROR,
			"Cannot write to /sys/visdn_tdm/internal-cxc/connect: %s\n",
			strerror(errno));

		close(fd);

		return AST_BRIDGE_FAILED;
	}

	close(fd);

	struct ast_channel *cs[2];
	cs[0] = c0;
	cs[1] = c1;

	struct ast_channel *who = NULL;
	for (;;) {
		int to = -1;
		who = ast_waitfor_n(cs, 2, &to);
		if (!who) {
			ast_log(LOG_DEBUG, "Ooh, empty read...\n");
			continue;
		}

		struct ast_frame *f;
		f = ast_read(who);
		if (!f)
			break;

		if (f->frametype == AST_FRAME_DTMF) {
			if (((who == c0) && (flags & AST_BRIDGE_DTMF_CHANNEL_0)) ||
			    ((who == c1) && (flags & AST_BRIDGE_DTMF_CHANNEL_1))) {

				*fo = f;
				*rc = who;

				// Disconnect channels
				return AST_BRIDGE_COMPLETE;
			}

			if (who == c0)
				ast_write(c1, f);
			else
				ast_write(c0, f);
		}

		ast_frfree(f);

		// Braindead anyone?
		struct ast_channel *t;
		t = cs[0];
		cs[0] = cs[1];
		cs[1] = t;
	}

	// Really braindead
	*fo = NULL;
	*rc = who;

#endif

	return AST_BRIDGE_COMPLETE;
}

struct ast_frame *visdn_exception(struct ast_channel *ast_chan)
{
	ast_log(LOG_WARNING, "visdn_exception\n");

	return NULL;
}

/* We are called with chan->lock'ed */
static int visdn_indicate(struct ast_channel *ast_chan, int condition)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	const struct tone_zone_sound *tone = NULL;
	int res = 0;

	FUNC_DEBUG("%d", condition);

	switch(condition) {
	case AST_CONTROL_RING:
	case AST_CONTROL_TAKEOFFHOOK:
	case AST_CONTROL_FLASH:
	case AST_CONTROL_WINK:
	case AST_CONTROL_OPTION:
	case AST_CONTROL_RADIO_KEY:
	case AST_CONTROL_RADIO_UNKEY:
		res = 1;
	break;

	case -1:
		ast_playtones_stop(ast_chan);
	break;

	case AST_CONTROL_INBAND_INFO:
		visdn_chan->inband_info = TRUE;
	break;

	case AST_CONTROL_DISCONNECT: {
		Q931_DECLARE_IES(ies);

		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_call(
							visdn_chan->q931_call);

		if (ast_chan->hangupcause)
			cause->value = ast_chan->hangupcause;
		else
			cause->value = Q931_IE_C_CV_USER_BUSY;

		q931_ies_add_put(&ies, &cause->ie);

		q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_DISCONNECT_REQUEST, &ies);

		if (!visdn_chan->inband_info)
			tone = ast_get_indication_tone(ast_chan->zone, "busy");

		Q931_UNDECLARE_IES(ies);
	break;
	}

	case AST_CONTROL_OFFHOOK: {
		if (!visdn_chan->inband_info)
			tone = ast_get_indication_tone(ast_chan->zone, "dial");
	}
	break;

	case AST_CONTROL_HANGUP: {
		if (!visdn_chan->inband_info)
			tone = ast_get_indication_tone(ast_chan->zone,
								"congestion");
	}
	break;

	case AST_CONTROL_RINGING: {
		Q931_DECLARE_IES(ies);

		struct q931_ie_progress_indicator *pi = NULL;

		if (ast_bridged_channel(ast_chan) &&
		   strcmp(ast_bridged_channel(ast_chan)->type,
			  				VISDN_CHAN_TYPE)) {

			visdn_debug("Channel is not VISDN, sending"
					" progress indicator\n");

			pi = q931_ie_progress_indicator_alloc();
			pi->coding_standard = Q931_IE_PI_CS_CCITT;
			pi->location = q931_ie_progress_indicator_location(
						visdn_chan->q931_call);
			pi->progress_description =
				Q931_IE_PI_PD_CALL_NOT_END_TO_END;
			q931_ies_add_put(&ies, &pi->ie);
		}

		pi = q931_ie_progress_indicator_alloc();
		pi->coding_standard = Q931_IE_PI_CS_CCITT;
		pi->location = q931_ie_progress_indicator_location(
					visdn_chan->q931_call);
		pi->progress_description = Q931_IE_PI_PD_IN_BAND_INFORMATION;
		q931_ies_add_put(&ies, &pi->ie);

		switch(visdn_chan->q931_call->state) {
		case N1_CALL_INITIATED:
			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_PROCEEDING_REQUEST, &ies);
		break;

		case N2_OVERLAP_SENDING:
		case N3_OUTGOING_CALL_PROCEEDING:
		case U6_CALL_PRESENT:
		case U9_INCOMING_CALL_PROCEEDING:
		case U25_OVERLAP_RECEIVING:
			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_ALERTING_REQUEST, &ies);
		break;

		default:
		break;
		}

		if (!visdn_chan->inband_info)
			tone = ast_get_indication_tone(ast_chan->zone, "ring");

		Q931_UNDECLARE_IES(ies);
	}
	break;

	case AST_CONTROL_ANSWER:
		ast_playtones_stop(ast_chan);
	break;

	case AST_CONTROL_BUSY: {
		Q931_DECLARE_IES(ies);

		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_call(
							visdn_chan->q931_call);

		if (ast_chan->hangupcause)
			cause->value = ast_chan->hangupcause;
		else
			cause->value = Q931_IE_C_CV_USER_BUSY;

		q931_ies_add_put(&ies, &cause->ie);

		q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_DISCONNECT_REQUEST, &ies);

		if (!visdn_chan->inband_info)
			tone = ast_get_indication_tone(ast_chan->zone, "busy");

		Q931_UNDECLARE_IES(ies);
	}
	break;

	case AST_CONTROL_CONGESTION: {

		Q931_DECLARE_IES(ies);
		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_call(
							visdn_chan->q931_call);

		if (ast_chan->hangupcause)
			cause->value = ast_chan->hangupcause;
		else
			cause->value =
				Q931_IE_C_CV_SWITCHING_EQUIPMENT_CONGESTION;

		q931_ies_add_put(&ies, &cause->ie);

		q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_DISCONNECT_REQUEST, &ies);

		if (!visdn_chan->inband_info)
			tone = ast_get_indication_tone(ast_chan->zone, "busy");

		Q931_UNDECLARE_IES(ies);
	}
	break;

	case AST_CONTROL_PROGRESS: {
		Q931_DECLARE_IES(ies);

		struct q931_ie_progress_indicator *pi =
			q931_ie_progress_indicator_alloc();
		pi->coding_standard = Q931_IE_PI_CS_CCITT;
		pi->location = q931_ie_progress_indicator_location(
					visdn_chan->q931_call);

		if (ast_bridged_channel(ast_chan) &&
		   strcmp(ast_bridged_channel(ast_chan)->type,
			   				VISDN_CHAN_TYPE)) {
			pi->progress_description =
				Q931_IE_PI_PD_CALL_NOT_END_TO_END; // FIXME
		} else if (visdn_chan->is_voice) {
			pi->progress_description =
				Q931_IE_PI_PD_IN_BAND_INFORMATION;
		}

		q931_ies_add_put(&ies, &pi->ie);

		q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_PROGRESS_REQUEST, &ies);

		Q931_UNDECLARE_IES(ies);
	}
	break;

	case AST_CONTROL_PROCEEDING:
		if (visdn_chan->q931_call->state == N1_CALL_INITIATED ||
		    visdn_chan->q931_call->state == N2_OVERLAP_SENDING ||
		    visdn_chan->q931_call->state == U6_CALL_PRESENT ||
		    visdn_chan->q931_call->state == U25_OVERLAP_RECEIVING)
			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_PROCEEDING_REQUEST, NULL);
	break;
	}

	if (tone)
		ast_playtones_start(ast_chan, 0, tone->data, 1);

	return res;
}

static int visdn_fixup(
	struct ast_channel *oldchan,
	struct ast_channel *newchan)
{
	struct visdn_chan *chan = to_visdn_chan(newchan);

	if (chan->ast_chan != oldchan) {
		ast_log(LOG_WARNING, "old channel wasn't %p but was %p\n",
				oldchan, chan->ast_chan);
		return -1;
	}

	chan->ast_chan = newchan;

	return 0;
}

static int visdn_setoption(
	struct ast_channel *ast_chan,
	int option,
	void *data,
	int datalen)
{
	ast_log(LOG_ERROR, "%s\n", __FUNCTION__);

	return -1;
}

static int visdn_transfer(
	struct ast_channel *ast,
	const char *dest)
{
	ast_log(LOG_ERROR, "%s\n", __FUNCTION__);

	return -1;
}

static int visdn_send_digit(struct ast_channel *ast_chan, char digit)
{
	FUNC_DEBUG("%c", digit);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	struct q931_call *q931_call = visdn_chan->q931_call;
	struct visdn_interface *intf = q931_call->intf->pvt;

	if (visdn_chan->dtmf_deferred) {
		visdn_queue_dtmf(visdn_chan, digit);
		return 0;
	}

	Q931_DECLARE_IES(ies);

	struct q931_ie_called_party_number *cdpn =
		q931_ie_called_party_number_alloc();
	cdpn->type_of_number = visdn_type_of_number_to_cdpn(
					intf->outbound_called_ton);
	cdpn->numbering_plan_identificator =
		Q931_IE_CDPN_NPI_ISDN_TELEPHONY;

	cdpn->number[0] = digit;
	cdpn->number[1] = '\0';
	q931_ies_add_put(&ies, &cdpn->ie);

	q931_send_primitive(visdn_chan->q931_call,
		Q931_CCB_INFO_REQUEST, &ies);

	Q931_UNDECLARE_IES(ies);

	/* IMPORTANT: Since Asterisk is a bug made software, if there
	 * are DTMF frames queued and we start generating DTMF tones
	 * the queued frames are discarded and we fail completing
	 * overlap dialing.
	 */

	if (q931_call->state != U1_CALL_INITIATED &&
	    q931_call->state != U2_OVERLAP_SENDING &&
	    q931_call->state != U6_CALL_PRESENT &&
	    q931_call->state != U25_OVERLAP_RECEIVING &&
	    q931_call->state != N1_CALL_INITIATED &&
	    q931_call->state != N2_OVERLAP_SENDING &&
	    q931_call->state != N6_CALL_PRESENT &&
	    q931_call->state != N25_OVERLAP_RECEIVING)
		return 1;
	else
		return 0;
}

static int visdn_sendtext(struct ast_channel *ast, const char *text)
{
	ast_log(LOG_WARNING, "%s\n", __FUNCTION__);

	return -1;
}

static void visdn_destroy(struct visdn_chan *visdn_chan)
{
	free(visdn_chan);
}

static struct visdn_chan *visdn_alloc()
{
	struct visdn_chan *visdn_chan;

	visdn_chan = malloc(sizeof(*visdn_chan));
	if (!visdn_chan)
		return NULL;

	memset(visdn_chan, 0, sizeof(*visdn_chan));

	visdn_chan->sp_fd = -1;
	visdn_chan->ec_fd = -1;

	return visdn_chan;
}

static void visdn_disconnect_chan_from_visdn(
	struct visdn_chan *visdn_chan)
{
	if (visdn_chan->sp_fd < 0)
		return;

	struct visdn_connect vc;
	vc.src_chan_id = visdn_chan->sp_channel_id;
	vc.dst_chan_id = 0;
	vc.flags = 0;

	if (ioctl(visdn.cxc_control_fd,
			VISDN_IOC_DISCONNECT_PATH,
			(caddr_t)&vc) < 0) {

		ast_log(LOG_ERROR,
			"ioctl(VISDN_IOC_DISCONNECT_PATH):"
			" %s\n",
			strerror(errno));
	}

	vc.src_chan_id = visdn_chan->isdn_channel_id;
	vc.dst_chan_id = 0;
	vc.flags = 0;

	if (ioctl(visdn.cxc_control_fd,
			VISDN_IOC_DISCONNECT_PATH,
			(caddr_t)&vc) < 0) {

		ast_log(LOG_ERROR,
			"ioctl(VISDN_IOC_DISCONNECT_PATH):"
			" %s\n",
			strerror(errno));
	}

	if (close(visdn_chan->sp_fd) < 0) {
		ast_log(LOG_ERROR,
			"close(visdn_chan->sp_fd): %s\n",
			strerror(errno));
	}

	visdn_chan->sp_fd = -1;

	if (visdn_chan->ec_fd >= 0) {
		if (close(visdn_chan->ec_fd) < 0) {
			ast_log(LOG_ERROR,
				"close(visdn_chan->ec_fd): %s\n",
				strerror(errno));
		}

		visdn_chan->ec_fd = -1;
	}
}

static int visdn_hangup(struct ast_channel *ast_chan)
{
	FUNC_DEBUG("%s", ast_chan->name);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	ast_setstate(ast_chan, AST_STATE_DOWN);

	ast_mutex_lock(&visdn.lock);
	if (visdn_chan->q931_call &&
	    visdn_chan->q931_call->intf) {

		struct q931_call *q931_call = visdn_chan->q931_call;

		Q931_DECLARE_IES(ies);

		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_call(q931_call);

		if (ast_chan->hangupcause)
			cause->value = ast_chan->hangupcause;
		else
			cause->value =
				Q931_IE_C_CV_NORMAL_CALL_CLEARING;

		q931_ies_add_put(&ies, &cause->ie);

		switch(q931_call->state) {
		case N0_NULL_STATE:
		case U0_NULL_STATE:
		break;

		case U1_CALL_INITIATED:
		case U2_OVERLAP_SENDING:
		case U3_OUTGOING_CALL_PROCEEDING:
		case U4_CALL_DELIVERED:
		case U7_CALL_RECEIVED:
		case U8_CONNECT_REQUEST:
		case U9_INCOMING_CALL_PROCEEDING:
		case U10_ACTIVE:
		case U25_OVERLAP_RECEIVING:
		case N2_OVERLAP_SENDING:
		case N3_OUTGOING_CALL_PROCEEDING:
		case N4_CALL_DELIVERED:
		case N6_CALL_PRESENT:
		case N7_CALL_RECEIVED:
		case N8_CONNECT_REQUEST:
		case N9_INCOMING_CALL_PROCEEDING:
		case N10_ACTIVE:
		case N15_SUSPEND_REQUEST:
		case N25_OVERLAP_RECEIVING:
			q931_send_primitive(q931_call,
				Q931_CCB_DISCONNECT_REQUEST, &ies);
		break;

		case U15_SUSPEND_REQUEST:
			/* Suspend reject and disconnect request ??? */
		break;

		case U6_CALL_PRESENT:
		case U17_RESUME_REQUEST:
		case N1_CALL_INITIATED:
		case N17_RESUME_REQUEST:
			/* No ast_chan has been created yet */
		break;

		case U11_DISCONNECT_REQUEST:
		case N12_DISCONNECT_INDICATION:
		case U19_RELEASE_REQUEST:
		case N19_RELEASE_REQUEST:
		case N22_CALL_ABORT:
			/* Do nothing, already releasing */
		break;

		case U12_DISCONNECT_INDICATION:
		case N11_DISCONNECT_REQUEST:
			q931_send_primitive(q931_call,
				Q931_CCB_RELEASE_REQUEST, &ies);
		break;
		}

		Q931_UNDECLARE_IES(ies);
	}

	if (visdn_chan->q931_call) {
		visdn_chan->q931_call->pvt = NULL;
		q931_call_put(visdn_chan->q931_call);
		visdn_chan->q931_call = NULL;
	}
	ast_mutex_unlock(&visdn.lock);

	ast_mutex_lock(&ast_chan->lock);

	if (visdn_chan->suspended_call) {
		// We are responsible for the channel
		q931_channel_release(visdn_chan->suspended_call->q931_chan);

		list_del(&visdn_chan->suspended_call->node);
		free(visdn_chan->suspended_call);
		visdn_chan->suspended_call = NULL;
	}

	close(ast_chan->fds[0]);

	if (visdn_chan->hg_first_intf) {
		visdn_intf_put(visdn_chan->hg_first_intf);
		visdn_chan->hg_first_intf = NULL;
	}

	if (visdn_chan->intf) {
		visdn_intf_put(visdn_chan->intf);
		visdn_chan->intf = NULL;
	}

	if (visdn_chan->huntgroup) {
		visdn_hg_put(visdn_chan->huntgroup);
		visdn_chan->huntgroup = NULL;
	}

	if (visdn_chan->dsp) {
		ast_dsp_free(visdn_chan->dsp);
		visdn_chan->dsp = NULL;
	}
	
	if (visdn_chan->sp_fd >= 0) {
		// Disconnect the softport since we cannot rely on
		// libq931 (see above)

		visdn_disconnect_chan_from_visdn(visdn_chan);
	}

	visdn_destroy(visdn_chan);
	ast_chan->tech_pvt = NULL;

	ast_mutex_unlock(&ast_chan->lock);

	FUNC_DEBUG("%s DONE", ast_chan->name);

	return 0;
}

static struct ast_frame *visdn_read(struct ast_channel *ast_chan)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	static struct ast_frame f;
	char buf[512];

	/* Acknowledge timer */
	read(ast_chan->fds[0], buf, 1);

	f.src = VISDN_CHAN_TYPE;
	f.mallocd = 0;
	f.delivery.tv_sec = 0;
	f.delivery.tv_usec = 0;

	if (visdn_chan->sp_fd < 0) {
		f.frametype = AST_FRAME_NULL;
		f.subclass = 0;
		f.samples = 0;
		f.datalen = 0;
		f.data = NULL;
		f.offset = 0;

		return &f;
	}

	int nread = read(visdn_chan->sp_fd, buf, sizeof(buf));
	if (nread < 0) {
		ast_log(LOG_WARNING, "read error: %s\n", strerror(errno));
		return &f;
	}

#if 0
struct timeval tv;
gettimeofday(&tv, NULL);
unsigned long long t = tv.tv_sec * 1000000ULL + tv.tv_usec;
ast_verbose(VERBOSE_PREFIX_3 "R %.3f %d\n",
	t/1000000.0,
	visdn_chan->sp_fd);
#endif
	f.frametype = AST_FRAME_VOICE;
	f.subclass = AST_FORMAT_ALAW;
	f.samples = nread;
	f.datalen = nread;
	f.data = buf;
	f.offset = 0;

	struct ast_frame *f2 = ast_dsp_process(ast_chan, visdn_chan->dsp, &f);

	return f2;
}

static int visdn_write(
	struct ast_channel *ast_chan,
	struct ast_frame *frame)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	if (frame->frametype != AST_FRAME_VOICE) {
		ast_log(LOG_WARNING,
			"Don't know what to do with frame type '%d'\n",
			frame->frametype);

		return 0;
	}

	if (frame->subclass != AST_FORMAT_ALAW) {
		ast_log(LOG_WARNING,
			"Cannot handle frames in %d format\n",
			frame->subclass);
		return 0;
	}

	if (visdn_chan->sp_fd < 0) {
//		ast_log(LOG_WARNING,
//			"Attempting to write on unconnected channel\n");
		return 0;
	}

#if 0
ast_verbose(VERBOSE_PREFIX_3 "W %d %02x%02x%02x%02x%02x%02x%02x%02x %d\n", visdn_chan->sp_fd,
	*(__u8 *)(frame->data + 0),
	*(__u8 *)(frame->data + 1),
	*(__u8 *)(frame->data + 2),
	*(__u8 *)(frame->data + 3),
	*(__u8 *)(frame->data + 4),
	*(__u8 *)(frame->data + 5),
	*(__u8 *)(frame->data + 6),
	*(__u8 *)(frame->data + 7),
	frame->datalen);
#endif

	write(visdn_chan->sp_fd, frame->data, frame->datalen);

	return 0;
}

static const struct ast_channel_tech visdn_tech;
static struct ast_channel *visdn_new(
	struct visdn_chan *visdn_chan,
	int state)
{
	struct ast_channel *ast_chan;
	ast_chan = ast_channel_alloc(1);
	if (!ast_chan) {
		ast_log(LOG_WARNING, "Unable to allocate channel\n");
		goto err_channel_alloc;
	}

	ast_chan->fds[0] = open("/dev/visdn/timer", O_RDONLY);
	if (ast_chan->fds[0] < 0) {
		ast_log(LOG_ERROR, "Unable to open timer: %s\n",
			strerror(errno));
		goto err_open_timer;
	}

	if (state == AST_STATE_RING)
		ast_chan->rings = 1;

	visdn_chan->ast_chan = ast_chan;

	visdn_chan->dsp = ast_dsp_new();
	ast_dsp_set_features(visdn_chan->dsp,
		DSP_FEATURE_DTMF_DETECT |
//		DSP_FEATURE_SILENCE_SUPPRESS |
		DSP_FEATURE_BUSY_DETECT |
		DSP_FEATURE_FAX_DETECT);
	ast_dsp_digitmode(visdn_chan->dsp, DSP_DIGITMODE_DTMF);

	ast_chan->adsicpe = AST_ADSI_UNAVAILABLE;

//	ast_chan->language[0] = '\0';
//	ast_set_flag(ast_chan, AST_FLAG_DIGITAL);

	ast_chan->nativeformats = AST_FORMAT_ALAW;
	ast_chan->readformat = AST_FORMAT_ALAW;
	ast_chan->writeformat = AST_FORMAT_ALAW;
	ast_chan->rawreadformat = AST_FORMAT_ALAW;
	ast_chan->rawwriteformat = AST_FORMAT_ALAW;

	ast_chan->type = VISDN_CHAN_TYPE;

	ast_chan->tech = &visdn_tech;
	ast_chan->tech_pvt = visdn_chan;

	ast_setstate(ast_chan, state);

	return ast_chan;

	close(ast_chan->fds[0]);
err_open_timer:
	ast_hangup(ast_chan);
err_channel_alloc:

	return NULL;
}

static struct ast_channel *visdn_request(
	const char *type, int format, void *data, int *cause)
{
	struct visdn_chan *visdn_chan;

	if (!(format & AST_FORMAT_ALAW)) {
		ast_log(LOG_NOTICE,
			"Asked to get a channel of unsupported format '%d'\n",
			format);
		goto err_unsupported_format;
	}

	visdn_chan = visdn_alloc();
	if (!visdn_chan) {
		ast_log(LOG_ERROR, "Cannot allocate visdn_chan\n");
		goto err_visdn_alloc;
	}

	struct ast_channel *ast_chan;
	ast_chan = visdn_new(visdn_chan, AST_STATE_DOWN);
	if (!ast_chan)
		goto err_visdn_new;

	snprintf(ast_chan->name, sizeof(ast_chan->name), "VISDN/null");

	ast_mutex_lock(&visdn.usecnt_lock);
	visdn.usecnt++;
	ast_mutex_unlock(&visdn.usecnt_lock);
	ast_update_use_count();

	return ast_chan;

err_visdn_new:
	visdn_destroy(visdn_chan);
err_visdn_alloc:
err_unsupported_format:

	return NULL;
}

static const struct ast_channel_tech visdn_tech = {
	.type		= VISDN_CHAN_TYPE,
	.description	= VISDN_DESCRIPTION,
	.capabilities	= AST_FORMAT_ALAW,
	.requester	= visdn_request,
	.call		= visdn_call,
	.hangup		= visdn_hangup,
	.answer		= visdn_answer,
	.read		= visdn_read,
	.write		= visdn_write,
	.indicate	= visdn_indicate,
	.transfer	= visdn_transfer,
	.fixup		= visdn_fixup,
	.send_digit	= visdn_send_digit,
	.bridge		= visdn_bridge,
	.send_text	= visdn_sendtext,
	.setoption	= visdn_setoption,
};

#define MAX_PAYLOAD 1024

// Must be called with visdn.lock acquired
static void visdn_netlink_receive()
{
	struct sockaddr_nl tonl;
	tonl.nl_family = AF_NETLINK;
	tonl.nl_pid = 0;
	tonl.nl_groups = 0;

	struct msghdr skmsg;
	struct sockaddr_nl dest_addr;
	struct cmsghdr cmsg;
	struct iovec iov;

	__u8 data[NLMSG_SPACE(MAX_PAYLOAD)];

	struct nlmsghdr *hdr = (struct nlmsghdr *)data;

	iov.iov_base = data;
	iov.iov_len = sizeof(data);

	skmsg.msg_name = &dest_addr;
	skmsg.msg_namelen = sizeof(dest_addr);
	skmsg.msg_iov = &iov;
	skmsg.msg_iovlen = 1;
	skmsg.msg_control = &cmsg;
	skmsg.msg_controllen = sizeof(cmsg);
	skmsg.msg_flags = 0;

	if(recvmsg(visdn.netlink_socket, &skmsg, 0) < 0) {
		ast_log(LOG_WARNING, "recvmsg: %s\n", strerror(errno));
		return;
	}

	// Implement multipart messages FIXME FIXME TODO

	if (hdr->nlmsg_type == RTM_NEWLINK) {
		struct ifinfomsg *ifi = NLMSG_DATA(hdr);

		if (ifi->ifi_type == ARPHRD_LAPD) {

			char ifname[IFNAMSIZ] = "";
			int len = hdr->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifinfomsg));

			struct rtattr *rtattr;
			for (rtattr = IFLA_RTA(ifi);
			     RTA_OK(rtattr, len);
			     rtattr = RTA_NEXT(rtattr, len)) {

				if (rtattr->rta_type == IFLA_IFNAME) {
					strncpy(ifname,
						RTA_DATA(rtattr),
						sizeof(ifname));
				}
			}

			if (ifi->ifi_flags & IFF_UP) {
				visdn_debug("Netlink msg: %s UP %s\n",
					ifname,
					(ifi->ifi_flags & IFF_ALLMULTI) ?
				 		"NT": "TE");

				//visdn_intf_now_up(ifname);XXX
			} else {
				visdn_debug("Netlink msg: %s DOWN %s\n",
					ifname,
					(ifi->ifi_flags & IFF_ALLMULTI) ?
						"NT": "TE");

				//visdn_intf_now_down(ifname);XXX
			}
		}
	}
}

static void visdn_ccb_q931_receive()
{
	struct q931_ccb_message *msg;

	while(1) {
		ast_mutex_lock(&visdn.ccb_q931_queue_lock);

		if (list_empty(&visdn.ccb_q931_queue)) {
			ast_mutex_unlock(&visdn.ccb_q931_queue_lock);
			break;
		}

		msg = list_entry(visdn.ccb_q931_queue.next,
				struct q931_ccb_message, node);

		char buf[1];
		read(visdn.ccb_q931_queue_pipe_read, buf, 1);

		list_del_init(&msg->node);
		ast_mutex_unlock(&visdn.ccb_q931_queue_lock);

		q931_ccb_dispatch(msg);

		free(msg);
	}
}

static void visdn_q931_ccb_receive();

static int visdn_q931_thread_do_poll()
{
	longtime_t usec_to_wait = q931_run_timers();
	int msec_to_wait;

	if (usec_to_wait < 0) {
		msec_to_wait = -1;
	} else {
		msec_to_wait = usec_to_wait / 1000 + 1;
	}

	if (visdn.open_pending)
		msec_to_wait = (msec_to_wait > 0 && msec_to_wait < 2001) ?
				msec_to_wait : 2001;

	visdn_debug("poll timeout = %d\n", msec_to_wait);

	// Uhm... we should lock, copy polls and unlock before poll()
	if (poll(visdn.polls, visdn.npolls, msec_to_wait) < 0) {
		if (errno == EINTR)
			return TRUE;

		ast_log(LOG_WARNING, "poll error: %s\n", strerror(errno));
		exit(1);
	}

	ast_mutex_lock(&visdn.lock);
	if (time(NULL) > visdn.open_pending_nextcheck) {

		struct visdn_interface *intf;
		list_for_each_entry(intf, &visdn.ifs, ifs_node) {

			if (intf->open_pending) {
				visdn_debug("Retry opening interface %s\n",
						intf->name);

				if (visdn_intf_open(intf) < 0)
					visdn.open_pending_nextcheck =
							time(NULL) + 2;
			}
		}

		refresh_polls_list();
	}
	ast_mutex_unlock(&visdn.lock);

	int i;
	for(i = 0; i < visdn.npolls; i++) {
		if (visdn.poll_infos[i].type == POLL_INFO_TYPE_NETLINK) {
			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {
				visdn_netlink_receive();
				break; // polls list may have been changed
			}
		} else if (visdn.poll_infos[i].type ==
						POLL_INFO_TYPE_Q931_CCB) {

			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {
				visdn_q931_ccb_receive();
			}
		} else if (visdn.poll_infos[i].type ==
						POLL_INFO_TYPE_CCB_Q931) {

			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {
				visdn_ccb_q931_receive();
			}
		} else if (visdn.poll_infos[i].type ==
						POLL_INFO_TYPE_INTERFACE) {

			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {
				ast_mutex_lock(&visdn.lock);
				visdn_accept(
					visdn.poll_infos[i].interface,
					visdn.polls[i].fd);
				ast_mutex_unlock(&visdn.lock);
				break; // polls list may have been changed
			}
		} else if (visdn.poll_infos[i].type == POLL_INFO_TYPE_DLC) {
			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {

				int err;
				ast_mutex_lock(&visdn.lock);
				err = q931_receive(visdn.poll_infos[i].dlc);

				if (err == Q931_RECEIVE_REFRESH) {
					refresh_polls_list();
					ast_mutex_unlock(&visdn.lock);

					break;
				}
				ast_mutex_unlock(&visdn.lock);
			}
		}
	}

	ast_mutex_lock(&visdn.lock);
	int active_calls_cnt = 0;
	if (visdn.have_to_exit) {
		active_calls_cnt = 0;

		struct visdn_interface *intf;
		list_for_each_entry(intf, &visdn.ifs, ifs_node) {
			if (intf->q931_intf) {
				struct q931_call *call;
				list_for_each_entry(call,
						&intf->q931_intf->calls,
						calls_node)
					active_calls_cnt++;
			}
		}

		ast_log(LOG_WARNING,
			"There are still %d active calls, waiting...\n",
			active_calls_cnt);
	}
	ast_mutex_unlock(&visdn.lock);

	return (!visdn.have_to_exit || active_calls_cnt > 0);
}


static void *visdn_q931_thread_main(void *data)
{
	ast_mutex_lock(&visdn.lock);

	visdn.npolls = 0;
	refresh_polls_list();

	visdn.have_to_exit = 0;

	ast_mutex_unlock(&visdn.lock);

	while(visdn_q931_thread_do_poll());

	return NULL;
}

static void visdn_handle_eventual_progind(
	struct ast_channel *ast_chan,
	const struct q931_ies *ies)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	int i;
	for (i=0; i<ies->count; i++) {
		if (ies->ies[i]->cls->id == Q931_IE_PROGRESS_INDICATOR) {
			struct q931_ie_progress_indicator *pi =
				container_of(ies->ies[i],
					struct q931_ie_progress_indicator, ie);

			if (pi->progress_description ==
					Q931_IE_PI_PD_IN_BAND_INFORMATION) {

				visdn_debug("In-band informations available\n");
				
				visdn_chan->inband_info = TRUE;
				break;
			}
		}
	}
}

static void visdn_q931_alerting_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);
	visdn_handle_eventual_progind(ast_chan, ies);
	ast_mutex_unlock(&ast_chan->lock);

	ast_queue_control(ast_chan, AST_CONTROL_RINGING);
	ast_setstate(ast_chan, AST_STATE_RINGING);
}

static void visdn_q931_connect_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	q931_send_primitive(q931_call, Q931_CCB_SETUP_COMPLETE_REQUEST, NULL);

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_queue_control(ast_chan, AST_CONTROL_ANSWER);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	if (visdn_chan->ec_fd >= 0) {
		visdn_debug("Activating echo canceller\n");

		if (ioctl(visdn_chan->ec_fd, VEC_START,
			(caddr_t)&visdn_chan->ec_ne_channel_id) < 0) {

			ast_log(LOG_ERROR,
				"ioctl(VEC_START): %s\n",
				strerror(errno));
		}
	}
}

static int visdn_ie_to_ast_hangupcause(
	const struct q931_ie_cause *cause)
{
	/* Asterisk uses the same q931 causes. Is it guaranteed? */

	return cause->value;
}

static void visdn_set_hangupcause_by_ies(
	struct ast_channel *ast_chan,
	const struct q931_ies *ies)
{
	int i;
	for (i=0; i<ies->count; i++) {
		if (ies->ies[i]->cls->id == Q931_IE_CAUSE) {
			 ast_chan->hangupcause =
				visdn_ie_to_ast_hangupcause(
					container_of(ies->ies[i],
						struct q931_ie_cause, ie));
		}
	}
}


static void visdn_q931_disconnect_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	FUNC_DEBUG();

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);
	visdn_handle_eventual_progind(ast_chan, ies);
	visdn_set_hangupcause_by_ies(ast_chan, ies);
	ast_mutex_unlock(&ast_chan->lock);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	if (visdn_chan->inband_info)
		ast_queue_control(ast_chan, AST_CONTROL_INBAND_INFO);
	else
		q931_send_primitive(q931_call, Q931_CCB_RELEASE_REQUEST, NULL);

	ast_queue_control(ast_chan, AST_CONTROL_DISCONNECT);
}

static void visdn_q931_error_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();
}

static void visdn_q931_info_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	if (q931_call->state != U2_OVERLAP_SENDING &&
	    q931_call->state != N2_OVERLAP_SENDING) {
		ast_log(LOG_WARNING, "Received info not in overlap sending\n");
		return;
	}

	struct q931_ie_called_party_number *cdpn = NULL;

	int i;
	for(i=0; i<ies->count; i++) {
		if (ies->ies[i]->cls->id == Q931_IE_SENDING_COMPLETE) {
			visdn_chan->sending_complete = TRUE;
		} else if (ies->ies[i]->cls->id ==
					 Q931_IE_CALLED_PARTY_NUMBER) {
			cdpn = container_of(ies->ies[i],
				struct q931_ie_called_party_number, ie);
		}
	}

	if (!cdpn)
		return;

	for(i=0; cdpn->number[i]; i++) {
		struct ast_frame f =
			{ AST_FRAME_DTMF, cdpn->number[i] };
		ast_queue_frame(ast_chan, &f);
	}
}

static void visdn_q931_more_info_indication(
	struct q931_call *q931_call,
	const struct q931_ies *user_ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	visdn_undefer_dtmf_in(visdn_chan);
}

static void visdn_q931_notify_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();
}

static void visdn_q931_proceeding_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);
	visdn_handle_eventual_progind(ast_chan, ies);
	ast_mutex_unlock(&ast_chan->lock);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	visdn_undefer_dtmf_in(visdn_chan);

	ast_queue_control(ast_chan, AST_CONTROL_PROCEEDING);
}

static void visdn_q931_progress_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);
	visdn_handle_eventual_progind(ast_chan, ies);
	ast_mutex_unlock(&ast_chan->lock);

	ast_queue_control(ast_chan, AST_CONTROL_PROGRESS);
}

static void visdn_q931_reject_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	struct q931_ie_cause *cause = NULL;

	int i;
	for(i=0; i<ies->count; i++) {
		if (ies->ies[i]->cls->id == Q931_IE_CAUSE) {
			cause = container_of(ies->ies[i],
				struct q931_ie_cause, ie);
		}
	}

	if (visdn_chan->huntgroup &&
	    cause &&
	    (cause->value == Q931_IE_C_CV_NO_CIRCUIT_CHANNEL_AVAILABLE ||
	    cause->value == Q931_IE_C_CV_NETWORK_OUT_OF_ORDER ||
	    cause->value == Q931_IE_C_CV_TEMPORARY_FAILURE ||
	    cause->value == Q931_IE_C_CV_SWITCHING_EQUIPMENT_CONGESTION ||
	    cause->value == Q931_IE_C_CV_ACCESS_INFORMATION_DISCARDED ||
	    cause->value ==
	    		Q931_IE_C_CV_REQUESTED_CIRCUIT_CHANNEL_NOT_AVAILABLE ||
	    cause->value == Q931_IE_C_CV_RESOURCES_UNAVAILABLE)) {

		struct visdn_interface *intf;

		intf = visdn_hg_hunt(visdn_chan->huntgroup, visdn_chan->intf,
				visdn_chan->hg_first_intf);
		if (!intf) {
			ast_mutex_lock(&ast_chan->lock);
			visdn_set_hangupcause_by_ies(ast_chan, ies);
			ast_chan->_softhangup |= AST_SOFTHANGUP_DEV;
			ast_mutex_unlock(&ast_chan->lock);

			return;
		}

		visdn_request_call(ast_chan, intf,
			visdn_chan->number, visdn_chan->options);

		visdn_intf_put(intf);
		intf = NULL;
	} else {
		ast_mutex_lock(&ast_chan->lock);
		visdn_set_hangupcause_by_ies(ast_chan, ies);
		ast_chan->_softhangup |= AST_SOFTHANGUP_DEV;
		ast_mutex_unlock(&ast_chan->lock);
	}
}

static void visdn_q931_release_confirm(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_release_confirm_status status)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);
	visdn_set_hangupcause_by_ies(ast_chan, ies);
	ast_chan->_softhangup |= AST_SOFTHANGUP_DEV;
	ast_mutex_unlock(&ast_chan->lock);
}

static void visdn_q931_release_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);
	visdn_set_hangupcause_by_ies(ast_chan, ies);
	ast_chan->_softhangup |= AST_SOFTHANGUP_DEV;
	ast_mutex_unlock(&ast_chan->lock);
}

static void visdn_q931_resume_confirm(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_resume_confirm_status status)
{
	FUNC_DEBUG();
}

static void visdn_q931_resume_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	enum q931_ie_cause_value cause;

	if (callpvt_to_astchan(q931_call)) {
		ast_log(LOG_WARNING, "Unexpexted ast_chan\n");
		cause = Q931_IE_C_CV_RESOURCES_UNAVAILABLE;
		goto err_ast_chan;
	}

	struct q931_ie_call_identity *ci = NULL;

	int i;
	for (i=0; i<ies->count; i++) {
		if (ies->ies[i]->cls->id == Q931_IE_CALL_IDENTITY) {
			ci = container_of(ies->ies[i],
				struct q931_ie_call_identity, ie);
		}
	}

	struct visdn_interface *intf = q931_call->intf->pvt;
	struct visdn_suspended_call *suspended_call;

	int found = FALSE;
	list_for_each_entry(suspended_call, &intf->suspended_calls, node) {
		if ((!ci && suspended_call->call_identity_len == 0) ||
		    (suspended_call->call_identity_len == ci->data_len &&
		     !memcmp(suspended_call->call_identity, ci->data,
					ci->data_len))) {

			found = TRUE;

			break;
		}
	}

	if (!found) {
		ast_log(LOG_NOTICE, "Unable to find suspended call\n");

		if (list_empty(&intf->suspended_calls))
			cause = Q931_IE_C_CV_SUSPENDED_CALL_EXISTS_BUT_NOT_THIS;
		else
			cause = Q931_IE_C_CV_NO_CALL_SUSPENDED;

		goto err_call_not_found;
	}

	assert(suspended_call->ast_chan);

	struct visdn_chan *visdn_chan = to_visdn_chan(suspended_call->ast_chan);

	q931_call->pvt = suspended_call->ast_chan;
	visdn_chan->q931_call = q931_call_get(q931_call);
	visdn_chan->suspended_call = NULL;

	if (ast_bridged_channel(suspended_call->ast_chan)) {
		if (!strcmp(ast_bridged_channel(suspended_call->ast_chan)->type,
							VISDN_CHAN_TYPE)) {
			// Wow, the remote channel is ISDN too, let's notify it!

			Q931_DECLARE_IES(response_ies);

			struct visdn_chan *remote_visdn_chan =
				to_visdn_chan(
					ast_bridged_channel(suspended_call->
						ast_chan));

			struct q931_call *remote_call =
					remote_visdn_chan->q931_call;

			struct q931_ie_notification_indicator *notify =
				q931_ie_notification_indicator_alloc();
			notify->description = Q931_IE_NI_D_USER_RESUMED;
			q931_ies_add_put(&response_ies, &notify->ie);

			q931_send_primitive(remote_call,
				Q931_CCB_NOTIFY_REQUEST, &response_ies);

			Q931_UNDECLARE_IES(response_ies);
		}

		ast_moh_stop(ast_bridged_channel(suspended_call->ast_chan));
	}

	{
	Q931_DECLARE_IES(response_ies);

	struct q931_ie_channel_identification *ci =
		q931_ie_channel_identification_alloc();
	ci->interface_id_present = Q931_IE_CI_IIP_IMPLICIT;
	ci->interface_type =
		q931_ie_channel_identification_intftype(q931_call->intf);
	ci->preferred_exclusive = Q931_IE_CI_PE_EXCLUSIVE;
	ci->coding_standard = Q931_IE_CI_CS_CCITT;
	q931_chanset_init(&ci->chanset);
	q931_chanset_add(&ci->chanset, suspended_call->q931_chan);
	q931_ies_add_put(&response_ies, &ci->ie);

	q931_send_primitive(q931_call, Q931_CCB_RESUME_RESPONSE, &response_ies);

	Q931_UNDECLARE_IES(response_ies);
	}

	list_del(&suspended_call->node);
	free(suspended_call);

	return;

err_call_not_found:
err_ast_chan:
	;
	Q931_DECLARE_IES(resp_ies);
	struct q931_ie_cause *c = q931_ie_cause_alloc();
	c->coding_standard = Q931_IE_C_CS_CCITT;
	c->location = q931_ie_cause_location_call(q931_call);
	c->value = cause;
	q931_ies_add_put(&resp_ies, &c->ie);

	q931_send_primitive(q931_call,
		Q931_CCB_RESUME_REJECT_REQUEST, &resp_ies);

	Q931_UNDECLARE_IES(resp_ies);

	return;
}

static void visdn_q931_setup_complete_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_setup_complete_indication_status status)
{
	FUNC_DEBUG();
}

static void visdn_q931_setup_confirm(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_setup_confirm_status status)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_setstate(ast_chan, AST_STATE_UP);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	visdn_undefer_dtmf_in(visdn_chan);

	if (visdn_chan->ec_fd >= 0) {

		visdn_debug("Activating echo canceller\n");

		if (ioctl(visdn_chan->ec_fd, VEC_START,
			(caddr_t)&visdn_chan->ec_ne_channel_id) < 0) {

			ast_log(LOG_ERROR,
				"ioctl(VEC_START): %s\n",
				strerror(errno));
		}
	}
}

static int visdn_cgpn_to_pres(
	struct q931_ie_calling_party_number *cgpn)
{
	switch(cgpn->presentation_indicator) {
	case Q931_IE_CGPN_PI_PRESENTATION_ALLOWED:
		return AST_PRES_ALLOWED;
	break;

	case Q931_IE_CGPN_PI_PRESENTATION_RESTRICTED:
		return AST_PRES_RESTRICTED;
	break;

	case Q931_IE_CGPN_PI_NOT_AVAILABLE:
		return AST_PRES_UNAVAILABLE;
	break;
	}

	return 0;
}

static const char *visdn_get_prefix_by_cdpn_ton(
	struct visdn_interface *intf,
	enum q931_ie_called_party_number_type_of_number ton)
{
	switch(ton) {
	case Q931_IE_CDPN_TON_UNKNOWN:
	case Q931_IE_CDPN_TON_RESERVED_FOR_EXT:
		return "";

	case Q931_IE_CDPN_TON_INTERNATIONAL:
		return intf->international_prefix;
	break;

	case Q931_IE_CDPN_TON_NATIONAL:
		return intf->national_prefix;
	break;

	case Q931_IE_CDPN_TON_NETWORK_SPECIFIC:
		return intf->network_specific_prefix;
	break;

	case Q931_IE_CDPN_TON_SUBSCRIBER:
		return intf->subscriber_prefix;
	break;

	case Q931_IE_CDPN_TON_ABBREVIATED:
		return intf->abbreviated_prefix;
	break;
	}

	assert(0);
	return NULL;
}

static const char *visdn_get_prefix_by_cgpn_ton(
	struct visdn_interface *intf,
	enum q931_ie_calling_party_number_type_of_number ton)
{
	switch(ton) {
	case Q931_IE_CGPN_TON_UNKNOWN:
	case Q931_IE_CGPN_TON_RESERVED_FOR_EXT:
		return "";

	case Q931_IE_CGPN_TON_INTERNATIONAL:
		return intf->international_prefix;
	break;

	case Q931_IE_CGPN_TON_NATIONAL:
		return intf->national_prefix;
	break;

	case Q931_IE_CGPN_TON_NETWORK_SPECIFIC:
		return intf->network_specific_prefix;
	break;

	case Q931_IE_CGPN_TON_SUBSCRIBER:
		return intf->subscriber_prefix;
	break;

	case Q931_IE_CGPN_TON_ABBREVIATED:
		return intf->abbreviated_prefix;
	break;
	}

	assert(0);
	return NULL;
}

static void visdn_rewrite_and_assign_cli(
	struct ast_channel *ast_chan,
	struct visdn_interface *intf,
	struct q931_ie_calling_party_number *cgpn)
{
	assert(!ast_chan->cid.cid_num);

	if (intf->cli_rewriting) {
		char rewritten_num[32];

		snprintf(rewritten_num, sizeof(rewritten_num),
			"%s%s",
			visdn_get_prefix_by_cgpn_ton(intf,
						cgpn->type_of_number),
			cgpn->number);

		ast_chan->cid.cid_num = strdup(rewritten_num);
	} else {
		ast_chan->cid.cid_num = strdup(cgpn->number);
	}
}

static void visdn_handle_clip_nt(
	struct ast_channel *ast_chan,
	struct visdn_interface *intf,
	struct q931_ie_calling_party_number *cgpn)
{

	/* If the numbering plan is incorrect ignore the information
	 * element. ETS 300 092 Par. 9.3.1
	 */

	if (!cgpn) {
		ast_chan->cid.cid_num = strdup(intf->clip_default_number);
		ast_chan->cid.cid_pres = AST_PRES_NETWORK_NUMBER;

		return;
	}
	
	if (cgpn->numbering_plan_identificator !=
			Q931_IE_CGPN_NPI_UNKNOWN &&
	    cgpn->numbering_plan_identificator !=
	    		Q931_IE_CGPN_NPI_ISDN_TELEPHONY) {

		ast_chan->cid.cid_num = strdup(intf->clip_default_number);
		ast_chan->cid.cid_pres = AST_PRES_NETWORK_NUMBER;

		return;
	}

	if (intf->clip_special_arrangement) {
		ast_chan->cid.cid_pres |=
			AST_PRES_USER_NUMBER_UNSCREENED;
	} else {
		if (visdn_intf_clip_valid(intf, cgpn->number)) {
			if (0) { /* Sequence is valid but incomplete */
				/* Complete sequence TODO FIXME */
			}

			visdn_rewrite_and_assign_cli(ast_chan, intf, cgpn);

			ast_chan->cid.cid_pres |=
				AST_PRES_USER_NUMBER_PASSED_SCREEN;
		} else {
			ast_chan->cid.cid_num =
				strdup(intf->clip_default_number);
			ast_chan->cid.cid_pres |= AST_PRES_NETWORK_NUMBER;
		}
	}
}

static void visdn_q931_setup_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct visdn_chan *visdn_chan;
	visdn_chan = visdn_alloc();
	if (!visdn_chan) {
		ast_log(LOG_ERROR, "Cannot allocate visdn_chan\n");
		goto err_visdn_alloc;
	}

	visdn_chan->q931_call = q931_call_get(q931_call);

	struct visdn_interface *intf = q931_call->intf->pvt;

	struct ast_channel *ast_chan;
	ast_chan = visdn_new(visdn_chan, AST_STATE_OFFHOOK);
	if (!ast_chan)
		goto err_visdn_new;

	struct q931_ie_calling_party_number *cgpn = NULL;
	struct q931_ie_called_party_number *cdpn = NULL;
	struct q931_ie_bearer_capability *bc = NULL;

	int i;
	for(i=0; i<ies->count; i++) {
		if (ies->ies[i]->cls->id == Q931_IE_SENDING_COMPLETE) {
			visdn_chan->sending_complete = TRUE;
		} else if (ies->ies[i]->cls->id ==
				Q931_IE_CALLED_PARTY_NUMBER) {

			cdpn = container_of(ies->ies[i],
				struct q931_ie_called_party_number, ie);

		} else if (ies->ies[i]->cls->id ==
				Q931_IE_CALLING_PARTY_NUMBER) {

			cgpn = container_of(ies->ies[i],
				struct q931_ie_calling_party_number, ie);

		} else if (ies->ies[i]->cls->id == Q931_IE_BEARER_CAPABILITY) {

			bc = container_of(ies->ies[i],
				struct q931_ie_bearer_capability, ie);
		}
	}

	assert(bc);

	/* ------ Handle Bearer Capability ------ */
	
	/* We should check the destination bearer capability
	 * unfortunately we don't know if the destination is
	 * compatible until we start the PBX... this is a
	 * design flaw in Asterisk
	 */

	{
		__u8 buf[20];
		char raw_bc_text[sizeof(buf) + 1];

		assert(bc->ie.cls->write_to_buf);

		int len = bc->ie.cls->write_to_buf(&bc->ie, buf, sizeof(buf));

		for (i=0; i<len; i++)
			sprintf(raw_bc_text + i * 2, "%02x", buf[i]);

		pbx_builtin_setvar_helper(ast_chan,
			"BEARERCAP_RAW", raw_bc_text);
	}

	if (bc->information_transfer_capability ==
		Q931_IE_BC_ITC_UNRESTRICTED_DIGITAL) {

		pbx_builtin_setvar_helper(ast_chan,
			"BEARERCAP_CLASS", "data");
	
		visdn_chan->is_voice = FALSE;
		q931_call->tones_option = FALSE;

	} else  if (bc->information_transfer_capability ==
			Q931_IE_BC_ITC_SPEECH ||
		    bc->information_transfer_capability ==
			Q931_IE_BC_ITC_3_1_KHZ_AUDIO) {

		visdn_chan->is_voice = TRUE;
		q931_call->tones_option = intf->tones_option;

		pbx_builtin_setvar_helper(ast_chan,
			"BEARERCAP_CLASS", "voice");
	} else {
		Q931_DECLARE_IES(ies);

		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_call(q931_call);
		cause->value = Q931_IE_C_CV_BEARER_CAPABILITY_NOT_IMPLEMENTED;
		q931_ies_add_put(&ies, &cause->ie);

		q931_send_primitive(visdn_chan->q931_call,
			Q931_CCB_REJECT_REQUEST, &ies);

		Q931_UNDECLARE_IES(ies);

		goto err_unsupported_bearercap;
	}
	/* ------ ----------------------------- ------ */

	q931_call->pvt = ast_chan;

	snprintf(ast_chan->name, sizeof(ast_chan->name), "VISDN/%s/%d.%c",
		q931_call->intf->name,
		q931_call->call_reference,
		q931_call->direction ==
			Q931_CALL_DIRECTION_INBOUND ? 'I' : 'O');

	strncpy(ast_chan->context,
		intf->context,
		sizeof(ast_chan->context)-1);

	ast_mutex_lock(&visdn.usecnt_lock);
	visdn.usecnt++;
	ast_mutex_unlock(&visdn.usecnt_lock);
	ast_update_use_count();

	char called_number[32] = "";

	if (cdpn) {
		snprintf(called_number, sizeof(called_number),
			"%s%s",
			visdn_get_prefix_by_cdpn_ton(intf,
						cdpn->type_of_number),
			cdpn->number);

	       	if (cdpn->number[strlen(cdpn->number) - 1] == '#')
			visdn_chan->sending_complete = TRUE;
	}

	/* ------ Handle Calling Line Presentation/Restriction ------ */

	assert(!ast_chan->cid.cid_name);
	assert(!ast_chan->cid.cid_num);

	ast_chan->cid.cid_pres = 0;
	ast_chan->cid.cid_name = strdup(intf->clip_default_name);

	if (intf->q931_intf->role == LAPD_INTF_ROLE_NT) {

		visdn_handle_clip_nt(ast_chan, intf, cgpn);

		/* Handle CLIR */
		if (intf->clir_mode == VISDN_CLIR_MODE_ON)
			ast_chan->cid.cid_pres |= AST_PRES_RESTRICTED;
		else if (intf->clir_mode == VISDN_CLIR_MODE_OFF)
			ast_chan->cid.cid_pres |= AST_PRES_ALLOWED;
		else {
			if (cgpn)
				ast_chan->cid.cid_pres |=
					visdn_cgpn_to_pres(cgpn);
			else if (intf->clir_mode == VISDN_CLIR_MODE_DEFAULT_ON)
				ast_chan->cid.cid_pres |= AST_PRES_RESTRICTED;
			else
				ast_chan->cid.cid_pres |= AST_PRES_ALLOWED;
		}

	} else {
		if (!cgpn) {
			ast_chan->cid.cid_pres =
				AST_PRES_UNAVAILABLE |
				AST_PRES_NETWORK_NUMBER;

			goto no_cgpn;
		}

		visdn_rewrite_and_assign_cli(ast_chan, intf, cgpn);

		switch(cgpn->screening_indicator) {
		case Q931_IE_CGPN_SI_USER_PROVIDED_NOT_SCREENED:
			ast_chan->cid.cid_pres |=
				AST_PRES_USER_NUMBER_UNSCREENED;
		break;

		case Q931_IE_CGPN_SI_USER_PROVIDED_VERIFIED_AND_PASSED:
			ast_chan->cid.cid_pres |=
				AST_PRES_USER_NUMBER_PASSED_SCREEN;
		break;

		case Q931_IE_CGPN_SI_USER_PROVIDED_VERIFIED_AND_FAILED:
			ast_chan->cid.cid_pres |=
				AST_PRES_USER_NUMBER_FAILED_SCREEN;
		break;

		case Q931_IE_CGPN_SI_NETWORK_PROVIDED:
			ast_chan->cid.cid_pres |=
				AST_PRES_NETWORK_NUMBER;
		break;
		}

		switch(cgpn->presentation_indicator) {
		case Q931_IE_CGPN_PI_PRESENTATION_ALLOWED:
			ast_chan->cid.cid_pres |=
				AST_PRES_ALLOWED;
		break;

		case Q931_IE_CGPN_PI_PRESENTATION_RESTRICTED:
			ast_chan->cid.cid_pres |=
				AST_PRES_RESTRICTED;
		break;

		case Q931_IE_CGPN_PI_NOT_AVAILABLE:
			ast_chan->cid.cid_pres |=
				AST_PRES_UNAVAILABLE;
		break;
		}

no_cgpn:;
	}

	if (cgpn) {
		/* They appear to have the same values (!) */
		ast_chan->cid.cid_ton = cgpn->type_of_number;
	}

	/* ------ ----------------------------- ------ */

	if (!intf->overlap_sending ||
	    visdn_chan->sending_complete) {
		if (ast_exists_extension(NULL, intf->context,
				called_number, 1,
				ast_chan->cid.cid_num)) {

			strncpy(ast_chan->exten,
				called_number,
				sizeof(ast_chan->exten)-1);

			ast_setstate(ast_chan, AST_STATE_RING);

			if (ast_pbx_start(ast_chan)) {
				ast_log(LOG_ERROR,
					"Unable to start PBX on %s\n",
					ast_chan->name);
				ast_hangup(ast_chan);

				Q931_DECLARE_IES(ies);

				struct q931_ie_cause *cause =
					q931_ie_cause_alloc();
				cause->coding_standard = Q931_IE_C_CS_CCITT;
				cause->location =
					q931_ie_cause_location_call(q931_call);
				cause->value =
					Q931_IE_C_CV_DESTINATION_OUT_OF_ORDER;

				q931_ies_add_put(&ies, &cause->ie);

				q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_REJECT_REQUEST, &ies);

				Q931_UNDECLARE_IES(ies);
			} else {
				q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_PROCEEDING_REQUEST, NULL);

				ast_setstate(ast_chan, AST_STATE_RING);
			}
		} else {
			ast_log(LOG_NOTICE,
				"No extension '%s' in context '%s',"
				" rejecting call\n",
				called_number,
				intf->context);

			Q931_DECLARE_IES(ies);

			struct q931_ie_cause *cause = q931_ie_cause_alloc();
			cause->coding_standard = Q931_IE_C_CS_CCITT;
			cause->location =
				q931_ie_cause_location_call(q931_call);

			cause->value = Q931_IE_C_CV_INCOMPATIBLE_DESTINATION;
			q931_ies_add_put(&ies, &cause->ie);

			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_REJECT_REQUEST, &ies);

			ast_hangup(ast_chan);

			Q931_UNDECLARE_IES(ies);
		}
	} else {
		strncpy(ast_chan->exten, "s",
			sizeof(ast_chan->exten)-1);

		if (ast_pbx_start(ast_chan)) {
			ast_log(LOG_ERROR,
				"Unable to start PBX on %s\n",
				ast_chan->name);
			ast_hangup(ast_chan);

			Q931_DECLARE_IES(ies_proc);
			struct q931_ie_cause *cause = q931_ie_cause_alloc();
			cause->coding_standard = Q931_IE_C_CS_CCITT;
			cause->location =
				q931_ie_cause_location_call(q931_call);

			cause->value = Q931_IE_C_CV_DESTINATION_OUT_OF_ORDER;
			q931_ies_add_put(&ies_proc, &cause->ie);

			Q931_DECLARE_IES(ies_disc);
			if (visdn_chan->is_voice) {
				struct q931_ie_progress_indicator *pi =
					q931_ie_progress_indicator_alloc();
				pi->coding_standard = Q931_IE_PI_CS_CCITT;
				pi->location =
					q931_ie_progress_indicator_location(
							visdn_chan->q931_call);
				pi->progress_description =
					Q931_IE_PI_PD_IN_BAND_INFORMATION;
				q931_ies_add_put(&ies_disc, &pi->ie);
			}

			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_PROCEEDING_REQUEST, &ies_proc);
			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_DISCONNECT_REQUEST, &ies_disc);

			Q931_UNDECLARE_IES(ies_disc);
			Q931_UNDECLARE_IES(ies_proc);

			return;
		}

#if 0 // Don't be tempted :^)
		Q931_DECLARE_IES(ies);
		struct q931_ie_display *disp = q931_ie_display_alloc();
		strcpy(disp->text, "Mark Spencer Sucks");
		q931_ies_add_put(&ies, &disp->ie);

		q931_send_primitive(visdn_chan->q931_call,
			Q931_CCB_MORE_INFO_REQUEST, &ies);

		Q931_UNDECLARE_IES(ies);
#endif

		q931_send_primitive(visdn_chan->q931_call,
			Q931_CCB_MORE_INFO_REQUEST, NULL);

		for(i=0; called_number[i]; i++) {
			struct ast_frame f =
				{ AST_FRAME_DTMF, called_number[i] };

			ast_queue_frame(ast_chan, &f);
		}
	}

	return;

err_unsupported_bearercap:
	ast_hangup(ast_chan);
err_visdn_new:
	visdn_destroy(visdn_chan);
err_visdn_alloc:
;
}

static void visdn_q931_status_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_status_indication_status status)
{
	FUNC_DEBUG();
}

static void visdn_q931_suspend_confirm(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_suspend_confirm_status status)
{
	FUNC_DEBUG();
}

struct visdn_dual {
	struct ast_channel *chan1;
	struct ast_channel *chan2;
};

static void visdn_q931_suspend_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	enum q931_ie_cause_value cause;

	if (!ast_chan) {
		ast_log(LOG_WARNING, "Unexpexted ast_chan\n");
		cause = Q931_IE_C_CV_RESOURCES_UNAVAILABLE;
		goto err_ast_chan;
	}

	struct q931_ie_call_identity *ci = NULL;

	int i;
	for (i=0; i<ies->count; i++) {
		if (ies->ies[i]->cls->id == Q931_IE_CALL_IDENTITY) {
			ci = container_of(ies->ies[i],
				struct q931_ie_call_identity, ie);
		}
	}

	struct visdn_interface *intf = q931_call->intf->pvt;
	struct visdn_suspended_call *suspended_call;
	list_for_each_entry(suspended_call, &intf->suspended_calls, node) {
		if ((!ci && suspended_call->call_identity_len == 0) ||
		    (ci && suspended_call->call_identity_len == ci->data_len &&
		     !memcmp(suspended_call->call_identity,
					ci->data, ci->data_len))) {

			cause = Q931_IE_C_CV_CALL_IDENITY_IN_USE;

			goto err_call_identity_in_use;
		}
	}

	suspended_call = malloc(sizeof(*suspended_call));
	if (!suspended_call) {
		cause = Q931_IE_C_CV_RESOURCES_UNAVAILABLE;
		goto err_suspend_alloc;
	}

	suspended_call->ast_chan = ast_chan;
	suspended_call->q931_chan = q931_call->channel;

	if (ci) {
		suspended_call->call_identity_len = ci->data_len;
		memcpy(suspended_call->call_identity, ci->data, ci->data_len);
	} else {
		suspended_call->call_identity_len = 0;
	}

	suspended_call->old_when_to_hangup = ast_chan->whentohangup;

	list_add_tail(&suspended_call->node, &intf->suspended_calls);

	q931_send_primitive(q931_call, Q931_CCB_SUSPEND_RESPONSE, NULL);

	if (ast_bridged_channel(ast_chan)) {
		ast_moh_start(ast_bridged_channel(ast_chan), NULL);

		if (!strcmp(ast_bridged_channel(ast_chan)->type,
							VISDN_CHAN_TYPE)) {
			// Wow, the remote channel is ISDN too, let's notify it!

			Q931_DECLARE_IES(response_ies);

			struct visdn_chan *remote_visdn_chan =
				to_visdn_chan(ast_bridged_channel(ast_chan));

			struct q931_call *remote_call =
					remote_visdn_chan->q931_call;

			struct q931_ie_notification_indicator *notify =
				q931_ie_notification_indicator_alloc();
			notify->description = Q931_IE_NI_D_USER_SUSPENDED;
			q931_ies_add_put(&response_ies, &notify->ie);

			q931_send_primitive(remote_call,
				Q931_CCB_NOTIFY_REQUEST, &response_ies);

			Q931_UNDECLARE_IES(response_ies);
		}
	}

	if (!ast_chan->whentohangup ||
	    time(NULL) + 45 < ast_chan->whentohangup)
		ast_channel_setwhentohangup(ast_chan, intf->T307);

	q931_call->pvt = NULL;
	visdn_chan->q931_call = NULL;
	visdn_chan->suspended_call = suspended_call;
	q931_call_put(q931_call);

	return;

err_suspend_alloc:
err_call_identity_in_use:
err_ast_chan:
	;
	Q931_DECLARE_IES(resp_ies);
	struct q931_ie_cause *c = q931_ie_cause_alloc();
	c->coding_standard = Q931_IE_C_CS_CCITT;
	c->location = q931_ie_cause_location_call(q931_call);
	c->value = cause;
	q931_ies_add_put(&resp_ies, &c->ie);

	q931_send_primitive(visdn_chan->q931_call,
		Q931_CCB_SUSPEND_REJECT_REQUEST, &resp_ies);

	Q931_UNDECLARE_IES(resp_ies);

	return;
}

static void visdn_q931_timeout_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();
}

static int visdn_connect_channels(
	struct visdn_chan *visdn_chan)
{
	visdn_debug("Connecting streamport %06d to chan %06d\n",
			visdn_chan->sp_channel_id,
			visdn_chan->isdn_channel_id);

	struct visdn_connect vc;
	vc.src_chan_id = visdn_chan->sp_channel_id;
	vc.dst_chan_id = visdn_chan->isdn_channel_id;
	vc.flags = 0;

	if (ioctl(visdn.cxc_control_fd, VISDN_IOC_CONNECT_PATH,
						(caddr_t) &vc) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_CONNECT_PATH, sp, isdn): %s\n",
			strerror(errno));
		goto err_ioctl;
	}

	vc.src_chan_id = visdn_chan->isdn_channel_id;
	vc.flags = 0;

	if (ioctl(visdn_chan->sp_fd, VISDN_IOC_ENABLE_PATH, (caddr_t)&vc) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_ENABLE_PATH, isdn): %s\n",
			strerror(errno));
		goto err_ioctl;
	}

	return 0;

err_ioctl:

	return -1;
}

static int visdn_connect_channels_with_ec(
	struct visdn_chan *visdn_chan)
{
	visdn_debug("Connecting streamport %06d to chan %06d via EC\n",
			visdn_chan->sp_channel_id,
			visdn_chan->isdn_channel_id);

	visdn_chan->ec_fd = open("/dev/visdn/ec-control", O_RDWR);
	if (visdn_chan->ec_fd < 0) {
		ast_log(LOG_ERROR,
			"Cannot open ec-control: %s\n",
			strerror(errno));
		goto err_open;
	}

/*	if (ioctl(visdn_chan->ec_fd, VEC_SET_TAPS,
		intf->echocancel_taps) < 0) {

		ast_log(LOG_ERROR,
			"ioctl(VEC_SET_TAPS): %s\n",
			strerror(errno));
		goto err_ioctl;
	}*/

	if (ioctl(visdn_chan->ec_fd, VEC_GET_NEAREND_CHANID,
		(caddr_t)&visdn_chan->ec_ne_channel_id) < 0) {

		ast_log(LOG_ERROR,
			"ioctl(VEC_GET_NEAREND_ID): %s\n",
			strerror(errno));
		goto err_ioctl;
	}

	if (ioctl(visdn_chan->ec_fd, VEC_GET_FAREND_CHANID,
		(caddr_t)&visdn_chan->ec_fe_channel_id) < 0) {

		ast_log(LOG_ERROR,
			"ioctl(VEC_GET_FAREND_ID): %s\n",
			strerror(errno));
		goto err_ioctl;
	}

	visdn_debug("EC near_end=%06d far_end=%06d\n",
			visdn_chan->ec_ne_channel_id,
			visdn_chan->ec_fe_channel_id);

	struct visdn_connect vc;
	vc.src_chan_id = visdn_chan->sp_channel_id;
	vc.dst_chan_id = visdn_chan->ec_fe_channel_id;
	vc.flags = 0;

	if (ioctl(visdn.cxc_control_fd, VISDN_IOC_CONNECT_PATH,
	    (caddr_t) &vc) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_CONNECT_PATH, sp, ec_fe): %s\n",
			strerror(errno));
		goto err_ioctl;
	}

	vc.src_chan_id = visdn_chan->ec_ne_channel_id;
	vc.dst_chan_id = visdn_chan->isdn_channel_id;
	vc.flags = 0;

	if (ioctl(visdn.cxc_control_fd, VISDN_IOC_CONNECT_PATH,
	    (caddr_t) &vc) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_CONNECT_PATH, ec_ne, isdn): %s\n",
			strerror(errno));
		goto err_ioctl;
	}

	vc.src_chan_id = visdn_chan->sp_channel_id;
	vc.dst_chan_id = 0;
	vc.flags = 0;

	if (ioctl(visdn.cxc_control_fd, VISDN_IOC_ENABLE_PATH,
						 (caddr_t)&vc) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_ENABLE_PATH, sp): %s\n",
			strerror(errno));
		goto err_ioctl;
	}

	vc.src_chan_id = visdn_chan->isdn_channel_id;
	vc.dst_chan_id = 0;
	vc.flags = 0;

	if (ioctl(visdn.cxc_control_fd, VISDN_IOC_ENABLE_PATH,
						 (caddr_t)&vc) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_ENABLE_PATH, isdn): %s\n",
			strerror(errno));
		goto err_ioctl;
	}

	if (visdn_chan->q931_call->state == N10_ACTIVE ||
	    visdn_chan->q931_call->state == U10_ACTIVE) {
		visdn_debug("Activating echo canceller\n");

		if (ioctl(visdn_chan->ec_fd, VEC_START,
			(caddr_t)&visdn_chan->ec_ne_channel_id) < 0) {

			ast_log(LOG_ERROR,
				"ioctl(VEC_START): %s\n",
				strerror(errno));
		}
	}

	return 0;

err_ioctl:
	close(visdn_chan->sp_fd);
err_open:

	return -1;
}

static void visdn_q931_connect_channel(
	struct q931_channel *channel)
{
	FUNC_DEBUG();

	assert(channel->call);
	struct ast_channel *ast_chan = callpvt_to_astchan(channel->call);

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	struct visdn_interface *visdn_intf = channel->intf->pvt;

	char path[100], dest[100];
	snprintf(path, sizeof(path),
		"%s/B%d",
		visdn_intf->remote_port,
		channel->id+1);

	memset(dest, 0, sizeof(dest));
	if (readlink(path, dest, sizeof(dest) - 1) < 0) {
		ast_log(LOG_ERROR, "readlink(%s): %s\n", path, strerror(errno));
		goto err_readlink;
	}

	char *chanid = strrchr(dest, '/');
	if (!chanid || !strlen(chanid + 1)) {
		ast_log(LOG_ERROR,
			"Invalid chanid found in symlink %s\n",
			dest);
		goto err_invalid_chanid;
	}

	visdn_chan->isdn_channel_id = atoi(chanid + 1);

	if (visdn_chan->is_voice) {
		visdn_chan->sp_fd = open("/dev/visdn/streamport", O_RDWR);
		if (visdn_chan->sp_fd < 0) {
			ast_log(LOG_ERROR,
				"Cannot open streamport: %s\n",
				strerror(errno));
			goto err_open;
		}

		if (ioctl(visdn_chan->sp_fd, VISDN_SP_GET_CHANID,
			(caddr_t)&visdn_chan->sp_channel_id) < 0) {

			ast_log(LOG_ERROR,
				"ioctl(VISDN_IOC_GET_CHANID): %s\n",
				strerror(errno));
			goto err_ioctl;
		}

		/* FIXME TODO FIXME XXX Handle return value */
		if (visdn_intf->echocancel)
			visdn_connect_channels_with_ec(visdn_chan);
		else
			visdn_connect_channels(visdn_chan);
	}

	ast_mutex_unlock(&ast_chan->lock);

	return;

err_ioctl:
err_open:
err_invalid_chanid:
err_readlink:

	ast_mutex_unlock(&ast_chan->lock);
}

static void visdn_q931_disconnect_channel(
	struct q931_channel *channel)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(channel->call);

	if (!ast_chan)
		return;

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	ast_mutex_lock(&ast_chan->lock);

	visdn_disconnect_chan_from_visdn(visdn_chan);

	ast_mutex_unlock(&ast_chan->lock);
}

static void visdn_q931_start_tone(struct q931_channel *channel,
	enum q931_tone_type tone)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(channel->call);

	// Unfortunately, after ast_hangup the channel is not valid
	// anymore and we cannot generate further tones thought we should
	if (!ast_chan)
		return;

	switch (tone) {
	case Q931_TONE_DIAL:
		ast_indicate(ast_chan, AST_CONTROL_OFFHOOK);
	break;

	case Q931_TONE_HANGUP:
		ast_indicate(ast_chan, AST_CONTROL_HANGUP);
	break;

	case Q931_TONE_BUSY:
		ast_indicate(ast_chan, AST_CONTROL_BUSY);
	break;

	case Q931_TONE_FAILURE:
		ast_indicate(ast_chan, AST_CONTROL_CONGESTION);
	break;
	default:;
	}
}

static void visdn_q931_stop_tone(struct q931_channel *channel)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(channel->call);

	if (!ast_chan)
		return;

	ast_indicate(ast_chan, -1);
}

static void visdn_q931_management_restart_confirm(
	struct q931_global_call *gc,
	const struct q931_chanset *chanset)
{
	FUNC_DEBUG();
}

static void visdn_q931_timeout_management_indication(
	struct q931_global_call *gc)
{
	FUNC_DEBUG();
}

static void visdn_q931_status_management_indication(
	struct q931_global_call *gc)
{
	FUNC_DEBUG();
}

static void visdn_logger(int level, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	char msg[200];
	vsnprintf(msg, sizeof(msg), format, ap);
	va_end(ap);

	switch(level) {
	case Q931_LOG_DEBUG:
		if (visdn.debug_q931)
			ast_verbose("q931 %s", msg);
	break;

	case Q931_LOG_INFO:
		ast_verbose(VERBOSE_PREFIX_2  "%s", msg);
	break;

	case Q931_LOG_NOTICE:
		ast_log(__LOG_NOTICE, "libq931", 0, "", "%s", msg);
	break;

	case Q931_LOG_WARNING:
		ast_log(__LOG_WARNING, "libq931", 0, "", "%s", msg);
	break;

	case Q931_LOG_ERR:
	case Q931_LOG_CRIT:
	case Q931_LOG_ALERT:
	case Q931_LOG_EMERG:
		ast_log(__LOG_ERROR, "libq931", 0, "", "%s", msg);
	break;
	}
}

void visdn_q931_timer_update()
{
	pthread_kill(visdn_q931_thread, SIGURG);
}

static void visdn_q931_ccb_receive()
{
	struct q931_ccb_message *msg;

	while(1) {
		ast_mutex_lock(&visdn.q931_ccb_queue_lock);

		if (list_empty(&visdn.q931_ccb_queue)) {
			ast_mutex_unlock(&visdn.q931_ccb_queue_lock);
			break;
		}

		msg = list_entry(visdn.q931_ccb_queue.next,
				struct q931_ccb_message, node);

		char buf[1];
		read(visdn.q931_ccb_queue_pipe_read, buf, 1);

		list_del_init(&msg->node);
		ast_mutex_unlock(&visdn.q931_ccb_queue_lock);

		switch (msg->primitive) {
		case Q931_CCB_ALERTING_INDICATION:
			visdn_q931_alerting_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_CONNECT_INDICATION:
			visdn_q931_connect_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_DISCONNECT_INDICATION:
			visdn_q931_disconnect_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_ERROR_INDICATION:
			visdn_q931_error_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_INFO_INDICATION:
			visdn_q931_info_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_MORE_INFO_INDICATION:
			visdn_q931_more_info_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_NOTIFY_INDICATION:
			visdn_q931_notify_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_PROCEEDING_INDICATION:
			visdn_q931_proceeding_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_PROGRESS_INDICATION:
			visdn_q931_progress_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_REJECT_INDICATION:
			visdn_q931_reject_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_RELEASE_CONFIRM:
			visdn_q931_release_confirm(msg->call, &msg->ies,
								msg->par1);
		break;

		case Q931_CCB_RELEASE_INDICATION:
			visdn_q931_release_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_RESUME_CONFIRM:
			visdn_q931_resume_confirm(msg->call, &msg->ies,
							msg->par1);
		break;

		case Q931_CCB_RESUME_INDICATION:
			visdn_q931_resume_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_SETUP_COMPLETE_INDICATION:
			visdn_q931_setup_complete_indication(msg->call,
						&msg->ies, msg->par1);
		break;

		case Q931_CCB_SETUP_CONFIRM:
			visdn_q931_setup_confirm(msg->call, &msg->ies,
						msg->par1);
		break;

		case Q931_CCB_SETUP_INDICATION:
			visdn_q931_setup_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_STATUS_INDICATION:
			visdn_q931_status_indication(msg->call, &msg->ies,
						msg->par1);
		break;

		case Q931_CCB_SUSPEND_CONFIRM:
			visdn_q931_suspend_confirm(msg->call, &msg->ies,
						msg->par1);
		break;

		case Q931_CCB_SUSPEND_INDICATION:
			visdn_q931_suspend_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_TIMEOUT_INDICATION:
			visdn_q931_timeout_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_TIMEOUT_MANAGEMENT_INDICATION:
			visdn_q931_timeout_management_indication(
				(struct q931_global_call *)msg->par1);
		break;

		case Q931_CCB_STATUS_MANAGEMENT_INDICATION:
			visdn_q931_status_management_indication(
				(struct q931_global_call *)msg->par1);
		break;

		case Q931_CCB_MANAGEMENT_RESTART_CONFIRM:
			visdn_q931_management_restart_confirm(
				(struct q931_global_call *)msg->par1,
				(struct q931_chanset *)msg->par2);
		break;

		case Q931_CCB_CONNECT_CHANNEL:
			visdn_q931_connect_channel(
				(struct q931_channel *)msg->par1);
		break;

		case Q931_CCB_DISCONNECT_CHANNEL:
			visdn_q931_disconnect_channel(
				(struct q931_channel *)msg->par1);
		break;

		case Q931_CCB_START_TONE:
			visdn_q931_start_tone(
				(struct q931_channel *)msg->par1, msg->par2);
		break;

		case Q931_CCB_STOP_TONE:
			visdn_q931_stop_tone((struct q931_channel *)msg->par1);
		break;

		default:
			ast_log(LOG_WARNING, "Unexpected primitive %d\n",
				msg->primitive);
		}

		free(msg);
	}
}

/*---------------------------------------------------------------------------*/

static int do_debug_visdn_generic(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);
	visdn.debug = TRUE;
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN debugging enabled\n");

	return 0;
}

static char debug_visdn_generic_help[] =
	"Usage: debug visdn generic\n"
	"	Debug generic vISDN events\n";

static struct ast_cli_entry debug_visdn_generic =
{
	{ "debug", "visdn", "generic", NULL },
	do_debug_visdn_generic,
	"Enables generic vISDN debugging",
	debug_visdn_generic_help,
	NULL
};

/*---------------------------------------------------------------------------*/

static int do_no_debug_visdn_generic(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);
	visdn.debug = FALSE;
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN debugging disabled\n");

	return 0;
}

static struct ast_cli_entry no_debug_visdn_generic =
{
	{ "no", "debug", "visdn", "generic", NULL },
	do_no_debug_visdn_generic,
	"Disables generic vISDN debugging",
	NULL,
	NULL
};

/*---------------------------------------------------------------------------*/

static int do_debug_visdn_q921(int fd, int argc, char *argv[])
{
	// Enable debugging on new DLCs FIXME TODO

	ast_mutex_lock(&visdn.lock);
	visdn.debug_q921 = TRUE;
	visdn_set_socket_debug(1);
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN q.921 debugging enabled\n");

	return 0;
}

static char debug_visdn_q921_help[] =
	"Usage: debug visdn q921 [interface]\n"
	"	Traces q921 traffic\n";

static struct ast_cli_entry debug_visdn_q921 =
{
	{ "debug", "visdn", "q921", NULL },
	do_debug_visdn_q921,
	"Enables q.921 tracing",
	debug_visdn_q921_help,
	NULL
};

/*---------------------------------------------------------------------------*/

static int do_no_debug_visdn_q921(int fd, int argc, char *argv[])
{
	// Disable debugging on new DLCs FIXME TODO

	ast_mutex_lock(&visdn.lock);
	visdn.debug_q921 = FALSE;
	visdn_set_socket_debug(0);
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN q.921 debugging disabled\n");

	return 0;
}

static struct ast_cli_entry no_debug_visdn_q921 =
{
	{ "no", "debug", "visdn", "q921", NULL },
	do_no_debug_visdn_q921,
	"Disables q.921 tracing",
	NULL,
	NULL
};

/*---------------------------------------------------------------------------*/

static int do_debug_visdn_q931(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);
	visdn.debug_q931 = TRUE;
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN q.931 debugging enabled\n");

	return 0;
}

static char debug_visdn_q931_help[] =
	"Usage: debug visdn q931 [interface]\n"
	"	Traces q931 traffic\n";

static struct ast_cli_entry debug_visdn_q931 =
{
	{ "debug", "visdn", "q931", NULL },
	do_debug_visdn_q931,
	"Enables q.931 tracing",
	debug_visdn_q931_help,
	NULL
};

/*---------------------------------------------------------------------------*/

static int do_no_debug_visdn_q931(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);
	visdn.debug_q931 = FALSE;
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN q.931 debugging disabled\n");

	return 0;
}

static struct ast_cli_entry no_debug_visdn_q931 =
{
	{ "no", "debug", "visdn", "q931", NULL },
	do_no_debug_visdn_q931,
	"Disables q.931 tracing",
	NULL,
	NULL
};

/*---------------------------------------------------------------------------*/

static int do_visdn_reload(int fd, int argc, char *argv[])
{
	visdn_reload_config();

	return 0;
}

static char visdn_visdn_reload_help[] =
	"Usage: visdn reload\n"
	"	Reloads vISDN config\n";

static struct ast_cli_entry visdn_reload =
{
	{ "visdn", "reload", NULL },
	do_visdn_reload,
	"Reloads vISDN configuration",
	visdn_visdn_reload_help,
	NULL
};

/*---------------------------------------------------------------------------*/

static int do_show_visdn_channels(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);

	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {

		if (!intf->q931_intf)
			continue;

		ast_cli(fd, "Interface: %s\n", intf->name);

		int j;
		for (j=0; j<intf->q931_intf->n_channels; j++) {
			ast_cli(fd, "  B%d: %s\n",
				intf->q931_intf->channels[j].id + 1,
				q931_channel_state_to_text(
					intf->q931_intf->channels[j].state));
		}
	}

	ast_mutex_unlock(&visdn.lock);

	return 0;
}

static char visdn_show_visdn_channels_help[] =
	"Usage: show visdn channels\n"
	"	Displays informations on vISDN channels\n";

static struct ast_cli_entry show_visdn_channels =
{
	{ "show", "visdn", "channels", NULL },
	do_show_visdn_channels,
	"Displays vISDN channel information",
	visdn_show_visdn_channels_help,
	NULL
};

/*---------------------------------------------------------------------------*/

static int visdn_cli_print_call_list(
	int fd,
	struct q931_interface *filter_intf)
{
	int first_call;

	ast_mutex_lock(&visdn.lock);

	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {

		if (!intf->q931_intf)
			continue;

		struct q931_call *call;
		first_call = TRUE;

		list_for_each_entry(call, &intf->q931_intf->calls, calls_node) {

			if (!filter_intf || call->intf == filter_intf) {

				if (first_call) {
					ast_cli(fd,
						"Interface: %s\n",
						intf->q931_intf->name);
					ast_cli(fd,
						"  CallRef  State\n");
					first_call = FALSE;
				}

				struct ast_channel *ast_chan =
						callpvt_to_astchan(call);

				struct visdn_chan *visdn_chan = NULL;
				if (ast_chan)
					visdn_chan = to_visdn_chan(ast_chan);

				ast_cli(fd, "  %5d.%c %s %d\n",
					call->call_reference,
					(call->direction ==
						Q931_CALL_DIRECTION_INBOUND)
							? 'I' : 'O',
					q931_call_state_to_text(call->state),
					call->refcnt);
			}
		}
	}

	ast_mutex_unlock(&visdn.lock);

	return RESULT_SUCCESS;
}

static void visdn_cli_print_call(int fd, struct q931_call *call)
{
	ast_cli(fd, "--------- Call %d %s (%d refs)\n",
		call->call_reference,
		call->direction == Q931_CALL_DIRECTION_INBOUND ?
			"inbound" : "outbound",
		call->refcnt);

	ast_cli(fd, "Interface       : %s\n", call->intf->name);

	if (call->dlc)
		ast_cli(fd, "DLC (TEI)       : %d\n", call->dlc->tei);

	ast_cli(fd, "State           : %s\n",
		q931_call_state_to_text(call->state));

	ast_cli(fd, "Broadcast seutp : %s\n",
		call->broadcast_setup ? "Yes" : "No");

	ast_cli(fd, "Tones option    : %s\n",
		call->tones_option ? "Yes" : "No");

	ast_cli(fd, "Active timers   : ");

	if (call->T301.pending) ast_cli(fd, "T301 ");
	if (call->T302.pending) ast_cli(fd, "T302 ");
	if (call->T303.pending) ast_cli(fd, "T303 ");
	if (call->T304.pending) ast_cli(fd, "T304 ");
	if (call->T305.pending) ast_cli(fd, "T305 ");
	if (call->T306.pending) ast_cli(fd, "T306 ");
	if (call->T308.pending) ast_cli(fd, "T308 ");
	if (call->T309.pending) ast_cli(fd, "T309 ");
	if (call->T310.pending) ast_cli(fd, "T310 ");
	if (call->T312.pending) ast_cli(fd, "T312 ");
	if (call->T313.pending) ast_cli(fd, "T313 ");
	if (call->T314.pending) ast_cli(fd, "T314 ");
	if (call->T316.pending) ast_cli(fd, "T316 ");
	if (call->T318.pending) ast_cli(fd, "T318 ");
	if (call->T319.pending) ast_cli(fd, "T319 ");
	if (call->T320.pending) ast_cli(fd, "T320 ");
	if (call->T321.pending) ast_cli(fd, "T321 ");
	if (call->T322.pending) ast_cli(fd, "T322 ");

	ast_cli(fd, "\n");

	ast_cli(fd, "CES:\n");
	struct q931_ces *ces;
	list_for_each_entry(ces, &call->ces, node) {
		ast_cli(fd, "%d %s %s ",
			ces->dlc->tei,
			q931_ces_state_to_text(ces->state),
			(ces == call->selected_ces ? "presel" :
			  (ces == call->preselected_ces ? "sel" : "")));

		if (ces->T304.pending) ast_cli(fd, "T304 ");
		if (ces->T308.pending) ast_cli(fd, "T308 ");
		if (ces->T322.pending) ast_cli(fd, "T322 ");

		ast_cli(fd, "\n");
	}

}

static int do_show_visdn_calls(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);

	if (argc == 3) {
		visdn_cli_print_call_list(fd, NULL);
	} else if (argc == 4) {
		char *callpos = strchr(argv[3], '/');
		if (callpos) {
			*callpos = '\0';
			callpos++;
		}

		struct visdn_interface *filter_intf = NULL;
		struct visdn_interface *intf;
		list_for_each_entry(intf, &visdn.ifs, ifs_node) {
			if (intf->q931_intf &&
			    !strcasecmp(intf->name, argv[3])) {
				filter_intf = intf;
				break;
			}
		}

		if (!filter_intf) {
			ast_cli(fd, "Interface not found\n");
			goto err_intf_not_found;
		}

		if (!callpos) {
			visdn_cli_print_call_list(fd, filter_intf->q931_intf);
		} else {
			struct q931_call *call;

			if (callpos[0] == 'i' || callpos[0] == 'I') {
				call = q931_get_call_by_reference(
							filter_intf->q931_intf,
					Q931_CALL_DIRECTION_INBOUND,
					atoi(callpos + 1));
			} else if (callpos[0] == 'o' || callpos[0] == 'O') {
				call = q931_get_call_by_reference(
							filter_intf->q931_intf,
					Q931_CALL_DIRECTION_OUTBOUND,
					atoi(callpos + 1));
			} else {
				ast_cli(fd, "Invalid call reference\n");
				goto err_unknown_direction;
			}

			if (!call) {
				ast_cli(fd, "Call %s not found\n", callpos);
				goto err_call_not_found;
			}

			visdn_cli_print_call(fd, call);

			q931_call_put(call);
		}
	}

err_call_not_found:
err_unknown_direction:
err_intf_not_found:

	ast_mutex_unlock(&visdn.lock);

	return RESULT_SUCCESS;
}

static char show_visdn_calls_help[] =
	"Usage: show visdn calls\n"
	"	Lists vISDN calls\n";

static struct ast_cli_entry show_visdn_calls =
{
	{ "show", "visdn", "calls", NULL },
	do_show_visdn_calls,
	"Lists vISDN calls",
	show_visdn_calls_help,
	NULL
};

/*---------------------------------------------------------------------------*/


int load_module()
{
	// Initialize q.931 library.
	// No worries, internal structures are read-only and thread safe
	ast_mutex_init(&visdn.lock);
	ast_mutex_init(&visdn.usecnt_lock);

	INIT_LIST_HEAD(&visdn.ccb_q931_queue);
	ast_mutex_init(&visdn.ccb_q931_queue_lock);

	INIT_LIST_HEAD(&visdn.q931_ccb_queue);
	ast_mutex_init(&visdn.q931_ccb_queue_lock);

	int filedes[2];
	if (pipe(filedes) < 0) {
		ast_log(LOG_ERROR, "Unable to create pipe: %s\n",
			strerror(errno));
		goto err_pipe_ccb_q931;
	}

	visdn.ccb_q931_queue_pipe_read = filedes[0];
	visdn.ccb_q931_queue_pipe_write = filedes[1];

	if (pipe(filedes) < 0) {
		ast_log(LOG_ERROR, "Unable to create pipe: %s\n",
			strerror(errno));
		goto err_pipe_q931_ccb;
	}

	visdn.q931_ccb_queue_pipe_read = filedes[0];
	visdn.q931_ccb_queue_pipe_write = filedes[1];

	INIT_LIST_HEAD(&visdn.ifs);
	INIT_LIST_HEAD(&visdn.huntgroups_list);

	q931_init();
	q931_set_report_func(visdn_logger);
	q931_set_timer_update_func(visdn_q931_timer_update);
	q931_set_queue_primitive_func(visdn_queue_primitive);

	visdn_reload_config();

	visdn.netlink_socket = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if(visdn.netlink_socket < 0) {
		ast_log(LOG_ERROR, "Unable to open netlink socket: %s\n",
			strerror(errno));
		goto err_socket_netlink;
	}

	struct sockaddr_nl snl;
	snl.nl_family = AF_NETLINK;
	snl.nl_pid = getpid();
	snl.nl_groups = RTMGRP_LINK;

	if (bind(visdn.netlink_socket,
			(struct sockaddr *)&snl,
			sizeof(snl)) < 0) {
		ast_log(LOG_ERROR, "Unable to bind netlink socket: %s\n",
			strerror(errno));
		goto err_bind_netlink;
	}

#if 0
	// Enum interfaces and open them
	struct ifaddrs *ifaddrs;
	struct ifaddrs *ifaddr;

	if (getifaddrs(&ifaddrs) < 0) {
		ast_log(LOG_ERROR, "getifaddr: %s\n", strerror(errno));
		goto err_getifaddrs;
	}

	int fd;
	fd = socket(PF_LAPD, SOCK_SEQPACKET, 0);
	if (fd < 0) {
		ast_log(LOG_ERROR, "socket: %s\n", strerror(errno));
		goto err_socket_lapd;
	}

	for (ifaddr = ifaddrs ; ifaddr; ifaddr = ifaddr->ifa_next) {
		struct ifreq ifreq;

		memset(&ifreq, 0, sizeof(ifreq));

		strncpy(ifreq.ifr_name,
			ifaddr->ifa_name,
			sizeof(ifreq.ifr_name));

		if (ioctl(fd, SIOCGIFHWADDR, &ifreq) < 0) {
			ast_log(LOG_ERROR, "ioctl (%s): %s\n",
				ifaddr->ifa_name, strerror(errno));
			continue;
		}

		if (ifreq.ifr_hwaddr.sa_family != ARPHRD_LAPD)
			continue;

		if (!(ifaddr->ifa_flags & IFF_UP))
			continue;

		// visdn_add_interface(ifreq.ifr_name);XXX

	}
	close(fd);
	freeifaddrs(ifaddrs);
#endif

	visdn.cxc_control_fd = open("/dev/visdn/cxc-control", O_RDWR);
	if (visdn.cxc_control_fd < 0) {
		ast_log(LOG_ERROR, "Unable to open timer: %s\n",
			strerror(errno));
		goto err_open_cxc_control;
	}

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	if (ast_pthread_create(&visdn_q931_thread, &attr,
					visdn_q931_thread_main, NULL) < 0) {
		ast_log(LOG_ERROR, "Unable to start q931 thread.\n");
		goto err_thread_create;
	}

	if (ast_channel_register(&visdn_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel class %s\n",
			VISDN_CHAN_TYPE);
		goto err_channel_register;
	}

	ast_cli_register(&debug_visdn_generic);
	ast_cli_register(&no_debug_visdn_generic);
	ast_cli_register(&debug_visdn_q921);
	ast_cli_register(&no_debug_visdn_q921);
	ast_cli_register(&debug_visdn_q931);
	ast_cli_register(&no_debug_visdn_q931);
	ast_cli_register(&visdn_reload);
	ast_cli_register(&show_visdn_channels);
	ast_cli_register(&show_visdn_calls);

	visdn_intf_cli_register();
	visdn_hg_cli_register();

	visdn_overlap_register();

	return 0;

err_channel_register:
err_thread_create:
	close(visdn.cxc_control_fd);
err_open_cxc_control:
//err_socket_lapd:
//err_getifaddrs:
err_bind_netlink:
	close(visdn.netlink_socket);
err_socket_netlink:
	close(visdn.q931_ccb_queue_pipe_write);
	close(visdn.q931_ccb_queue_pipe_read);
err_pipe_q931_ccb:
	close(visdn.ccb_q931_queue_pipe_write);
	close(visdn.ccb_q931_queue_pipe_read);
err_pipe_ccb_q931:

	return -1;
}

int unload_module(void)
{
	visdn_overlap_unregister();

	visdn_intf_cli_unregister();
	visdn_hg_cli_unregister();

	ast_cli_unregister(&show_visdn_calls);
	ast_cli_unregister(&show_visdn_channels);
	ast_cli_unregister(&visdn_reload);
	ast_cli_unregister(&no_debug_visdn_q931);
	ast_cli_unregister(&debug_visdn_q931);
	ast_cli_unregister(&no_debug_visdn_q921);
	ast_cli_unregister(&debug_visdn_q921);
	ast_cli_unregister(&no_debug_visdn_generic);
	ast_cli_unregister(&debug_visdn_generic);

	ast_channel_unregister(&visdn_tech);

	q931_leave();

	close(visdn.cxc_control_fd);
	close(visdn.netlink_socket);

	return 0;
}

int reload(void)
{
	visdn_reload_config();

	return 0;
}

int usecount()
{
	int res;
	ast_mutex_lock(&visdn.usecnt_lock);
	res = visdn.usecnt;
	ast_mutex_unlock(&visdn.usecnt_lock);
	return res;
}

char *description()
{
	return VISDN_DESCRIPTION;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
