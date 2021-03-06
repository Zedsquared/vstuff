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

#ifndef _LIBQ931_IE_BEARER_CAPABILITY_H
#define _LIBQ931_IE_BEARER_CAPABILITY_H

#include <libq931/ie.h>

enum q931_ie_bearer_capability_coding_standard
{
	Q931_IE_BC_CS_CCITT	= 0x0,
	Q931_IE_BC_CS_RESERVED	= 0x1,
	Q931_IE_BC_CS_NATIONAL	= 0x2,
	Q931_IE_BC_CS_SPECIFIC	= 0x3,
};

enum q931_ie_bearer_capability_information_transfer_capability
{
	Q931_IE_BC_ITC_SPEECH				= 0x00,
	Q931_IE_BC_ITC_UNRESTRICTED_DIGITAL		= 0x08,
	Q931_IE_BC_ITC_RESTRICTED_DIGITAL		= 0x09,
	Q931_IE_BC_ITC_3_1_KHZ_AUDIO			= 0x10,
	Q931_IE_BC_ITC_UNRESTRICTED_DIGITAL_WITH_TONES	= 0x11,
	Q931_IE_BC_ITC_VIDEO				= 0x18,
};

enum q931_ie_bearer_capability_transfer_mode
{
	Q931_IE_BC_TM_CIRCUIT	= 0x0,
	Q931_IE_BC_TM_PACKET	= 0x2,
};

enum q931_ie_bearer_capability_information_transfer_rate
{
	Q931_IE_BC_ITR_PACKET	= 0x00,
	Q931_IE_BC_ITR_64	= 0x10,
	Q931_IE_BC_ITR_64_X_2	= 0x11,
	Q931_IE_BC_ITR_384	= 0x13,
	Q931_IE_BC_ITR_1536	= 0x15,
	Q931_IE_BC_ITR_1920	= 0x17,
};

enum q931_ie_bearer_capability_user_information_layer_1_protocol
{
	Q931_IE_BC_UIL1P_V110		= 0x01,
	Q931_IE_BC_UIL1P_G711_ULAW	= 0x02,
	Q931_IE_BC_UIL1P_G711_ALAW	= 0x03,
	Q931_IE_BC_UIL1P_G721		= 0x04,
	Q931_IE_BC_UIL1P_G722		= 0x05,
	Q931_IE_BC_UIL1P_G7XX_VIDEO	= 0x06,
	Q931_IE_BC_UIL1P_NON_CCITT	= 0x07,
	Q931_IE_BC_UIL1P_V120		= 0x08,
	Q931_IE_BC_UIL1P_X31		= 0x09,
	Q931_IE_BC_UIL1P_UNUSED		= -1,
};

enum q931_ie_bearer_capability_synchronous_asynchronous
{
	Q931_IE_BC_SA_SYNCHRONOUS	= 0x0,
	Q931_IE_BC_SA_ASYNCHRONOUS	= 0x1,
};

enum q931_ie_bearer_capability_negotiation
{
	Q931_IE_BC_N_IN_BAND_NEGOTIATION_NOT_POSSIBLE	= 0x0,
	Q931_IE_BC_N_IN_BAND_NEGOTIATION_POSSIBLE	= 0x1,
};

