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

#include "nci_core.h"
#include "nci_sar.h"
#include "nci_log.h"

#include <gutil_idlepool.h>
#include <gutil_misc.h>
#include <gutil_macros.h>

GLOG_MODULE_DEFINE("nci");

typedef struct nci_core_state_transition NciCoreStateTransition;

typedef enum nci_interface_version {
    NCI_INTERFACE_VERSION_UNKNOWN,
    NCI_INTERFACE_VERSION_1,
    NCI_INTERFACE_VERSION_2
} NCI_INTERFACE_VERSION;

/* Table 9: NFCC Features */
typedef enum nci_nfcc_discovery {
    NCI_NFCC_DISCOVERY_NONE = 0,
    NCI_NFCC_DISCOVERY_FREQUENCY_CONFIG = 0x01,
    NCI_NFCC_DISCOVERY_RF_CONFIG_MERGE = 0x02
} NCI_NFCC_DISCOVERY;

typedef enum nci_nfcc_routing {
    NCI_NFCC_ROUTING_NONE = 0,
    NCI_NFCC_ROUTING_TECHNOLOGY_BASED = 0x02,
    NCI_NFCC_ROUTING_PROTOCOL_BASED = 0x04,
    NCI_NFCC_ROUTING_AID_BASED = 0x08
} NCI_NFCC_ROUTING;

typedef enum nci_nfcc_power {
    NCI_NFCC_POWER_NONE = 0,
    NCI_NFCC_POWER_BATTERY_OFF = 0x01,
    NCI_NFCC_POWER_SWITCH_OFF = 0x02
} NCI_NFCC_POWER;

typedef
void
(*NciCoreTransitionFunc)(
    NciCore* self);

typedef
const NciCoreStateTransition**
(*NciCoreTransitionPathFunc)(
    NciCore* self,
    NCI_STATE to);

typedef
void
(*NciCoreControlPacketHandler)(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint payload_len);

typedef struct nci_core_send_data {
    NciCoreSendFunc complete;
    GDestroyNotify destroy;
    gpointer user_data;
} NciCoreSendData;

typedef struct nci_core_state {
    NCI_STATE state;
    NciCoreControlPacketHandler notification_handler;
    NciCoreTransitionPathFunc transition_path;
} NciCoreState;

struct nci_core_state_transition {
    const NciCoreState* destination;
    NciCoreTransitionFunc start;
    NciCoreControlPacketHandler notification_handler;
};

struct nci_core_priv {
    NciSarClient sar_client;
    GUtilIdlePool* pool;
    NciSar* sar;
    NciCore* self;
    GBytes* rf_interfaces;
    guint32 pending_signals;
    guint cmd_id;
    guint cmd_timeout_id;
    guint8 rsp_gid;
    guint8 rsp_oid;
    NciCoreControlPacketHandler rsp_handler;
    const NciCoreState* last_state;
    const NciCoreStateTransition* transition;
    GPtrArray* next_transitions;
    guint max_routing_table_size;
    NCI_INTERFACE_VERSION version;
    NCI_NFCC_DISCOVERY nfcc_discovery;
    NCI_NFCC_ROUTING nfcc_routing;
    NCI_NFCC_POWER nfcc_power;
};

typedef GObjectClass NciCoreClass;
G_DEFINE_TYPE(NciCore, nci_core, G_TYPE_OBJECT)
#define NCI_TYPE_CORE (nci_core_get_type())
#define NCI_CORE(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        NCI_TYPE_CORE, NciCore))

enum nci_core_signal {
    SIGNAL_CURRENT_STATE,
    SIGNAL_NEXT_STATE,
    SIGNAL_INTF_ACTIVATED,
    SIGNAL_DATA_PACKET,
    SIGNAL_COUNT
};

#define SIGNAL_CURRENT_STATE_NAME   "nci-core-current-state"
#define SIGNAL_NEXT_STATE_NAME      "nci-core-next-state"
#define SIGNAL_INTF_ACTIVATED_NAME  "nci-core-intf-activated"
#define SIGNAL_DATA_PACKET_NAME     "nci-core-data-packet"

static guint nci_core_signals[SIGNAL_COUNT] = { 0 };

static const char* nci_core_state_names[] = {
    "INIT",
    "ERROR",
    "STOP",
    "RFST_IDLE",
    "RFST_DISCOVERY",
    "RFST_W4_ALL_DISCOVERIES",
    "RFST_W4_HOST_SELECT",
    "RFST_POLL_ACTIVE",
    "RFST_LISTEN_ACTIVE",
    "RFST_LISTEN_SLEEP"
};

#define DIR_IN  '>'
#define DIR_OUT '<'

#define DEFAULT_TIMEOUT (2000) /* msec */

/* GID values */
#define NCI_GID_CORE            (0x00)
#define NCI_GID_RF              (0x01)
#define NCI_GID_NFCEE           (0x02)

/* OID values */
#define NCI_OID_CORE_RESET                      (0x00)
#define NCI_OID_CORE_INIT                       (0x01)
#define NCI_OID_CORE_SET_CONFIG                 (0x02)
#define NCI_OID_CORE_GET_CONFIG                 (0x03)
#define NCI_OID_CORE_CONN_CREATE                (0x04)
#define NCI_OID_CORE_CONN_CLOSE                 (0x05)
#define NCI_OID_CORE_CONN_CREDITS               (0x06)
#define NCI_OID_CORE_GENERIC_ERROR              (0x07)
#define NCI_OID_CORE_INTERFACE_ERROR            (0x08)

#define NCI_OID_RF_DISCOVER_MAP                 (0x00)
#define NCI_OID_RF_SET_LISTEN_MODE_ROUTING      (0x01)
#define NCI_OID_RF_GET_LISTEN_MODE_ROUTING      (0x02)
#define NCI_OID_RF_DISCOVER                     (0x03)
#define NCI_OID_RF_DISCOVER_SELECT              (0x04)
#define NCI_OID_RF_INTF_ACTIVATED               (0x05)
#define NCI_OID_RF_DEACTIVATE                   (0x06)
#define NCI_OID_RF_FIELD_INFO                   (0x07)
#define NCI_OID_RF_T3T_POLLING                  (0x08)
#define NCI_OID_RF_NFCEE_ACTION                 (0x09)
#define NCI_OID_RF_NFCEE_DISCOVERY_REQ          (0x0a)
#define NCI_OID_RF_PARAMETER_UPDATE             (0x0b)

#define NCI_STATUS_OK                           (0x00)
#define NCI_STATUS_REJECTED                     (0x01)
#define NCI_STATUS_RF_FRAME_CORRUPTED           (0x02)
#define NCI_STATUS_FAILED                       (0x03)
#define NCI_STATUS_NOT_INITIALIZED              (0x04)
#define NCI_STATUS_SYNTAX_ERROR                 (0x05)
#define NCI_STATUS_SEMANTIC_ERROR               (0x06)
#define NCI_STATUS_INVALID_PARAM                (0x09)
#define NCI_STATUS_MESSAGE_SIZE_EXCEEDED        (0x0a)
#define NCI_DISCOVERY_ALREADY_STARTED           (0xa0)
#define NCI_DISCOVERY_TARGET_ACTIVATION_FAILED  (0xa1)
#define NCI_DISCOVERY_TEAR_DOWN                 (0xa2)
#define NCI_RF_TRANSMISSION_ERROR               (0xb0)
#define NCI_RF_PROTOCOL_ERROR                   (0xb1)
#define NCI_RF_TIMEOUT_ERROR                    (0xb2)
#define NCI_NFCEE_INTERFACE_ACTIVATION_FAILED   (0xc0)
#define NCI_NFCEE_TRANSMISSION_ERROR            (0xc1)
#define NCI_NFCEE_PROTOCOL_ERROR                (0xc2)
#define NCI_NFCEE_TIMEOUT_ERROR                 (0xc3)

/* Table 43: Value Field for Mode */
#define NCI_DISCOVER_MAP_MODE_POLL   (0x01)
#define NCI_DISCOVER_MAP_MODE_LISTEN (0x02)

/* Table 46: TLV Coding for Listen Mode Routing */
#define NCI_ROUTING_ENTRY_TYPE_TECHNOLOGY (0x00)
#define NCI_ROUTING_ENTRY_TYPE_PROTOCOL (0x01)
#define NCI_ROUTING_ENTRY_TYPE_AID (0x02)

/* Table 50: Value Field for Power State */
#define NCI_ROUTING_ENTRY_POWER_ON (0x01)
#define NCI_ROUTING_ENTRY_POWER_OFF (0x02)
#define NCI_ROUTING_ENTRY_POWER_BATTERY_OFF (0x04)
#define NCI_ROUTING_ENTRY_POWER_ALL (0x07)

/* Table 63: Deactivation Types */
typedef enum nci_deactivation_type {
    NCI_DEACTIVATE_TYPE_IDLE = 0x00,
    NCI_DEACTIVATE_TYPE_SLEEP = 0x01,
    NCI_DEACTIVATE_TYPE_SLEEP_AF = 0x02,
    NCI_DEACTIVATE_TYPE_DISCOVERY = 0x03
} NCI_DEACTIVATION_TYPE;

