/*
 * Copyright (C) 2018-2019 Jolla Ltd.
 * Copyright (C) 2018-2019 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef NCI_CORE_H
#define NCI_CORE_H

#include <nci_types.h>

#include <glib-object.h>

G_BEGIN_DECLS

/* NCI state machine */
typedef enum nci_state {
    NCI_STATE_INIT,
    NCI_STATE_ERROR,
    NCI_STATE_STOP,
    /* RFST states are taken from the NCI spec */
    NCI_RFST_IDLE,
    NCI_RFST_DISCOVERY,
    NCI_RFST_W4_ALL_DISCOVERIES,
    NCI_RFST_W4_HOST_SELECT,
    NCI_RFST_POLL_ACTIVE,
    NCI_RFST_LISTEN_ACTIVE,
    NCI_RFST_LISTEN_SLEEP
} NCI_STATE;

/* Table 4: Conn ID
 * 0: Static RF Connection between the DH and a Remote NFC Endpoint */
#define NCI_STATIC_RF_CONN_ID (0x00)

/* Table 94: Status Codes */
typedef enum nci_status {
    NCI_STATUS_OK = 0x00,
    NCI_STATUS_REJECTED = 0x01,
    NCI_STATUS_RF_FRAME_CORRUPTED = 0x02,
    NCI_STATUS_FAILED = 0x03,
    NCI_STATUS_NOT_INITIALIZED = 0x04,
    NCI_STATUS_SYNTAX_ERROR = 0x05,
    NCI_STATUS_SEMANTIC_ERROR = 0x06,
    NCI_STATUS_INVALID_PARAM = 0x09,
    NCI_STATUS_MESSAGE_SIZE_EXCEEDED = 0x0A,
    NCI_STATUS_DISCOVERY_ALREADY_STARTED = 0xA0,
    NCI_STATUS_DISCOVERY_TARGET_ACTIVATION_FAILED = 0xA1,
    NCI_STATUS_DISCOVERY_TEAR_DOWN = 0xA2,
    NCI_STATUS_RF_TRANSMISSION_ERROR = 0xB0,
    NCI_STATUS_RF_PROTOCOL_ERROR = 0xB1,
    NCI_STATUS_RF_TIMEOUT_ERROR = 0xB2,
    NCI_STATUS_NFCEE_INTERFACE_ACTIVATION_FAILED = 0xC0,
    NCI_STATUS_NFCEE_TRANSMISSION_ERROR = 0xC1,
    NCI_STATUS_NFCEE_PROTOCOL_ERROR = 0xC2,
    NCI_STATUS_NFCEE_TIMEOUT_ERROR = 0xC3
} NCI_STATUS;

/* Table 96: RF Technology and Mode */
typedef enum nci_mode {
    NCI_MODE_PASSIVE_POLL_A = 0x00,
    NCI_MODE_PASSIVE_POLL_B = 0x01,
    NCI_MODE_PASSIVE_POLL_F = 0x02,
    NCI_MODE_ACTIVE_POLL_A = 0x03,
    NCI_MODE_ACTIVE_POLL_F = 0x05,
    NCI_MODE_PASSIVE_POLL_15693 = 0x06,
    NCI_MODE_PASSIVE_LISTEN_A = 0x80,
    NCI_MODE_PASSIVE_LISTEN_B = 0x81,
    NCI_MODE_PASSIVE_LISTEN_F = 0x82,
    NCI_MODE_ACTIVE_LISTEN_A = 0x83,
    NCI_MODE_ACTIVE_LISTEN_F = 0x85,
    NCI_MODE_PASSIVE_LISTEN_15693 = 0x86
} NCI_MODE;

/* Table 97: Bit Rates */
typedef enum nci_bit_rate {
    NFC_BIT_RATE_106 = 0x00, /* 106 Kbit/s */
    NFC_BIT_RATE_212 = 0x01, /* 212 Kbit/s */
    NFC_BIT_RATE_424 = 0x02, /* 424 Kbit/s */
    NFC_BIT_RATE_848 = 0x03, /* 848 Kbit/s */
    NFC_BIT_RATE_1695 = 0x04, /* 1695 Kbit/s */
    NFC_BIT_RATE_3390 = 0x05, /* 3390 Kbit/s */
    NFC_BIT_RATE_6780 = 0x06  /* 6780 Kbit/s */
} NFC_BIT_RATE;

/* Table 98: RF Protocols */
typedef enum nci_protocol {
    NCI_PROTOCOL_UNDETERMINED = 0x00,
    NCI_PROTOCOL_T1T = 0x01,
    NCI_PROTOCOL_T2T = 0x02,
    NCI_PROTOCOL_T3T = 0x03,
    NCI_PROTOCOL_ISO_DEP = 0x04,
    NCI_PROTOCOL_NFC_DEP = 0x05,
} NCI_PROTOCOL;