enum q931_ie_bearer_capability_user_rate
{
	Q931_IE_BC_UR_INDICATED_BY_E_BITS	= 0x00,
	Q931_IE_BC_UR_0_6_KBIT			= 0x01,
	Q931_IE_BC_UR_1_2_KBIT			= 0x02,
	Q931_IE_BC_UR_2_4_KBIT			= 0x03,
	Q931_IE_BC_UR_3_6_KBIT			= 0x04,
	Q931_IE_BC_UR_4_8_KBIT			= 0x05,
	Q931_IE_BC_UR_7_2_KBIT			= 0x06,
	Q931_IE_BC_UR_8_KBIT			= 0x07,
	Q931_IE_BC_UR_9_6_KBIT			= 0x08,
	Q931_IE_BC_UR_14_4_KBIT			= 0x09,
	Q931_IE_BC_UR_16_KBIT			= 0x0a,
	Q931_IE_BC_UR_19_2_KBIT			= 0x0b,
	Q931_IE_BC_UR_32_KBIT			= 0x0c,
	Q931_IE_BC_UR_48_KBIT			= 0x0e,
	Q931_IE_BC_UR_56_KBIT			= 0x0f,
	Q931_IE_BC_UR_64_KBIT			= 0x10,
	Q931_IE_BC_UR_0_1345_KBIT		= 0x15,
	Q931_IE_BC_UR_0_100_KBIT		= 0x16,
	Q931_IE_BC_UR_0_075_1_2_KBIT		= 0x17,
	Q931_IE_BC_UR_1_2_0_075_KBIT		= 0x18,
	Q931_IE_BC_UR_0_050_KBIT		= 0x19,
	Q931_IE_BC_UR_0_075_KBIT		= 0x1a,
	Q931_IE_BC_UR_0_110_KBIT		= 0x1b,
	Q931_IE_BC_UR_0_150_KBIT		= 0x1c,
	Q931_IE_BC_UR_0_200_KBIT		= 0x1d,
	Q931_IE_BC_UR_0_300_KBIT		= 0x1e,
	Q931_IE_BC_UR_12_KBIT			= 0x1f
};

enum q931_ie_bearer_capability_intermediate_rate
{
	Q931_IE_BC_IR_NOT_USED		= 0x0,
	Q931_IE_BC_IR_8_KBIT		= 0x1,
	Q931_IE_BC_IR_16_KBIT		= 0x2,
	Q931_IE_BC_IR_32_KBIT		= 0x3
};

enum q931_ie_bearer_capability_network_independent_clock_tx
{
	Q931_IE_BC_NICTX_NOT_REQUIRED	= 0x0,
	Q931_IE_BC_NICTX_REQUIRED	= 0x1
};

enum q931_ie_bearer_capability_network_independent_clock_rx
{
	Q931_IE_BC_NICRX_NOT_REQUIRED	= 0x0,
	Q931_IE_BC_NICRX_REQUIRED	= 0x1
};

enum q931_ie_bearer_capability_flow_control_tx
{
	Q931_IE_BC_FCTX_NOT_REQUIRED	= 0x0,
	Q931_IE_BC_FCTX_REQUIRED	= 0x1
};

enum q931_ie_bearer_capability_flow_control_rx
{
	Q931_IE_BC_FCRX_NOT_REQUIRED	= 0x0,
	Q931_IE_BC_FCRX_REQUIRED	= 0x1
};

enum q931_ie_bearer_capability_rate_adaption_header
{
	Q931_IE_BC_RAH_NOT_INCLUDED	= 0x0,
	Q931_IE_BC_RAH_INCLUDED		= 0x1
};

enum q931_ie_bearer_capability_multiframe_establishment_support
{
	Q931_IE_BC_MES_NOT_SUPPORTED	= 0x0,
	Q931_IE_BC_MES_SUPPORTED	= 0x1
};

enum q931_ie_bearer_capability_mode_of_operation
{
	Q931_IE_BC_MOO_BIT_TRANSPARENT_MODE	= 0x0,
	Q931_IE_BC_MOO_PROTOCOL_SENSITIVE_MODE	= 0x1
};

enum q931_ie_bearer_capability_logical_link_identifier_negotiation
{
	Q931_IE_BC_LLIN_DEFAULT				= 0x0,
	Q931_IE_BC_LLIN_FULL_PROTOCOL_NEGOTIATION	= 0x1
};

enum q931_ie_bearer_capability_assignor_assignee
{
	Q931_IE_BC_AA_ORIGINATOR_IS_DEFAULT_ASSIGNEE	= 0x0,
	Q931_IE_BC_AA_ORIGINATOR_IS_ASSIGNOR_ONLY	= 0x1
};

enum q931_ie_bearer_capability_inband_outband_negotitation
{
	Q931_IE_BC_ION_OUT_OF_BAND	= 0x0,
	Q931_IE_BC_ION_INBAND		= 0x1
};