/* Table 64: Deactivation Reasons */
typedef enum nci_deactivation_reason {
    NCI_DEACTIVATION_REASON_DH_REQUEST = 0x00,
    NCI_DEACTIVATION_REASON_ENDPOINT_REQUEST = 0x01,
    NCI_DEACTIVATION_REASON_RF_LINK_LOSS = 0x02,
    NCI_DEACTIVATION_REASON_BAD_AFI = 0x03
} NCI_DEACTIVATION_REASON;

/* Table 84: NFCEE IDs */
#define NCI_NFCEE_ID_DH (0x00)

/* Table 95: RF Technologies */
typedef enum nci_rf_technology {
    NCI_RF_TECHNOLOGY_A = 0x00,
    NCI_RF_TECHNOLOGY_B = 0x01,
    NCI_RF_TECHNOLOGY_F = 0x02,
    NCI_RF_TECHNOLOGY_15693 = 0x03
} NCI_RF_TECHNOLOGY;

/* Table 101: Configuration Parameter Tags */
/* ==== Common Discovery Parameters ==== */
#define NCI_CONFIG_TOTAL_DURATION               (0x00)
#define NCI_CONFIG_CON_DEVICES_LIMIT            (0x01)
/* ==== Poll Mode: NFC-A Discovery Parameters ==== */
#define NCI_CONFIG_PA_BAIL_OUT                  (0x08)
/* ==== Poll Mode: NFC-B Discovery Parameters ==== */
#define NCI_CONFIG_PB_AFI                       (0x10)
#define NCI_CONFIG_PB_BAIL_OUT                  (0x11)
#define NCI_CONFIG_PB_ATTRIB_PARAM1             (0x12)
#define NCI_CONFIG_PB_SENSB_REQ_PARAM           (0x13)
/* ==== Poll Mode: NFC-F Discovery Parameters ==== */
#define NCI_CONFIG_PF_BIT_RATE                  (0x18)
#define NCI_CONFIG_PF_RC_CODE                   (0x19)
/* ==== Poll Mode: ISO-DEP Discovery Parameters ==== */
#define NCI_CONFIG_PB_H_INFO                    (0x20)
#define NCI_CONFIG_PI_BIT_RATE                  (0x21)
#define NCI_CONFIG_PA_ADV_FEAT                  (0x22)
/* ==== Poll Mode: NFC-DEP Discovery Parameters ==== */
#define NCI_CONFIG_PN_NFC_DEP_SPEED             (0x28)
#define NCI_CONFIG_PN_ATR_REQ_GEN_BYTES         (0x29)
#define NCI_CONFIG_PN_ATR_REQ_CONFIG            (0x2A)
/* ==== Listen Mode: NFC-A Discovery Parameters ==== */
#define NCI_CONFIG_LA_BIT_FRAME_SDD             (0x30)
#define NCI_CONFIG_LA_PLATFORM_CONFIG           (0x31)
#define NCI_CONFIG_LA_SEL_INFO                  (0x32)
#define NCI_CONFIG_LA_NFCID1                    (0x33)
/* ==== Listen Mode: NFC-B Discovery Parameters ==== */
#define NCI_CONFIG_LB_SENSB_INFO                (0x38)
#define NCI_CONFIG_LB_NFCID0                    (0x39)
#define NCI_CONFIG_LB_APPLICATION_DATA          (0x3A)
#define NCI_CONFIG_LB_SFGI                      (0x3B)
#define NCI_CONFIG_LB_ADC_FO                    (0x3C)
/* ==== Listen Mode: NFC-F Discovery Parameters ==== */
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_1         (0x40)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_2         (0x41)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_3         (0x42)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_4         (0x43)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_5         (0x44)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_6         (0x45)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_7         (0x46)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_8         (0x47)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_9         (0x48)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_10        (0x49)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_11        (0x4A)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_12        (0x4B)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_13        (0x4C)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_14        (0x4D)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_15        (0x4E)
#define NCI_CONFIG_LF_T3T_IDENTIFIERS_16        (0x4F)
#define NCI_CONFIG_LF_PROTOCOL_TYPE             (0x50)
#define NCI_CONFIG_LF_T3T_PMM                   (0x51)
#define NCI_CONFIG_LF_T3T_MAX                   (0x52)
#define NCI_CONFIG_LF_T3T_FLAGS                 (0x53)
#define NCI_CONFIG_LF_CON_BITR_F                (0x54)
#define NCI_CONFIG_LF_ADV_FEAT                  (0x55)
/* ==== Listen Mode: ISO-DEP Discovery Parameters ==== */
#define NCI_CONFIG_LI_FWI                       (0x58)
#define NCI_CONFIG_LA_HIST_BY                   (0x59)
#define NCI_CONFIG_LB_H_INFO_RESP               (0x5A)
#define NCI_CONFIG_LI_BIT_RATE                  (0x5B)
/* ==== Listen Mode: NFC-DEP Discovery Parameters ==== */
#define NCI_CONFIG_LN_WT                        (0x60)
#define NCI_CONFIG_LN_ATR_RES_GEN_BYTES         (0x61)
#define NCI_CONFIG_LN_ATR_RES_CONFIG            (0x62)
#define NCI_CONFIG_RF_FIELD_INFO                (0x80)
#define NCI_CONFIG_RF_NFCEE_ACTION              (0x81)
#define NCI_CONFIG_NFCDEP_OP                    (0x82)

static const NciCoreState nci_core_state_idle;
static const NciCoreState nci_core_state_discovery;
static const NciCoreState nci_core_state_poll_active;

static const NciCoreStateTransition nci_core_transition_poll_to_idle;
static const NciCoreStateTransition nci_core_transition_poll_to_discovery;
static const NciCoreStateTransition nci_core_transition_discovery_to_idle;
static const NciCoreStateTransition nci_core_transition_idle_to_discovery;
static const NciCoreStateTransition nci_core_transition_to_idle;

static
NciCorePriv*
nci_core_priv_from_sar_client(
    NciSarClient* sar_client)
{
    return G_CAST(sar_client, NciCorePriv, sar_client);
}

static
inline
void
nci_core_queue_signal(
    NciCore* self,
    int sig)
{
    NciCorePriv* priv = self->priv;

    priv->pending_signals |= (1 << sig);
}

static
void
nci_core_emit_pending_signals(
    NciCore* self)
{
    NciCorePriv* priv = self->priv;

    if (priv->pending_signals) {
        int sig;

        /* Handlers could drops their references to us */
        g_object_ref(self);
        for (sig = 0; priv->pending_signals && sig < SIGNAL_COUNT; sig++) {
            const guint32 signal_bit = (1 << sig);
            if (priv->pending_signals & signal_bit) {
                priv->pending_signals &= ~signal_bit;
                g_signal_emit(self, nci_core_signals[sig], 0);
            }
        }
        /* And release the temporary reference */
        g_object_unref(self);
    }
}

static
const char*
nci_core_state_name(
    guint state)
{
    if (state < G_N_ELEMENTS(nci_core_state_names)) {
        return nci_core_state_names[state];
    } else {
        return "????";
    }
}

static
void
nci_core_set_current_state(
    NciCore* self,
    NCI_STATE state)
{
    if (self->current_state != state) {
        GDEBUG("Current state %s -> %s",
           nci_core_state_name(self->current_state),
           nci_core_state_name(state));
        self->current_state = state;
        nci_core_queue_signal(self, SIGNAL_CURRENT_STATE);
    }
}

static
void
nci_core_set_next_state(
    NciCore* self,
    NCI_STATE state)
{
    if (self->next_state != state) {
        GDEBUG("Next state %s -> %s",
           nci_core_state_name(self->next_state),
           nci_core_state_name(state));
        self->next_state = state;
        nci_core_queue_signal(self, SIGNAL_NEXT_STATE);
    }
}

static
void
nci_core_cancel_command(
    NciCore* self)
{
    NciCorePriv* priv = self->priv;

    if (priv->cmd_id) {
        nci_sar_cancel(priv->sar, priv->cmd_id);
        priv->cmd_id = 0;
        priv->rsp_handler = NULL;
    }
}

static
GBytes* /* autoreleased */
nci_core_static_bytes(
    NciCore* self,
    gconstpointer data,
    gsize size)
{
    NciCorePriv* priv = self->priv;
    GBytes* bytes = g_bytes_new_static(data, size);

    gutil_idle_pool_add_bytes(priv->pool, bytes);
    return bytes;
}

static
void
nci_core_generic_command_completion(
    NciSarClient* sar_client,
    gboolean success,
    gpointer user_data)
{
    NciCore* self = NCI_CORE(user_data);

    if (!success) {
        GWARN("Command failed");
        nci_core_stall(self, TRUE);
    }
}

static
gboolean
nci_core_command_timeout(
    gpointer user_data)
{
    NciCore* self = NCI_CORE(user_data);
    NciCorePriv* priv = self->priv;

    GWARN("Command %02x/%02x timed out", priv->rsp_gid, priv->rsp_oid);
    priv->cmd_timeout_id = 0;
    nci_core_stall(self, TRUE);
    return G_SOURCE_REMOVE;
}

