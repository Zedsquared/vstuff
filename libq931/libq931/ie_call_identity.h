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

#ifndef _LIBQ931_IE_CALL_IDENTITY_H
#define _LIBQ931_IE_CALL_IDENTITY_H

#include <libq931/ie.h>

/************************* Call Identity ***************************/

struct q931_ie_call_identity
{
	struct q931_ie ie;

	__u8 data[8];
	int data_len;
};

struct q931_ie_call_identity *q931_ie_call_identity_alloc(void);
struct q931_ie *q931_ie_call_identity_alloc_abstract(void);

int q931_ie_call_identity_read_from_buf(
	struct q931_ie *abstract_ie,
	void *buf,
	int len,
	void (*report_func)(int level, const char *format, ...),
	struct q931_interface *intf);

int q931_ie_call_identity_write_to_buf(
	const struct q931_ie *generic_ie,
	void *buf,
	int max_size);

void q931_ie_call_identity_dump(
	const struct q931_ie *ie,
	void (*report)(int level, const char *format, ...),
	const char *prefix);

#ifdef Q931_PRIVATE

void q931_ie_call_identity_register(
	const struct q931_ie_class *ie_class);

#endif

#endif