enum q931_ie_bearer_capability_number_of_stop_bits
{
	Q931_IE_BC_NOSB_NOT_USED	= 0x0,
	Q931_IE_BC_NOSB_1_BIT		= 0x1,
	Q931_IE_BC_NOSB_1_5_BIT		= 0x2,
	Q931_IE_BC_NOSB_2_BITS		= 0x3
};

enum q931_ie_bearer_capability_number_of_data_bits
{
	Q931_IE_BC_NODB_NOT_USED	= 0x0,
	Q931_IE_BC_NODB_5_BITS		= 0x1,
	Q931_IE_BC_NODB_7_BITS		= 0x2,
	Q931_IE_BC_NODB_8_BITS		= 0x3
};

enum q931_ie_bearer_capability_parity_information
{
	Q931_IE_BC_PI_ODD		= 0x0,
	Q931_IE_BC_PI_EVEN		= 0x2,
	Q931_IE_BC_PI_NONE		= 0x3,
	Q931_IE_BC_PI_FORCED_0		= 0x4,
	Q931_IE_BC_PI_FORCED_1		= 0x5
};

enum q931_ie_bearer_capability_duplex_mode
{
	Q931_IE_BC_DM_HALF_DUPLEX	= 0x0,
	Q931_IE_BC_DM_FULL_DUPLEX	= 0x1
};

enum q931_ie_bearer_capability_modem_type
{
	Q931_IE_BC_MT_V_21		= 0x0,
	Q931_IE_BC_MT_V_22		= 0x1,
	Q931_IE_BC_MT_V_22_BIS		= 0x2,
	Q931_IE_BC_MT_V_23		= 0x3,
	Q931_IE_BC_MT_V_26		= 0x4,
	Q931_IE_BC_MT_V_26_BIS		= 0x5,
	Q931_IE_BC_MT_V_26_TER		= 0x6,
	Q931_IE_BC_MT_V_27		= 0x7,
	Q931_IE_BC_MT_V_27_BIS		= 0x8,
	Q931_IE_BC_MT_V_27_TER		= 0x9,
	Q931_IE_BC_MT_V_29		= 0xa,
	Q931_IE_BC_MT_V_32		= 0xb,
	Q931_IE_BC_MT_V_35		= 0xc,
};

enum q931_ie_bearer_capability_user_information_layer_2_protocol
{
	Q931_IE_BC_UIL2P_Q921		= 0x02,
	Q931_IE_BC_UIL2P_X_25		= 0x06,
	Q931_IE_BC_UIL2P_UNUSED		= -1,
};

enum q931_ie_bearer_capability_user_information_layer_3_protocol
{
	Q931_IE_BC_UIL3P_Q931		= 0x02,
	Q931_IE_BC_UIL3P_X_25		= 0x06,
	Q931_IE_BC_UIL3P_UNUSED		= -1,
};

enum q931_ie_bearer_capability_user_information_layer_ident
{
	Q931_IE_BC_LAYER_1_IDENT	= 0x1,
	Q931_IE_BC_LAYER_2_IDENT	= 0x2,
	Q931_IE_BC_LAYER_3_IDENT	= 0x2,
};

struct q931_ie_bearer_capability
{
	struct q931_ie ie;

	enum q931_ie_bearer_capability_coding_standard
		coding_standard;
	enum q931_ie_bearer_capability_information_transfer_capability
		information_transfer_capability;
	enum q931_ie_bearer_capability_transfer_mode
		transfer_mode;
	enum q931_ie_bearer_capability_information_transfer_rate
		information_transfer_rate;
	enum q931_ie_bearer_capability_user_information_layer_1_protocol
		user_information_layer_1_protocol;
	enum q931_ie_bearer_capability_user_information_layer_2_protocol
		user_information_layer_2_protocol;
	enum q931_ie_bearer_capability_user_information_layer_3_protocol
		user_information_layer_3_protocol;
};

struct q931_ie_bearer_capability *q931_ie_bearer_capability_alloc(void);
struct q931_ie *q931_ie_bearer_capability_alloc_abstract(void);

#define Q931_IE_BC_IDENT_MASK 0x60
#define Q931_IE_BC_IDENT_SHIFT 5

