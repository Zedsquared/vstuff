/*
 * vISDN DSSS-1/q.931 signalling library
 *
 * Copyright (C) 2004-2005 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU Lesser General Public License.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>

#include <lapd.h>

#define Q931_PRIVATE

#include <libq931/lib.h>
#include <libq931/list.h>
#include <libq931/logging.h>
#include <libq931/msgtype.h>
#include <libq931/ie.h>
#include <libq931/out.h>
#include <libq931/ces.h>
#include <libq931/call.h>
#include <libq931/dlc.h>
#include <libq931/intf.h>

q931_callref q931_intf_take_call_reference(struct q931_interface *interface)
{
	assert(interface);

	q931_callref call_reference;
	q931_callref first_call_reference;

	first_call_reference = interface->next_call_reference;

try_again:

	call_reference = interface->next_call_reference;

	interface->next_call_reference++;

	if (interface->next_call_reference >=
	    (1 << ((interface->call_reference_len * 8) - 1)))
		interface->next_call_reference = 1;

	struct q931_call *call;
	list_for_each_entry(call, &interface->calls, calls_node) {
		if (call->direction == Q931_CALL_DIRECTION_OUTBOUND &&
		    call->call_reference == call_reference) {
			if (call_reference == interface->next_call_reference)
				return -1;
			else
				goto try_again;
		}
	}

	return call_reference;
}

struct q931_interface *q931_open_interface(
	struct q931_lib *lib,
	const char *name,
	int flags)
{
	struct q931_interface *intf;

	assert(name);

	intf = malloc(sizeof(*intf));
	if (!intf)
		abort();

	memset(intf, 0x00, sizeof(*intf));

	INIT_LIST_HEAD(&intf->calls);
	INIT_LIST_HEAD(&intf->dlcs);

	intf->lib = lib;
	intf->name = strdup(name);

	int s = socket(PF_LAPD, SOCK_SEQPACKET, 0);
	if (socket < 0)
		goto err_socket;

	intf->flags = flags;

	if (intf->flags & Q931_INTF_FLAGS_DEBUG) {
		int on=1;

		setsockopt(s, SOL_SOCKET, SO_DEBUG,
				&on, sizeof(on));
	}

	if (setsockopt(s, SOL_LAPD, SO_BINDTODEVICE,
			name, strlen(name)+1) < 0)
		goto err_setsockopt;

	int oldflags;
      	if (fcntl(s, F_GETFL, &oldflags) < 0) {
      		report_intf(intf, LOG_ERR, "fcntl: %s\n", strerror(errno));
      		goto err_fcntl_getfl;
      	}

      	if (fcntl(s, F_SETFL, oldflags | O_NONBLOCK) < 0) {
      		report_intf(intf, LOG_ERR, "fcntl: %s\n", strerror(errno));
      		goto err_fcntl_setfl;
      	}

	int optlen=sizeof(intf->role);
	if (getsockopt(s, SOL_LAPD, LAPD_ROLE,
		&intf->role, &optlen)<0)
		goto err_getsockopt;

	if (intf->role == LAPD_ROLE_TE) {
		intf->master_socket = -1;
		intf->bc_dlc.socket = -1;
		intf->bc_dlc.intf = NULL;

		q931_dlc_init(&intf->dlc, intf, s);

		intf->dlc.status = Q931_DLC_STATUS_DISCONNECTED;

		intf->T301 = 180 * 1000000LL;
		intf->T302 =  15 * 1000000LL;
		intf->T303 =   4 * 1000000LL;
		intf->T304 =  30 * 1000000LL;
		intf->T305 =  30 * 1000000LL;
		intf->T308 =   4 * 1000000LL;
		intf->T309 =  90 * 1000000LL;
		intf->T310 =  45 * 1000000LL;
		intf->T313 =   4 * 1000000LL;
		intf->T314 =   4 * 1000000LL;
		intf->T316 = 120 * 1000000LL;
		intf->T317 =  60 * 1000000LL;
		intf->T318 =   4 * 1000000LL;
		intf->T319 =   4 * 1000000LL;
		intf->T321 =  30 * 1000000LL;
		intf->T322 =   4 * 1000000LL;
	} else {
		intf->master_socket = s;

		intf->bc_dlc.socket = s;
		intf->bc_dlc.intf = intf;

		q931_dlc_init(&intf->dlc, NULL, -1);

		intf->T301 = 180 * 1000000LL;
		intf->T302 =  12 * 1000000LL;
		intf->T303 =   4 * 1000000LL;
		intf->T304 =  20 * 1000000LL;
		intf->T305 =  30 * 1000000LL;
		intf->T306 =  30 * 1000000LL;
		intf->T308 =   4 * 1000000LL;
		intf->T309 =  90 * 1000000LL;
		intf->T310 =  35 * 1000000LL;
		intf->T312 = intf->T303 + 2 * 1000000LL;
		intf->T314 =   4 * 1000000LL;
		intf->T316 = 120 * 1000000LL;
		intf->T317 =  60 * 1000000LL;
		intf->T320 =  30 * 1000000LL;
		intf->T321 =  30 * 1000000LL;
		intf->T322 =   4 * 1000000LL;
	}

	// FIXME TODO: Take this from the config
	intf->type = Q931_INTF_TYPE_BRA;
	intf->config = Q931_INTF_CONFIG_MULTIPOINT;

	switch (intf->type) {
	case Q931_INTF_TYPE_BRA:
		intf->n_channels = 2;
		intf->call_reference_len = 1;
	break;
	case Q931_INTF_TYPE_PRA:
		intf->n_channels = 30;
		intf->call_reference_len = 2;
	break;
	}

	intf->next_call_reference = (rand() &
	    ((1 << ((intf->call_reference_len * 8) - 1)) - 2)) + 1;

	int i;
	for (i=0; i<intf->n_channels; i++) {
		intf->channels[i].id = i;
		intf->channels[i].intf = intf;
		intf->channels[i].state = Q931_CHANSTATE_AVAILABLE;
		intf->channels[i].call = NULL;
	}

	intf->global_call.intf = intf;

	list_add(&intf->node, &lib->intfs);

	return intf;

err_getsockopt:
err_fcntl_setfl:
err_fcntl_getfl:
err_setsockopt:
	close(s);
err_socket:
	free(intf);

	return NULL;
}

void q931_close_interface(struct q931_interface *interface)
{
	assert(interface);

	list_del(&interface->node);

	if (interface->role == LAPD_ROLE_TE) {
		shutdown(interface->dlc.socket, 2);
		close(interface->dlc.socket);
	} else {
		struct q931_dlc *dlc, *tpos;
		list_for_each_entry_safe(dlc, tpos,
				&interface->dlcs, intf_node) {

			shutdown(dlc->socket, 2);
			close(dlc->socket);

			list_del(&dlc->intf_node);

			free(dlc);
		}

		// Broadcast socket == master socket but only we know it :)

		close(interface->master_socket);
	}

	if (interface->name)
		free(interface->name);

	free(interface);
}