static
void
nci_core_send_command(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    GBytes* payload,
    NciSarCompletionFunc complete,
    NciCoreControlPacketHandler resp)
{
    NciCorePriv* priv = self->priv;

    /* Cancel the previous one, if any */
    nci_core_cancel_command(self);

    priv->rsp_gid = gid;
    priv->rsp_oid = oid;
    priv->rsp_handler = resp;
    priv->cmd_id = nci_sar_send_command(priv->sar, gid, oid, payload,
        complete, NULL, self);
    if (!priv->cmd_id) {
        nci_core_stall(self, TRUE);
    } else {
        if (priv->cmd_timeout_id) {
            g_source_remove(priv->cmd_timeout_id);
            priv->cmd_timeout_id = 0;
        }
        if (self->cmd_timeout) {
            priv->cmd_timeout_id = g_timeout_add(self->cmd_timeout,
                nci_core_command_timeout, self);
        }
    }
}

static
void
nci_core_deactivate_to_idle(
    NciCore* self,
    NciCoreControlPacketHandler resp)
{
    static const guint8 cmd_data[] = { NCI_DEACTIVATE_TYPE_IDLE };

    GDEBUG("%c RF_DEACTIVATE_CMD (Idle)", DIR_OUT);
    nci_core_send_command(self, NCI_GID_RF, NCI_OID_RF_DEACTIVATE,
        nci_core_static_bytes(self, cmd_data, sizeof(cmd_data)),
        nci_core_generic_command_completion, resp);
}

static
void
nci_core_deactivate_to_discovery(
    NciCore* self,
    NciCoreControlPacketHandler resp)
{
    static const guint8 cmd_data[] = { NCI_DEACTIVATE_TYPE_DISCOVERY };

    GDEBUG("%c RF_DEACTIVATE_CMD (Discovery)", DIR_OUT);
    nci_core_send_command(self, NCI_GID_RF, NCI_OID_RF_DEACTIVATE,
        nci_core_static_bytes(self, cmd_data, sizeof(cmd_data)),
        nci_core_generic_command_completion, resp);
}

static
void
nci_core_send_data_msg_complete(
    NciSarClient* client,
    gboolean ok,
    gpointer user_data)
{
    NciCoreSendData* send = user_data;
    NciCorePriv* priv = nci_core_priv_from_sar_client(client);

    if (send->complete) {
        send->complete(priv->self, ok, send->user_data);
    }
}

static
void
nci_core_send_data_msg_destroy(
    gpointer data)
{
    NciCoreSendData* send = data;

    if (send->destroy) {
        send->destroy(send->user_data);
    }
    g_slice_free(NciCoreSendData, send);
}

static
void
nci_core_transition_start(
    NciCore* self,
    const NciCoreStateTransition* transition)
{
    NciCorePriv* priv = self->priv;

    priv->transition = transition;
    nci_core_set_next_state(self, transition->destination->state);
    transition->start(self);
    nci_core_emit_pending_signals(self);
}

static
void
nci_core_transition_enter_state(
    NciCore* self,
    const NciCoreState* destination)
{
    if (destination) {
        NciCorePriv* priv = self->priv;

        priv->last_state = destination;
        nci_core_set_next_state(self, destination->state);
        nci_core_set_current_state(self, destination->state);
    }
}

static
void
nci_core_transition_finish(
    NciCore* self,
    const NciCoreState* destination)
{
    if (destination) {
        NciCorePriv* priv = self->priv;

        GASSERT(!priv->cmd_id);
        nci_core_transition_enter_state(self, destination);
        if (priv->next_transitions->len > 0) {
            const NciCoreStateTransition* next =
                priv->next_transitions->pdata[0];

            g_ptr_array_remove_index(priv->next_transitions, 0);
            nci_core_transition_start(self, next);
        } else {
            priv->transition = NULL;
            nci_core_emit_pending_signals(self);
        }
    }
}

static
void
nci_core_ignore_ntf(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    GDEBUG("Notification 0x%02x/0x%02x is ignored in %s state", gid, oid,
        nci_core_state_name(self->priv->last_state->state));
}

static
void
nci_core_generic_error_ntf(
    NciCore* self,
    const guint8* payload,
    guint len)
{
    if (len == 1) {
        GWARN("Generic error 0x%02x", payload[0]);
    } else {
        GWARN("Failed to parse CORE_GENERIC_ERROR_NTF");
    }
}

static
const NciCoreState*
nci_core_parse_rf_deactivate_ntf(
    NciCore* self,
    const guint8* payload,
    guint len)
{
    if (len == 2) {
        switch (payload[0]) {
        case NCI_DEACTIVATE_TYPE_IDLE:
            GDEBUG("RF_DEACTIVATE_NTF Idle (%u)", payload[1]);
            return &nci_core_state_idle;
        case NCI_DEACTIVATE_TYPE_DISCOVERY:
            GDEBUG("RF_DEACTIVATE_NTF Discovery (%u)", payload[1]);
            return &nci_core_state_discovery;
        default:
            GDEBUG("RF_DEACTIVATE_NTF %u (%u)", payload[0], payload[1]);
            break;
        }
    } else {
        GDEBUG("Failed to parse RF_DEACTIVATE_NTF");
        nci_core_stall(self, TRUE);
    }
    return NULL;
}

static
void
nci_core_state_rf_deactivate_ntf(
    NciCore* self,
    const guint8* payload,
    guint len)
{
    nci_core_transition_enter_state(self,
        nci_core_parse_rf_deactivate_ntf(self, payload, len));
    nci_core_emit_pending_signals(self);
}

static
void
nci_core_transition_rf_deactivate_ntf(
    NciCore* self,
    const guint8* payload,
    guint len)
{
    nci_core_transition_finish(self,
        nci_core_parse_rf_deactivate_ntf(self, payload, len));
}

static
void
nci_core_conn_credits_ntf(
    NciCore* self,
    const guint8* payload,
    guint len)
{
    if (len > 0 && len == (1 + payload[0] * 2)) {
        const guint8* entry = payload + 1;
        NciCorePriv* priv = self->priv;
        const guint n = payload[0];
        guint i;

        GDEBUG("CORE_CONN_CREDITS_NTF");
        for (i = 0; i < n; i++, entry += 2) {
            nci_sar_add_credits(priv->sar, entry[0], entry[1]);
        }
    } else {
        GWARN("Failed to parse CORE_CONN_CREDITS_NTF");
    }
}

static
void
nci_core_transition_default_ntf(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    switch (gid) {
    case NCI_GID_CORE:
        switch (oid) {
        case NCI_OID_CORE_CONN_CREDITS:
            nci_core_conn_credits_ntf(self, payload, len);
            return;
        case NCI_OID_CORE_GENERIC_ERROR:
            nci_core_generic_error_ntf(self, payload, len);
            return;
        }
        break;
    }
    GDEBUG("Notification 0x%02x/0x%02x is ignored in transition", gid, oid);
}

/*==========================================================================*
 * NCI_RFST_POLL_ACTIVE state
 *==========================================================================*/

static
void
nci_core_state_poll_active_ntf(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    switch (gid) {
    case NCI_GID_CORE:
        switch (oid) {
        case NCI_OID_CORE_CONN_CREDITS:
            nci_core_conn_credits_ntf(self, payload, len);
            return;
        }
        break;
    }
    nci_core_ignore_ntf(self, gid, oid, payload, len);
}

static
const NciCoreStateTransition**
nci_core_state_poll_active_transition_path(
    NciCore* self,
    NCI_STATE to)
{
    static const NciCoreStateTransition* poll_to_idle [] = {
        &nci_core_transition_poll_to_idle, NULL
    };
    static const NciCoreStateTransition* poll_to_discovery [] = {
        &nci_core_transition_poll_to_discovery, NULL
    };

    switch (to) {
    case NCI_RFST_IDLE:
        return poll_to_idle;
    case NCI_RFST_DISCOVERY:
        return poll_to_discovery;
    default:
        GWARN("Unsupported transition %s -> %s",
            nci_core_state_name(self->current_state),
            nci_core_state_name(to));
        return NULL;
    }
}

static const NciCoreState nci_core_state_poll_active = {
    .state = NCI_RFST_POLL_ACTIVE,
    .notification_handler = nci_core_state_poll_active_ntf,
    .transition_path = nci_core_state_poll_active_transition_path
};

/*==========================================================================*
 * NCI_RFST_DISCOVERY state
 *==========================================================================*/

static
const NciCoreStateTransition**
nci_core_state_disocovery_transition_path(
    NciCore* self,
    NCI_STATE to)
{
    static const NciCoreStateTransition* discovery_to_idle [] = {
        &nci_core_transition_discovery_to_idle, NULL
    };

    switch (to) {
    case NCI_RFST_IDLE:
        return discovery_to_idle;
    default:
        GWARN("Unsupported transition %s -> %s",
            nci_core_state_name(self->current_state),
            nci_core_state_name(to));
        return NULL;
    }
}