static inline enum q931_ie_bearer_capability_user_information_layer_ident
	q931_ie_bearer_capability_oct_ident(__u8 oct)
{
	return (oct & Q931_IE_BC_IDENT_MASK) >> Q931_IE_BC_IDENT_SHIFT;
}

int q931_ie_bearer_capability_read_from_buf(
	struct q931_ie *abstract_ie,
	void *buf,
	int len,
	void (*report_func)(int level, const char *format, ...),
	struct q931_interface *intf);

int q931_ie_bearer_capability_write_to_buf(
	const struct q931_ie *ie,
	void *buf,
	int max_size);

void q931_ie_bearer_capability_dump(
	const struct q931_ie *ie,
	void (*report)(int level, const char *format, ...),
	const char *prefix);


#ifdef Q931_PRIVATE

struct q931_ie_bearer_capability_onwire_3
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 coding_standard:2;
	__u8 information_transfer_capability:5;
#else
	__u8 information_transfer_capability:5;
	__u8 coding_standard:2;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_4
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 transfer_mode:2;
	__u8 information_transfer_rate:5;
#else
	__u8 information_transfer_rate:5;
	__u8 transfer_mode:2;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_4a
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 structure:3;
	__u8 configuration:2;
	__u8 establishment:2;
#else
	__u8 establishment:2;
	__u8 configuration:2;
	__u8 structure:3;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_4b
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 symmetry:2;
	__u8 information_transfer_rate_dst_to_orig:5;
#else
	__u8 information_transfer_rate_dst_to_orig:5;
	__u8 symmetry:2;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_5
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 layer_1_ident:2;
	__u8 user_information_layer_1_protocol:5;
#else
	__u8 user_information_layer_1_protocol:5;
	__u8 layer_1_ident:2;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_5a
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 sync_async:1;
	__u8 negotiation:1;
	__u8 user_rate:5;
#else
	__u8 user_rate:5;
	__u8 negotiation:1;
	__u8 sync_async:1;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_5b1
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 intermediate_rate:2;
	__u8 nic_on_tx:1;
	__u8 nic_on_rx:1;
	__u8 flowcontrol_on_tx:1;
	__u8 flowcontrol_on_rx:1;
	__u8 :1;
#else
	__u8 :1;
	__u8 flowcontrol_on_rx:1;
	__u8 flowcontrol_on_tx:1;
	__u8 nic_on_rx:1;
	__u8 nic_on_tx:1;
	__u8 intermediate_rate:2;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_5b2
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 hdr_nohdr:1;
	__u8 multiframe_support:1;
	__u8 mode:1;
	__u8 lli_negotiation:1;
	__u8 assignor_assignee:1;
	__u8 inband_outband_nego:1;
	__u8 :1;
#else
	__u8 :1;
	__u8 inband_outband_nego:1;
	__u8 assignor_assignee:1;
	__u8 lli_negotiation:1;
	__u8 mode:1;
	__u8 multiframe_support:1;
	__u8 hdr_nohdr:1;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_5c
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 number_of_stop_bits:2;
	__u8 number_of_data_bits:2;
	__u8 parity:3;
#else
	__u8 parity:3;
	__u8 number_of_data_bits:2;
	__u8 number_of_stop_bits:2;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_5d
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 duplex_mode:1;
	__u8 modem_type:6;
#else
	__u8 modem_type:6;
	__u8 duplex_mode:1;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_6
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 layer_2_ident:2;
	__u8 user_information_layer_2_protocol:5;
#else
	__u8 user_information_layer_2_protocol:5;
	__u8 layer_2_ident:2;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

struct q931_ie_bearer_capability_onwire_7
{
	union { struct {
#if __BYTE_ORDER == __BIG_ENDIAN
	__u8 ext:1;
	__u8 layer_3_ident:2;
	__u8 user_information_layer_3_protocol:5;
#else
	__u8 user_information_layer_3_protocol:5;
	__u8 layer_3_ident:2;
	__u8 ext:1;
#endif
	}; __u8 raw; };
} __attribute__ ((__packed__));

void q931_ie_bearer_capability_register(
	const struct q931_ie_class *ie_class);

#endif
#endif