/* Table 99: RF Interfaces */
typedef enum nci_rf_interface {
    NCI_RF_INTERFACE_NFCEE_DIRECT = 0x00,
    NCI_RF_INTERFACE_FRAME = 0x01,
    NCI_RF_INTERFACE_ISO_DEP = 0x02,
    NCI_RF_INTERFACE_NFC_DEP = 0x03
} NCI_RF_INTERFACE;

/* See Table 54: Specific Parameters for NFC-A Poll Mode */
typedef struct nci_mode_param_poll_a {
    guint8 sens_res[2];
    guint8 nfcid1_len;
    guint8 sel_res_len;
    guint8 sel_res;
    const guint8* nfcid1;
} NciModeParamPollA;

typedef union nci_mode_param {
    NciModeParamPollA poll_a;
} NciModeParam;

/* Table 76: Activation Parameters for NFC-A/ISO-DEP Poll Mode */
typedef struct nci_activation_param_iso_dep_poll_a {
    guint fsc;     /* FSC (FSCI converted to bytes) */
    GUtilData t1;  /* T1 to Tk (otherwise called historical bytes) */
} NciActivationParamIsoDepPollA;

typedef union nci_activation_param {
    NciActivationParamIsoDepPollA iso_dep_poll_a;
} NciActivationParam;

/* See Table 61: Notification for RF Interface activation */
struct nci_intf_activated_ntf {
    guint8 discovery_id;
    NCI_RF_INTERFACE rf_intf;
    NCI_PROTOCOL protocol;
    NCI_MODE mode;
    guint8 max_data_packet_size;
    guint8 num_credits;
    guint8 mode_param_len;
    const void* mode_param_bytes;
    const NciModeParam* mode_param;
    NCI_MODE data_exchange_mode;
    NFC_BIT_RATE transmit_rate;
    NFC_BIT_RATE receive_rate;
    guint8 activation_param_len;
    const void* activation_param_bytes;
    const NciActivationParam* activation_param;
};

/*
 * If current_state != next_state, the state machine is transitioning
 * from one state to another. That may take a while.
 */

typedef struct nci_core NciCore;
typedef struct nci_core_priv NciCorePriv;

struct nci_core {
    GObject object;
    NciCorePriv* priv;
    NCI_STATE current_state;
    NCI_STATE next_state;
    guint cmd_timeout;
};

typedef
void
(*NciCoreFunc)(
    NciCore* nci,
    void* user_data);

typedef
void
(*NciCoreSendFunc)(
    NciCore* nci,
    gboolean success,
    void* user_data);

typedef
void
(*NciCoreDataPacketFunc)(
    NciCore* nci,
    guint8 cid,
    const void* payload,
    guint len,
    void* user_data);

typedef
void
(*NciCoreIntfActivationFunc)(
    NciCore* nci,
    const NciIntfActivationNtf* ntf,
    void* user_data);

NciCore*
nci_core_new(
    NciHalIo* io);

void
nci_core_free(
    NciCore* nci);

void
nci_core_restart(
    NciCore* nci);

gboolean
nci_core_set_state(
    NciCore* nci,
    NCI_STATE state);

void
nci_core_stall(
    NciCore* nci,
    gboolean error);

guint
nci_core_send_data_msg(
    NciCore* nci,
    guint8 cid,
    GBytes* payload,
    NciCoreSendFunc complete,
    GDestroyNotify destroy,
    void* user_data);

void
nci_core_cancel(
    NciCore* nci,
    guint id);

gulong
nci_core_add_current_state_changed_handler(
    NciCore* nci,
    NciCoreFunc func,
    void* user_data);

gulong
nci_core_add_next_state_changed_handler(
    NciCore* nci,
    NciCoreFunc func,
    void* user_data);

gulong
nci_core_add_intf_activated_handler(
    NciCore* nci,
    NciCoreIntfActivationFunc func,
    void* user_data);

gulong
nci_core_add_data_packet_handler(
    NciCore* nci,
    NciCoreDataPacketFunc func,
    void* user_data);

void
nci_core_remove_handler(
    NciCore* nci,
    gulong id);

void
nci_core_remove_handlers(
    NciCore* nci,
    gulong* ids,
    guint count);

#define nci_core_remove_all_handlers(core,ids) \
    nci_core_remove_handlers(core, ids, G_N_ELEMENTS(ids))

G_END_DECLS

#endif /* NCI_CORE_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