static
const NciModeParam*
nci_core_state_discovery_parse_mode_param(
    NciModeParam* param,
    NCI_MODE mode,
    const guint8* bytes,
    guint len)
{
    switch (mode) {
    case NCI_MODE_ACTIVE_POLL_A:
    case NCI_MODE_PASSIVE_POLL_A:
        if (len >= 4) {
            NciModeParamPollA* ppa = &param->poll_a;

            /* Table 54: Specific Parameters for NFC-A Poll Mode */
            ppa->sens_res[0] = bytes[0];
            ppa->sens_res[1] = bytes[1];
            ppa->nfcid1_len = bytes[2];
            if (len >= ppa->nfcid1_len + 4 &&
                len >= ppa->nfcid1_len + 4 +
                bytes[ppa->nfcid1_len + 3]) {
                if (ppa->nfcid1_len) {
                    ppa->nfcid1 = bytes + 3;
                }
                ppa->sel_res_len = bytes[ppa->nfcid1_len + 3];
                if (ppa->sel_res_len) {
                    ppa->sel_res = bytes[ppa->nfcid1_len + 4];
                }
#if GUTIL_LOG_DEBUG
                if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                    GString* buf = g_string_new(NULL);
                    guint i;

                    for (i = 0; i < ppa->nfcid1_len; i++) {
                        g_string_append_printf(buf, " %02x", ppa->nfcid1[i]);
                    }
                    GDEBUG("NFC-A");
                    GDEBUG("  PollA.sel_res = 0x%02x", ppa->sel_res);
                    GDEBUG("  PollA.nfcid1 =%s", buf->str);
                    g_string_free(buf, TRUE);
                }
#endif
                return param;
            }
        }
        GDEBUG("Failed to parse parameters for NFC-A poll mode");
        return NULL;
    default:
        GDEBUG("Unhandled activation mode");
        return NULL;
    }
}

static
gboolean
nci_core_state_discovery_parse_iso_dep_poll_a_param(
    NciActivationParamIsoDepPollA* param,
    const guint8* bytes,
    guint len)
{
    /* Answer To Select */
    const guint8 ats_len = bytes[0];

    if (ats_len >= 1 && len >= ats_len + 1) {
        const guint8* ats = bytes + 1;
        const guint8* ats_end = ats + ats_len;
        const guint8* ats_ptr = ats;
        const guint8 t0 = *ats_ptr++;

#define NFC_T4A_ATS_T0_A         (0x10) /* TA is transmitted */
#define NFC_T4A_ATS_T0_B         (0x20) /* TB is transmitted */
#define NFC_T4A_ATS_T0_C         (0x30) /* TC is transmitted */
#define NFC_T4A_ATS_T0_FSCI_MASK (0x0f) /* FSCI mask */

        /* Skip TA, TB and TC if they are present */
        if (t0 & NFC_T4A_ATS_T0_A) ats_ptr++;
        if (t0 & NFC_T4A_ATS_T0_B) ats_ptr++;
        if (t0 & NFC_T4A_ATS_T0_C) ats_ptr++;
        if (ats_ptr <= ats_end) {
            /* Table 66: FSCI to FSC Conversion */
            const guint8 fsci = (t0 & NFC_T4A_ATS_T0_FSCI_MASK);
            static const guint fsc_table[] = {
                16, 24, 32, 40, 48, 64, 96, 128, 256
            };

            param->fsc = (fsci < G_N_ELEMENTS(fsc_table)) ?
                fsc_table[fsci] :
                fsc_table[G_N_ELEMENTS(fsc_table) - 1];
            if ((param->t1.size = ats_end - ats_ptr) > 0) {
                param->t1.bytes = ats_ptr;
            }
#if GUTIL_LOG_DEBUG
            if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                GDEBUG("ISO-DEP");
                GDEBUG("  FSC = %u", param->fsc);
                if (param->t1.bytes) {
                    GString* buf = g_string_new(NULL);
                    guint i;

                    for (i = 0; i < param->t1.size; i++) {
                        g_string_append_printf(buf, " %02x",
                            param->t1.bytes[i]);
                    }
                    GDEBUG("  T1 =%s", buf->str);
                    g_string_free(buf, TRUE);
                }
            }
#endif
            return TRUE;
        }
    }
    return FALSE;
}

static
const NciActivationParam*
nci_core_state_discovery_parse_activation_param(
    NciActivationParam* param,
    NCI_RF_INTERFACE intf,
    NCI_MODE mode,
    const guint8* bytes,
    guint len)
{
    switch (intf) {
    case NCI_RF_INTERFACE_ISO_DEP:
        switch (mode) {
        case NCI_MODE_PASSIVE_POLL_A:
        case NCI_MODE_ACTIVE_POLL_A:
            if (nci_core_state_discovery_parse_iso_dep_poll_a_param(&param->
                iso_dep_poll_a, bytes, len)) {
                return param;
            }
            GDEBUG("Failed to parse parameters for NFC-A/ISO-DEP poll mode");
            break;
        case NCI_MODE_PASSIVE_POLL_B:
        case NCI_MODE_PASSIVE_POLL_F:
        case NCI_MODE_ACTIVE_POLL_F:
        case NCI_MODE_PASSIVE_POLL_15693:
        case NCI_MODE_PASSIVE_LISTEN_A:
        case NCI_MODE_PASSIVE_LISTEN_B:
        case NCI_MODE_PASSIVE_LISTEN_F:
        case NCI_MODE_ACTIVE_LISTEN_A:
        case NCI_MODE_ACTIVE_LISTEN_F:
        case NCI_MODE_PASSIVE_LISTEN_15693:
            break;
        }
        break;
    case NCI_RF_INTERFACE_FRAME:
        /* There are no Activation Parameters for Frame RF interface */
        break;
    case NCI_RF_INTERFACE_NFCEE_DIRECT:
    case NCI_RF_INTERFACE_NFC_DEP:
        GDEBUG("Unhandled interface type");
        break;
    }
    return NULL;
}

static
void
nci_core_state_discovery_intf_activated_ntf(
    NciCore* self,
    const guint8* payload,
    guint len)
{
    /* Check packet structure */
    if (len > 6) {
        const guint mode_param_len = payload[6];
        const guint off = 7 + mode_param_len;

        if (len > off + 3 &&
            len == off + 4 + payload[off + 3]) {
            NciCorePriv* priv = self->priv;
            NciIntfActivationNtf ntf;
            const guint8* mode_param_bytes =
                mode_param_len ? (payload + 7) : NULL;
            const guint activation_param_len = payload[off + 3];
            const guint8* activation_param_bytes =
                activation_param_len ? (payload + (off + 4)) : NULL;

            memset(&ntf, 0, sizeof(ntf));
            ntf.discovery_id = payload[0];
            ntf.rf_intf = payload[1];
            ntf.protocol = payload[2];
            ntf.mode = payload[3];
            ntf.max_data_packet_size = payload[4];
            ntf.num_credits = payload[5];
            ntf.mode_param_len = mode_param_len;
            ntf.data_exchange_mode = payload[off];
            ntf.transmit_rate = payload[off + 1];
            ntf.receive_rate = payload[off + 2];

            GDEBUG("RF_INTF_ACTIVATED_NTF");
            GDEBUG("  RF Discovery ID = 0x%02x", ntf.discovery_id);
            GDEBUG("  RF Interface = 0x%02x", ntf.rf_intf);
#if GUTIL_LOG_DEBUG
            if (ntf.rf_intf != NCI_RF_INTERFACE_NFCEE_DIRECT) {
                GDEBUG("  RF Protocol = 0x%02x", ntf.protocol);
                GDEBUG("  Activation RF Tech = 0x%02x", ntf.mode);
                GDEBUG("  Max Data Packet Size = %u", ntf.max_data_packet_size);
                GDEBUG("  Initial Credits = %u", ntf.num_credits);
                if (mode_param_len || activation_param_len) {
                    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                        GString* buf = g_string_new(NULL);
                        guint i;

                        if (mode_param_len) {
                            for (i = 0; i < mode_param_len; i++) {
                                g_string_append_printf(buf, " %02x",
                                    mode_param_bytes[i]);
                            }
                            GDEBUG("  RF Tech Parameters =%s", buf->str);
                        }
                        GDEBUG("  Data Exchange RF Tech = 0x%02x",
                            ntf.data_exchange_mode);
                        if (activation_param_len) {
                            g_string_set_size(buf, 0);
                            for (i = 0; i < ntf.activation_param_len; i++) {
                                g_string_append_printf(buf, " %02x",
                                    activation_param_bytes[i]);
                            }
                            GDEBUG("  Activation Parameters =%s", buf->str);
                        }
                        g_string_free(buf, TRUE);
                    }
                } else {
                    GDEBUG("  Data Exchange RF Tech = 0x%02x",
                        ntf.data_exchange_mode);
                }
            }
#endif /* GUTIL_LOG_DEBUG */

            /* Require RF Tech Parameters */
            if (mode_param_bytes) {
                NciModeParam mode_param;
                NciActivationParam activation_param;

                memset(&mode_param, 0, sizeof(mode_param));
                ntf.mode_param_bytes = mode_param_bytes;
                ntf.mode_param = nci_core_state_discovery_parse_mode_param
                    (&mode_param, ntf.mode, mode_param_bytes,
                         mode_param_len);

                if (activation_param_len) {
                    memset(&activation_param, 0, sizeof(activation_param));
                    ntf.activation_param_len = activation_param_len;
                    ntf.activation_param_bytes = activation_param_bytes;
                    ntf.activation_param =
                        nci_core_state_discovery_parse_activation_param
                            (&activation_param, ntf.rf_intf, ntf.mode,
                                 activation_param_bytes, activation_param_len);
                }

                nci_sar_set_initial_credits(priv->sar, NCI_STATIC_RF_CONN_ID,
                    ntf.num_credits);
                g_signal_emit(self, nci_core_signals
                    [SIGNAL_INTF_ACTIVATED], 0, &ntf);
                nci_core_transition_enter_state(self,
                    &nci_core_state_poll_active);
                nci_core_emit_pending_signals(self);
                return;
            }
            GDEBUG("Missing RF Tech Parameters");
        }
    }

    /* Deactivate this target */
    GDEBUG("Failed to parse RF_INTF_ACTIVATED_NTF");
    nci_core_transition_enter_state(self, &nci_core_state_poll_active);
    nci_core_set_state(self, NCI_RFST_DISCOVERY);
}

static
void
nci_core_state_discovery_ntf(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    switch (gid) {
    case NCI_GID_CORE:
        switch (oid) {
        case NCI_OID_CORE_CONN_CREDITS:
            nci_core_conn_credits_ntf(self, payload, len);
            return;
        case NCI_OID_CORE_GENERIC_ERROR:
            nci_core_generic_error_ntf(self, payload, len);
            return;
        }
        break;
    case NCI_GID_RF:
        switch (oid) {
        case NCI_OID_RF_INTF_ACTIVATED:
            nci_core_state_discovery_intf_activated_ntf(self, payload, len);
            return;
        case NCI_OID_RF_DEACTIVATE:
            nci_core_state_rf_deactivate_ntf(self, payload, len);
            return;
        }
        break;
    }
    nci_core_ignore_ntf(self, gid, oid, payload, len);
}

static const NciCoreState nci_core_state_discovery = {
    .state = NCI_RFST_DISCOVERY,
    .notification_handler = nci_core_state_discovery_ntf,
    .transition_path = nci_core_state_disocovery_transition_path
};

/*==========================================================================*
 * NCI_RFST_IDLE state
 *==========================================================================*/

static
void
nci_core_state_idle_ntf(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    switch (gid) {
    case NCI_GID_CORE:
        switch (oid) {
        case NCI_OID_CORE_CONN_CREDITS:
            nci_core_conn_credits_ntf(self, payload, len);
            return;
        }
        break;
    case NCI_GID_RF:
        switch (oid) {
        case NCI_OID_RF_DEACTIVATE:
            nci_core_state_rf_deactivate_ntf(self, payload, len);
            return;
        }
        break;
    }
    nci_core_ignore_ntf(self, gid, oid, payload, len);
}

static
const NciCoreStateTransition**
nci_core_state_idle_transition_path(
    NciCore* self,
    NCI_STATE to)
{
    static const NciCoreStateTransition* idle_to_discovery [] = {
        &nci_core_transition_idle_to_discovery, NULL
    };

    switch (to) {
    case NCI_RFST_DISCOVERY:
        return idle_to_discovery;
    default:
        GWARN("Unsupported transition %s -> %s",
            nci_core_state_name(self->current_state),
            nci_core_state_name(to));
        return NULL;
    }
}

static const NciCoreState nci_core_state_idle = {
    .state = NCI_RFST_IDLE,
    .notification_handler = nci_core_state_idle_ntf,
    .transition_path = nci_core_state_idle_transition_path
};

/*==========================================================================*
 * NCI_RFST_POLL_ACTIVE -> NCI_RFST_IDLE
 *==========================================================================*/

static
void
nci_core_transition_poll_to_idle_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;

    if (len == 1 && pkt[0] == NCI_STATUS_OK) {
        GDEBUG("%c RF_DEACTIVATE_RSP ok", DIR_IN);
        /* Wait for RF_DEACTIVATE_NTF */
    } else {
        GWARN("RF_DEACTIVATE_CMD failed");
        nci_core_stall(self, TRUE);
    }
}

static
void
nci_core_transition_poll_to_idle_ntf(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    switch (gid) {
    case NCI_GID_RF:
        switch (oid) {
        case NCI_OID_RF_DEACTIVATE:
            nci_core_transition_rf_deactivate_ntf(self, payload, len);
            return;
        }
        break;
    }
    nci_core_transition_default_ntf(self, gid, oid, payload, len);
}

static
void
nci_core_transition_poll_to_idle_start(
    NciCore* self)
{
    nci_core_deactivate_to_idle(self, nci_core_transition_poll_to_idle_rsp);
}

static const NciCoreStateTransition nci_core_transition_poll_to_idle = {
    .destination = &nci_core_state_idle,
    .start = nci_core_transition_poll_to_idle_start,
    .notification_handler = nci_core_transition_poll_to_idle_ntf
};

/*==========================================================================*
 * NCI_RFST_ACTIVE -> NCI_RFST_DISCOVERY transition
 *==========================================================================*/

static
void
nci_core_transition_poll_to_discovery_idle_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;

    if (len == 1 && pkt[0] == NCI_STATUS_OK) {
        GDEBUG("%c RF_DEACTIVATE_RSP (Idle) ok", DIR_IN);
        nci_core_transition_finish(self, &nci_core_state_idle);
    } else {
        GWARN("RF_DEACTIVATE_CMD (Idle) failed too");
        nci_core_stall(self, TRUE);
    }
}

static
void
nci_core_transition_poll_to_discovery_deactivate_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;

    if (len == 1 && pkt[0] == NCI_STATUS_OK) {
        GDEBUG("%c RF_DEACTIVATE_RSP ok", DIR_IN);
        /* Wait for RF_DEACTIVATE_NTF */
    } else {
        GWARN("RF_DEACTIVATE_CMD (Discovery) failed");
        nci_core_deactivate_to_idle(self,
            nci_core_transition_poll_to_discovery_idle_rsp);
    }
}

static
void
nci_core_transition_poll_to_discovery_ntf(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    switch (gid) {
    case NCI_GID_RF:
        switch (oid) {
        case NCI_OID_RF_DEACTIVATE:
            nci_core_transition_rf_deactivate_ntf(self, payload, len);
            return;
        }
        break;
    }
    nci_core_transition_default_ntf(self, gid, oid, payload, len);
}

static
void
nci_core_transition_poll_to_discovery_start(
    NciCore* self)
{
    nci_core_deactivate_to_discovery(self,
        nci_core_transition_poll_to_discovery_deactivate_rsp);
}

static const NciCoreStateTransition nci_core_transition_poll_to_discovery = {
    .destination = &nci_core_state_discovery,
    .start = nci_core_transition_poll_to_discovery_start,
    .notification_handler = nci_core_transition_poll_to_discovery_ntf
};

/*==========================================================================*
 * NCI_RFST_DISCOVERY -> NCI_RFST_IDLE
 *==========================================================================*/

static
void
nci_core_transition_discovery_to_idle_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;

    if (len == 1 && pkt[0] == NCI_STATUS_OK) {
        GDEBUG("%c RF_DEACTIVATE_RSP ok", DIR_IN);
        nci_core_transition_finish(self, &nci_core_state_idle);
    } else {
        GWARN("RF_DEACTIVATE_CMD failed");
        nci_core_stall(self, TRUE);
    }
}

static
void
nci_core_transition_discovery_to_idle_start(
    NciCore* self)
{
    nci_core_deactivate_to_idle(self,
        nci_core_transition_discovery_to_idle_rsp);
}

static const NciCoreStateTransition nci_core_transition_discovery_to_idle = {
    .destination = &nci_core_state_idle,
    .start = nci_core_transition_discovery_to_idle_start,
    .notification_handler = nci_core_transition_default_ntf
};

/*==========================================================================*
 * NCI_RFST_IDLE -> NCI_RFST_DISCOVERY transition
 *==========================================================================*/

static
void
nci_core_transition_idle_to_discovery_discover_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;

    if (len > 0 && pkt[0] == NCI_STATUS_OK) {
        GDEBUG("%c RF_DISCOVER_RSP ok", DIR_IN);
        nci_core_transition_finish(self, &nci_core_state_discovery);
    } else {
        GWARN("RF_DISCOVER_CMD failed");
        nci_core_stall(self, TRUE);
    }
}

static
void
nci_core_transition_idle_to_discovery_discover(
    NciCore* self)
{
    static const guint8 cmd_data[] = {
        0x04,
        NCI_MODE_PASSIVE_POLL_A, 1,
        NCI_MODE_PASSIVE_POLL_B, 1,
        NCI_MODE_PASSIVE_POLL_F, 1,
        NCI_MODE_PASSIVE_POLL_15693, 1
    };

    GDEBUG("%c RF_DISCOVER_CMD", DIR_OUT);
    nci_core_send_command(self, NCI_GID_RF, NCI_OID_RF_DISCOVER,
        nci_core_static_bytes(self, cmd_data, sizeof(cmd_data)),
        nci_core_generic_command_completion,
        nci_core_transition_idle_to_discovery_discover_rsp);
}

static
void
nci_core_transition_idle_to_discover_map_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;

    if (len > 0 && pkt[0] == NCI_STATUS_OK) {
        GDEBUG("%c RF_DISCOVER_MAP_RSP ok", DIR_IN);
        nci_core_transition_idle_to_discovery_discover(self);
    } else {
        GWARN("RF_DISCOVER_MAP_CMD failed");
        nci_core_stall(self, TRUE);
    }
}

static
void
nci_core_transition_idle_to_discover_map(
    NciCore* self)
{
    static const guint8 cmd_data[] = {
        0x05,

        NCI_PROTOCOL_T1T,
        NCI_DISCOVER_MAP_MODE_POLL,
        NCI_RF_INTERFACE_FRAME,

        NCI_PROTOCOL_T2T,
        NCI_DISCOVER_MAP_MODE_POLL,
        NCI_RF_INTERFACE_FRAME,

        NCI_PROTOCOL_T3T,
        NCI_DISCOVER_MAP_MODE_POLL,
        NCI_RF_INTERFACE_FRAME,

        NCI_PROTOCOL_ISO_DEP,
        NCI_DISCOVER_MAP_MODE_POLL,
        NCI_RF_INTERFACE_ISO_DEP,

        NCI_PROTOCOL_NFC_DEP,
        NCI_DISCOVER_MAP_MODE_POLL,
        NCI_RF_INTERFACE_NFC_DEP
    };

    GDEBUG("%c RF_DISCOVER_MAP_CMD", DIR_OUT);
    nci_core_send_command(self, NCI_GID_RF, NCI_OID_RF_DISCOVER_MAP,
        nci_core_static_bytes(self, cmd_data, sizeof(cmd_data)),
        nci_core_generic_command_completion,
        nci_core_transition_idle_to_discover_map_rsp);
}

static
void
nci_core_transition_idle_to_discovery_set_technology_routing_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;

    if (len > 0 && pkt[0] == NCI_STATUS_OK) {
        GDEBUG("%c RF_SET_LISTEN_MODE_ROUTING_RSP (Technology) ok", DIR_IN);
    } else {
        GDEBUG("RF_SET_LISTEN_MODE_ROUTING_CMD (Technology) failed");
    }

    /* Ignore errors */
    nci_core_transition_idle_to_discover_map(self);
}

static
void
nci_core_transition_idle_to_discovery_set_technology_routing(
    NciCore* self)
{
    static const guint8 template[] = {
        0x00, /* Last message */
        0x04, /* Number of Routing Entries */

        NCI_ROUTING_ENTRY_TYPE_TECHNOLOGY,
        3,
        NCI_NFCEE_ID_DH,
        NCI_ROUTING_ENTRY_POWER_ON,
        NCI_RF_TECHNOLOGY_A,

        NCI_ROUTING_ENTRY_TYPE_TECHNOLOGY,
        3,
        NCI_NFCEE_ID_DH,
        NCI_ROUTING_ENTRY_POWER_ON,
        NCI_RF_TECHNOLOGY_B,

        NCI_ROUTING_ENTRY_TYPE_TECHNOLOGY,
        3,
        NCI_NFCEE_ID_DH,
        NCI_ROUTING_ENTRY_POWER_ON,
        NCI_RF_TECHNOLOGY_F,

        NCI_ROUTING_ENTRY_TYPE_TECHNOLOGY,
        3,
        NCI_NFCEE_ID_DH,
        NCI_ROUTING_ENTRY_POWER_ON,
        NCI_RF_TECHNOLOGY_15693,
    };

    GDEBUG("%c RF_SET_LISTEN_MODE_ROUTING_CMD (Technology)", DIR_OUT);
    nci_core_send_command(self, NCI_GID_RF, NCI_OID_RF_SET_LISTEN_MODE_ROUTING,
        nci_core_static_bytes(self, template, sizeof(template)),
        nci_core_generic_command_completion,
        nci_core_transition_idle_to_discovery_set_technology_routing_rsp);
}

static
void
nci_core_transition_idle_to_discovery_set_protocol_routing_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;

    if (len > 0 && pkt[0] == NCI_STATUS_OK) {
        GDEBUG("%c RF_SET_LISTEN_MODE_ROUTING_RSP (Protocol) ok", DIR_IN);
        nci_core_transition_idle_to_discover_map(self);
    } else {
        NciCorePriv* priv = self->priv;

        GDEBUG("RF_SET_LISTEN_MODE_ROUTING_CMD (Protocol) failed");
        if (priv->nfcc_routing & NCI_NFCC_ROUTING_TECHNOLOGY_BASED) {
            nci_core_transition_idle_to_discovery_set_technology_routing(self);
        } else {
            nci_core_transition_idle_to_discover_map(self);
        }
    }
}

static
void
nci_core_transition_idle_to_discovery_set_protocol_routing(
    NciCore* self)
{
    static const guint8 template[] = {
        0x00, /* Last message */
        0x05, /* Number of Routing Entries */

        NCI_ROUTING_ENTRY_TYPE_PROTOCOL,
        3,
        NCI_NFCEE_ID_DH,
        NCI_ROUTING_ENTRY_POWER_ON,
        NCI_PROTOCOL_T1T,

        NCI_ROUTING_ENTRY_TYPE_PROTOCOL,
        3,
        NCI_NFCEE_ID_DH,
        NCI_ROUTING_ENTRY_POWER_ON,
        NCI_PROTOCOL_T2T,

        NCI_ROUTING_ENTRY_TYPE_PROTOCOL,
        3,
        NCI_NFCEE_ID_DH,
        NCI_ROUTING_ENTRY_POWER_ON,
        NCI_PROTOCOL_T3T,

        NCI_ROUTING_ENTRY_TYPE_PROTOCOL,
        3,
        NCI_NFCEE_ID_DH,
        NCI_ROUTING_ENTRY_POWER_ON,
        NCI_PROTOCOL_ISO_DEP,

        NCI_ROUTING_ENTRY_TYPE_PROTOCOL,
        3,
        NCI_NFCEE_ID_DH,
        NCI_ROUTING_ENTRY_POWER_ON,
        NCI_PROTOCOL_NFC_DEP,
    };

    GDEBUG("%c RF_SET_LISTEN_MODE_ROUTING_CMD (Protocol)", DIR_OUT);
    nci_core_send_command(self, NCI_GID_RF, NCI_OID_RF_SET_LISTEN_MODE_ROUTING,
        nci_core_static_bytes(self, template, sizeof(template)),
        nci_core_generic_command_completion,
        nci_core_transition_idle_to_discovery_set_protocol_routing_rsp);
}

static
void
nci_core_transition_idle_to_discovery_start(
    NciCore* self)
{
    NciCorePriv* priv = self->priv;

    /*
     * Some controllers seem to require RF_SET_LISTEN_MODE_ROUTING,
     * some don't support it at all. Let's give it a try (provided
     * that controller indicated support for protocol based routing
     * in CORE_INIT_RSP) and ignore any errors.
     */
    if (priv->version > NCI_INTERFACE_VERSION_1) {
        if (priv->nfcc_routing & NCI_NFCC_ROUTING_PROTOCOL_BASED) {
            nci_core_transition_idle_to_discovery_set_protocol_routing(self);
        } else if (priv->nfcc_routing & NCI_NFCC_ROUTING_TECHNOLOGY_BASED) {
            nci_core_transition_idle_to_discovery_set_technology_routing(self);
        } else {
            nci_core_transition_idle_to_discover_map(self);
        }
    } else {
        nci_core_transition_idle_to_discover_map(self);
    }
}

static const NciCoreStateTransition nci_core_transition_idle_to_discovery = {
    .destination = &nci_core_state_discovery,
    .start = nci_core_transition_idle_to_discovery_start,
    .notification_handler = nci_core_transition_default_ntf
};

/*==========================================================================*
 * Transition -> NCI_RFST_IDLE
 *==========================================================================*/

static
void
nci_core_transition_to_idle_get_config_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;

    if (len > 1 && pkt[0] == NCI_STATUS_OK) {
        GDEBUG("%c CORE_GET_CONFIG_RSP ok", DIR_IN);
        nci_core_transition_finish(self, &nci_core_state_idle);
    } else {
        GWARN("CORE_GET_CONFIG_CMD failed");
        nci_core_stall(self, TRUE);
    }
}

static
void
nci_core_transition_to_idle_get_config(
    NciCore* self)
{
    static const guint8 cmd_data[] = {
        4,
        NCI_CONFIG_PI_BIT_RATE,
        NCI_CONFIG_LA_SEL_INFO,
        NCI_CONFIG_LF_PROTOCOL_TYPE,
        NCI_CONFIG_TOTAL_DURATION
    };

    /*
     * We may want to set some parameters some day but for now let's just
     * query something and see how it works...
     */
    GDEBUG("%c CORE_GET_CONFIG_CMD", DIR_OUT);
    nci_core_send_command(self, NCI_GID_CORE, NCI_OID_CORE_GET_CONFIG,
        nci_core_static_bytes(self, cmd_data, sizeof(cmd_data)),
        nci_core_generic_command_completion,
        nci_core_transition_to_idle_get_config_rsp);
}

static
void
nci_core_transition_to_idle_init_v1_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    NciCorePriv* priv = self->priv;
    const guint8* pkt = payload;
    guint n; /* Number of Supported RF Interfaces */

    /*
     * NFC Controller Interface (NCI), Version 1.1, Section 4.2
     *
     * CORE_INIT_RSP
     *
     * +=======================================================+
     * | Offset | Size | Description                           |
     * +=======================================================+
     * | 0      | 1    | Status                                |
     * | 1      | 4    | NFCC Features                         |
     * | 5      | 1    | Number of Supported RF Interfaces (n) |
     * | 6      | n    | Supported RF Interfaces               |
     * | 6 + n  | 1    | Max Logical Connections               |
     * | 7 + n  | 2    | Max Routing Table Size                |
     * | 9 + n  | 1    | Max Control Packet Payload Size       |
     * | 10 + n | 2    | Max Size for Large Parameters         |
     * | 12 + n | 1    | Manufacturer ID                       |
     * | 13 + n | 4    | Manufacturer Specific Information     |
     * +=======================================================+
     */
    if (len >= 17 && pkt[0] == NCI_STATUS_OK &&
        len == ((n = pkt[5]) + 17)) {
        const guint8* rf_interfaces = pkt + 6;
        guint8 max_logical_conns = pkt[6 + n];
        guint8 max_control_packet = pkt[9 + n];

        if (priv->rf_interfaces) {
            g_bytes_unref(priv->rf_interfaces);
            priv->rf_interfaces = NULL;
        }
        if (n > 0) {
            priv->rf_interfaces = g_bytes_new(rf_interfaces, n);
        }

        priv->nfcc_discovery = pkt[1];
        priv->nfcc_routing = pkt[2];
        priv->nfcc_power = pkt[3];
        priv->max_routing_table_size = ((guint)pkt[8 + n] << 8) + pkt[7 + n];

        GDEBUG("%c CORE_INIT_RSP (v1) ok", DIR_IN);
        GDEBUG("  Features = %02x %02x %02x %02x",
            pkt[1], pkt[2], pkt[3], pkt[4]);
#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GString* buf = g_string_new(NULL);
            guint i;

            for (i = 0; i < n; i++) {
                g_string_append_printf(buf, " %02x", rf_interfaces[i]);
            }
            GDEBUG("  Supported interfaces =%s", buf->str);
            g_string_free(buf, TRUE);
        }
#endif
        GDEBUG("  Max Logical Connections = %u", max_logical_conns);
        GDEBUG("  Max Routing Table Size = %u", priv->max_routing_table_size);
        GDEBUG("  Max Control Packet Size = %u", max_control_packet);
        GDEBUG("  Manufacturer = 0x%02x", pkt[12 + n]);
        GDEBUG("  Manufacturer Info = %02x %02x %02x %02x",
            pkt[13 + n], pkt[14 + n], pkt[15 + n], pkt[16 + n]);

        nci_sar_set_max_logical_connections(priv->sar, max_logical_conns);
        nci_sar_set_max_control_packet_size(priv->sar, max_control_packet);
        nci_core_transition_to_idle_get_config(self);
    } else {
        GWARN("CORE_INIT (v1) failed");
        nci_core_stall(self, TRUE);
    }
}

static
void
nci_core_transition_to_idle_init_v2_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    NciCorePriv* priv = self->priv;
    const guint8* pkt = payload;
    guint n; /* Number of Supported RF Interfaces */

    /*
     * NFC Controller Interface (NCI), Version 2.0, Section 4.2
     *
     * CORE_INIT_RSP
     *
     * +=========================================================+
     * | Offset | Size | Description                             |
     * +=========================================================+
     * | 0      | 1    | Status                                  |
     * | 1      | 4    | NFCC Features                           |
     * | 5      | 1    | Max Logical Connections                 |
     * | 6      | 2    | Max Routing Table Size                  |
     * | 8      | 1    | Max Control Packet Payload Size         |
     * | 9      | 1    | Max Static HCI Packet Size              |
     * | 10     | 1    | Number of Static HCI Connection Credits |
     * | 11     | 2    | Max NFC-V RF Frame Size                 |
     * | 13     | 1    | Number of Supported RF Interfaces (n)   |
     * | 14     | 2*n  | Supported RF Interfaces and Extensions  |
     * +=========================================================+
     */
    if (len >= 14 && pkt[0] == NCI_STATUS_OK &&
        len == (2 * (n = pkt[13]) + 14)) {
        const guint8* rf_interfaces = pkt + 14;
        guint8 max_logical_conns = pkt[5];
        guint8 max_control_packet = pkt[8];

        if (priv->rf_interfaces) {
            g_bytes_unref(priv->rf_interfaces);
            priv->rf_interfaces = NULL;
        }
        if (n > 0) {
            priv->rf_interfaces = g_bytes_new(rf_interfaces, n);
        }

        priv->nfcc_discovery = pkt[1];
        priv->nfcc_routing = pkt[2];
        priv->nfcc_power = pkt[3];
        priv->max_routing_table_size = ((guint)pkt[7] << 8) + pkt[6];

        GDEBUG("%c CORE_INIT_RSP (v2) ok", DIR_IN);
        GDEBUG("  Features = %02x %02x %02x %02x",
            pkt[1], pkt[2], pkt[3], pkt[4]);
#if GUTIL_LOG_DEBUG
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            GString* buf = g_string_new(NULL);
            guint i;

            for (i = 0; i < n; i++) {
                g_string_append_printf(buf, " %02x", rf_interfaces[2 * i]);
            }
            GDEBUG("  Supported interfaces =%s", buf->str);
            g_string_free(buf, TRUE);
        }
#endif
        GDEBUG("  Max Logical Connections = %u", max_logical_conns);
        GDEBUG("  Max Routing Table Size = %u", priv->max_routing_table_size);
        GDEBUG("  Max Control Packet Size = %u", max_control_packet);

        nci_sar_set_max_logical_connections(priv->sar, max_logical_conns);
        nci_sar_set_max_control_packet_size(priv->sar, max_control_packet);
        nci_core_transition_to_idle_get_config(self);
    } else {
        GWARN("CORE_INIT (v2) failed");
        nci_core_stall(self, TRUE);
    }
}

static
void
nci_core_transition_to_idle_reset_rsp(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    const guint8* pkt = payload;
    NciCorePriv* priv = self->priv;

    if (len == 3) {
        priv->version = NCI_INTERFACE_VERSION_1;
        if (pkt[0] == NCI_STATUS_OK) {
            GDEBUG("%c CORE_RESET_RSP (v1) ok", DIR_IN);
            GDEBUG("%c CORE_INIT_CMD (v1)", DIR_OUT);
            nci_core_send_command(self, NCI_GID_CORE, NCI_OID_CORE_INIT, NULL,
                nci_core_generic_command_completion,
                nci_core_transition_to_idle_init_v1_rsp);
        } else {
            GWARN("CORE_RESET_CMD failed");
            nci_core_stall(self, TRUE);
        }
    } else if (len == 1) {
        GDEBUG("%c CORE_RESET_RSP (v2)", DIR_IN);
        priv->version = NCI_INTERFACE_VERSION_2;
        /* Wait for notification */
    } else {
        GWARN("Unexpected CORE_RESET response");
        nci_core_stall(self, TRUE);
    }
}

static
void
nci_core_transition_to_idle_ntf(
    NciCore* self,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    NciCorePriv* priv = self->priv;

    switch (gid) {
    case NCI_GID_CORE:
        switch (oid) {
        case NCI_OID_CORE_RESET:
            /* Notification is only expected in NCI 2.x case */
            if (priv->version == NCI_INTERFACE_VERSION_2) {
                static const guint8 cmd_data[] = { 0x00, 0x00 };

                GDEBUG("CORE_RESET_NTF (v2)");
                GDEBUG("%c CORE_INIT_CMD (v2)", DIR_OUT);
                nci_core_send_command(self, NCI_GID_CORE, NCI_OID_CORE_INIT,
                    nci_core_static_bytes(self, cmd_data, sizeof(cmd_data)),
                    nci_core_generic_command_completion,
                    nci_core_transition_to_idle_init_v2_rsp);
                return;
            }
        }
        break;
    }
    nci_core_transition_default_ntf(self, gid, oid, payload, len);
}

static
void
nci_core_transition_to_idle_start(
    NciCore* self)
{
    static const guint8 cmd_data[] = { 0x00 /* Keep Configuration */ };

    GDEBUG("%c CORE_RESET_CMD", DIR_OUT);
    nci_core_send_command(self, NCI_GID_CORE, NCI_OID_CORE_RESET,
        nci_core_static_bytes(self, cmd_data, sizeof(cmd_data)),
        nci_core_generic_command_completion,
        nci_core_transition_to_idle_reset_rsp);
}

static const NciCoreStateTransition nci_core_transition_to_idle = {
    .destination = &nci_core_state_idle,
    .start = nci_core_transition_to_idle_start,
    .notification_handler = nci_core_transition_to_idle_ntf
};

/*==========================================================================*
 * SAR client
 *==========================================================================*/

static
void
nci_core_sar_error(
    NciSarClient* client)
{
    GWARN("State machine broke");
    nci_core_stall(nci_core_priv_from_sar_client(client)->self, TRUE);
}

static
void
nci_core_sar_handle_response(
    NciSarClient* client,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    NciCorePriv* priv = nci_core_priv_from_sar_client(client);
    NciCoreControlPacketHandler handler = priv->rsp_handler;

    if (handler) {
        if (priv->rsp_gid == gid && priv->rsp_oid == oid) {
            if (priv->cmd_timeout_id) {
                g_source_remove(priv->cmd_timeout_id);
                priv->cmd_timeout_id = 0;
            }
            priv->cmd_id = 0;
            priv->rsp_handler = NULL;
            handler(priv->self, gid, oid, payload, len);
        } else {
            GWARN("Invalid response %02x/%02x", gid, oid);
        }
    } else {
        GWARN("Unexpected response %02x/%02x", gid, oid);
    }
}

static
void
nci_core_sar_handle_notification(
    NciSarClient* client,
    guint8 gid,
    guint8 oid,
    const void* payload,
    guint len)
{
    NciCorePriv* priv = nci_core_priv_from_sar_client(client);

    if (priv->transition) {
        priv->transition->notification_handler(priv->self, gid, oid,
            payload, len);
    } else if (priv->last_state) {
        priv->last_state->notification_handler(priv->self, gid, oid,
            payload, len);
    } else {
        GDEBUG("Unhandled notification 0x%02x/0x%02x", gid, oid);
    }
}

static
void
nci_core_sar_handle_data_packet(
    NciSarClient* client,
    guint8 cid,
    const void* payload,
    guint len)
{
    NciCore* self = nci_core_priv_from_sar_client(client)->self;

    g_signal_emit(self, nci_core_signals [SIGNAL_DATA_PACKET], 0,
        cid, payload, len);
}

/*==========================================================================*
 * Interface
 *==========================================================================*/

NciCore*
nci_core_new(
    NciHalIo* io)
{
    if (G_LIKELY(io)) {
        NciCore* self = g_object_new(NCI_TYPE_CORE, NULL);
        NciCorePriv* priv = self->priv;

        priv->sar = nci_sar_new(io, &priv->sar_client);
        return self;
    }
    return NULL;
}

void
nci_core_free(
    NciCore* self)
{
    if (G_LIKELY(self)) {
        NciCorePriv* priv = self->priv;

        nci_core_stall(self, FALSE);
        nci_sar_free(priv->sar);
        priv->sar = NULL;
        g_object_unref(NCI_CORE(self));
    }
}

void
nci_core_restart(
    NciCore* self)
{
    if (G_LIKELY(self)) {
        NciCorePriv* priv = self->priv;

        nci_sar_reset(priv->sar);
        nci_core_cancel_command(self);
        nci_core_set_current_state(self, NCI_STATE_INIT);
        nci_core_transition_start(self, &nci_core_transition_to_idle);
    }
}

static
gboolean
nci_core_append_transitions(
    NciCore* self,
    const NciCoreStateTransition** path)
{
    if (path) {
        NciCorePriv* priv = self->priv;
        guint i;

        for (i = 0; path[i]; i++) {
            g_ptr_array_add(priv->next_transitions, (void*)path[i]);
        }
        return TRUE;
    } else {
        return FALSE;
    }
}

gboolean
nci_core_set_state(
    NciCore* self,
    NCI_STATE state)
{
    if (G_LIKELY(self)) {
        NciCorePriv* priv = self->priv;

        if (self->next_state == state) {
            /* We are either already there or can just let the transition
             * to run to the end. In either case there's nothing to do.*/
            return TRUE;
        } else if (priv->transition) { 
            const NciCoreState* dest = priv->transition->destination;

            if (dest->state == state) {
                /* Transition is already running */
                return TRUE;
            } else {
                return nci_core_append_transitions(self,
                    dest->transition_path(self, state));
            }
        } else if (priv->last_state) { 
            const NciCoreStateTransition** path =
               priv->last_state->transition_path(self, state);

            if (path) {
                if (path[0]) {
                    nci_core_append_transitions(self, path + 1);
                }
                nci_core_transition_start(self, path[0]);
                return TRUE;
            }
        } else if (!priv->transition) {
            /* Switch to initial state */
            nci_core_transition_start(self, &nci_core_transition_to_idle);
            if (state == NCI_RFST_IDLE) {
                /* Initial state is our target */
                return TRUE;
            } else {
                /* Continue from the initial state */
                return nci_core_append_transitions(self,
                    nci_core_state_idle_transition_path(self, state));
            }
            return TRUE;
        }
    }
    return FALSE;
}

void
nci_core_stall(
    NciCore* self,
    gboolean error)
{
    if (G_LIKELY(self)) {
        NciCorePriv* priv = self->priv;
        NCI_STATE state = error ? NCI_STATE_ERROR : NCI_STATE_STOP;

        priv->last_state = NULL;
        priv->transition = NULL;
        nci_core_cancel_command(self);
        nci_core_set_current_state(self, state);
        nci_core_set_next_state(self, state);
        nci_core_emit_pending_signals(self);
    }
}

guint
nci_core_send_data_msg(
    NciCore* self,
    guint8 cid,
    GBytes* payload,
    NciCoreSendFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self)) {
        NciCorePriv* priv = self->priv;

        if (complete || destroy) {
            NciCoreSendData* data = g_slice_new0(NciCoreSendData);

            data->complete = complete;
            data->destroy = destroy;
            data->user_data = user_data;
            return nci_sar_send_data_packet(priv->sar, cid, payload,
                nci_core_send_data_msg_complete,
                nci_core_send_data_msg_destroy, data);
        } else {
            return nci_sar_send_data_packet(priv->sar, cid, payload,
                NULL, NULL, NULL);
        }
    }
    return 0;
}

void
nci_core_cancel(
    NciCore* self,
    guint id)
{
    if (G_LIKELY(self)) {
        NciCorePriv* priv = self->priv;

        nci_sar_cancel(priv->sar, id);
    }
}

gulong
nci_core_add_current_state_changed_handler(
    NciCore* self,
    NciCoreFunc func,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(func)) ? g_signal_connect(self,
        SIGNAL_CURRENT_STATE_NAME, G_CALLBACK(func), user_data) : 0;
}

gulong
nci_core_add_next_state_changed_handler(
    NciCore* self,
    NciCoreFunc func,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(func)) ? g_signal_connect(self,
        SIGNAL_NEXT_STATE_NAME, G_CALLBACK(func), user_data) : 0;
}

gulong
nci_core_add_intf_activated_handler(
    NciCore* self,
    NciCoreIntfActivationFunc func,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(func)) ? g_signal_connect(self,
        SIGNAL_INTF_ACTIVATED_NAME, G_CALLBACK(func), user_data) : 0;
}

gulong
nci_core_add_data_packet_handler(
    NciCore* self,
    NciCoreDataPacketFunc func,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(func)) ? g_signal_connect(self,
        SIGNAL_DATA_PACKET_NAME, G_CALLBACK(func), user_data) : 0;
}

void
nci_core_remove_handler(
    NciCore* self,
    gulong id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        g_signal_handler_disconnect(self, id);
    }
}

void
nci_core_remove_handlers(
    NciCore* self,
    gulong* ids,
    guint count)
{
    gutil_disconnect_handlers(self, ids, count);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nci_core_init(
    NciCore* self)
{
    static const NciSarClientFunctions sar_client_functions = {
        .error = nci_core_sar_error,
        .handle_response = nci_core_sar_handle_response,
        .handle_notification = nci_core_sar_handle_notification,
        .handle_data_packet = nci_core_sar_handle_data_packet
    };

    NciCorePriv* priv = G_TYPE_INSTANCE_GET_PRIVATE(self, NCI_TYPE_CORE,
        NciCorePriv);

    self->cmd_timeout = DEFAULT_TIMEOUT;
    self->priv = priv;
    priv->self = self;
    priv->sar_client.fn = &sar_client_functions;
    priv->next_transitions = g_ptr_array_new();
    priv->pool = gutil_idle_pool_new();
}

static
void
nci_core_finalize(
    GObject* object)
{
    NciCore* self = NCI_CORE(object);
    NciCorePriv* priv = self->priv;

    if (priv->cmd_timeout_id) {
        g_source_remove(priv->cmd_timeout_id);
    }
    nci_sar_free(priv->sar);
    if (priv->rf_interfaces) {
        g_bytes_unref(priv->rf_interfaces);
    }
    gutil_idle_pool_unref(priv->pool);
    g_ptr_array_free(priv->next_transitions, TRUE);
    G_OBJECT_CLASS(nci_core_parent_class)->finalize(object);
}

static
void
nci_core_class_init(
    NciCoreClass* klass)
{
    g_type_class_add_private(klass, sizeof(NciCorePriv));
    G_OBJECT_CLASS(klass)->finalize = nci_core_finalize;
    nci_core_signals[SIGNAL_CURRENT_STATE] =
        g_signal_new(SIGNAL_CURRENT_STATE_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    nci_core_signals[SIGNAL_NEXT_STATE] =
        g_signal_new(SIGNAL_NEXT_STATE_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    nci_core_signals[SIGNAL_INTF_ACTIVATED] =
        g_signal_new(SIGNAL_INTF_ACTIVATED_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
            G_TYPE_POINTER);
    nci_core_signals[SIGNAL_DATA_PACKET] =
        g_signal_new(SIGNAL_DATA_PACKET_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
            G_TYPE_UCHAR, G_TYPE_POINTER, G_TYPE_UINT);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
