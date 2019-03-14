/*
 * Copyright (C) 2019 Jolla Ltd.
 * Copyright (C) 2019 Slava Monich <slava.monich@jolla.com>
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

#include "nci_sm.h"
#include "nci_sar.h"
#include "nci_state_p.h"
#include "nci_state_impl.h"
#include "nci_log.h"

typedef NciState NciStateDiscovery;
typedef NciStateClass NciStateDiscoveryClass;

G_DEFINE_TYPE(NciStateDiscovery, nci_state_discovery, NCI_TYPE_STATE)
#define THIS_TYPE (nci_state_discovery_get_type())
#define PARENT_CLASS (nci_state_discovery_parent_class)

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
const NciModeParam*
nci_state_discovery_parse_mode_param(
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

            /*
             * Table 54: Specific Parameters for NFC-A Poll Mode
             *
             * +=========================================================+
             * | Offset | Size | Description                             |
             * +=========================================================+
             * | 0      | 2    | SENS_RES Response                       |
             * | 2      | 1    | Length of NFCID1 Parameter (n)          |
             * | 3      | n    | NFCID1 (0, 4, 7, or 10 Octets)          |
             * | 3 + n  | 1    | SEL_RES Response Length (m)             |
             * | 4 + n  | m    | SEL_RES Response (0 or 1 Octet)         |
             * +=========================================================+
             */
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
nci_state_discovery_parse_iso_dep_poll_a_param(
    NciActivationParamIsoDepPollA* param,
    const guint8* bytes,
    guint len)
{
    /* Answer To Select */
    const guint8 ats_len = bytes[0];

    /*
     * Table 76: Activation Parameters for NFC-A/ISO-DEP Poll Mode
     *
     * +=========================================================+
     * | Offset | Size | Description                             |
     * +=========================================================+
     * | 0      | 1    | RATS Response Length (n)                |
     * | 1      | n    | RATS Response starting from Byte 2      |
     * +=========================================================+
     */
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
nci_state_discovery_parse_activation_param(
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
            if (nci_state_discovery_parse_iso_dep_poll_a_param(&param->
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
nci_state_discovery_intf_activated_ntf(
    NciState* self,
    const GUtilData* payload)
{
    NciSm* sm = nci_state_sm(self);
    const guint len = payload->size;
    const guint8* pkt = payload->bytes;

    /*
     * Table 61: Notification for RF Interface activation
     *
     * RF_INTF_ACTIVATED_NTF
     *
     * +=========================================================+
     * | Offset | Size | Description                             |
     * +=========================================================+
     * | 0      | 1    | RF Discovery ID                         |
     * | 1      | 1    | RF Interface                            |
     * | 2      | 1    | RF Protocol                             |
     * | 3      | 1    | Activation RF Technology and Mode       |
     * | 4      | 1    | Max Data Packet Payload Size            |
     * | 5      | 1    | Initial Number of Credits               |
     * | 6      | 1    | Length of RF Technology Parameters (n)  |
     * | 7      | n    | RF Technology Specific Parameters       |
     * | 7 + n  | 1    | Data Exchange RF Technology and Mode    |
     * | 8 + n  | 1    | Data Exchange Transmit Bit Rate         |
     * | 9 + n  | 1    | Data Exchange Receive Bit Rate          |
     * | 10 + n | 1    | Length of Activation Parameters (m)     |
     * | 11 + n | m    | Activation Parameters                   |
     * +=========================================================+
     */
    
    if (len > 6) {
        const guint n = pkt[6];
        const guint m = (len > (10 + n)) ? pkt[10 + n] : 0;

        if (len >= 11 + n + m) {
            NciIntfActivationNtf ntf;
            const guint8* mode_param_bytes = n ? (pkt + 7) : NULL;
            const guint8* activation_param_bytes = m ? (pkt + (11 + n)) : NULL;

            memset(&ntf, 0, sizeof(ntf));
            ntf.discovery_id = pkt[0];
            ntf.rf_intf = pkt[1];
            ntf.protocol = pkt[2];
            ntf.mode = pkt[3];
            ntf.max_data_packet_size = pkt[4];
            ntf.num_credits = pkt[5];
            ntf.mode_param_len = n;
            ntf.data_exchange_mode = pkt[7 + n];
            ntf.transmit_rate = pkt[8 + n];
            ntf.receive_rate = pkt[9 + n];

            GDEBUG("RF_INTF_ACTIVATED_NTF");
            GDEBUG("  RF Discovery ID = 0x%02x", ntf.discovery_id);
            GDEBUG("  RF Interface = 0x%02x", ntf.rf_intf);
#if GUTIL_LOG_DEBUG
            if (ntf.rf_intf != NCI_RF_INTERFACE_NFCEE_DIRECT) {
                GDEBUG("  RF Protocol = 0x%02x", ntf.protocol);
                GDEBUG("  Activation RF Tech = 0x%02x", ntf.mode);
                GDEBUG("  Max Data Packet Size = %u", ntf.max_data_packet_size);
                GDEBUG("  Initial Credits = %u", ntf.num_credits);
                if (n || m) {
                    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
                        GString* buf = g_string_new(NULL);
                        guint i;

                        if (n) {
                            for (i = 0; i < n; i++) {
                                g_string_append_printf(buf, " %02x",
                                    mode_param_bytes[i]);
                            }
                            GDEBUG("  RF Tech Parameters =%s", buf->str);
                        }
                        GDEBUG("  Data Exchange RF Tech = 0x%02x",
                            ntf.data_exchange_mode);
                        if (m) {
                            g_string_set_size(buf, 0);
                            for (i = 0; i < m; i++) {
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
                ntf.mode_param = nci_state_discovery_parse_mode_param
                    (&mode_param, ntf.mode, mode_param_bytes, n);

                if (activation_param_bytes) {
                    memset(&activation_param, 0, sizeof(activation_param));
                    ntf.activation_param_len = m;
                    ntf.activation_param_bytes = activation_param_bytes;
                    ntf.activation_param =
                        nci_state_discovery_parse_activation_param
                            (&activation_param, ntf.rf_intf, ntf.mode,
                                 activation_param_bytes, m);
                }

                nci_sar_set_initial_credits(nci_sm_sar(sm),
                    NCI_STATIC_RF_CONN_ID, ntf.num_credits);
                nci_sm_enter_state(sm, NCI_RFST_POLL_ACTIVE, NULL);
                nci_sm_intf_activated(sm, &ntf);
                return;
            }
            GDEBUG("Missing RF Tech Parameters");
        }
    }

    /* Deactivate this target */
    GDEBUG("Failed to parse RF_INTF_ACTIVATED_NTF");
    nci_sm_enter_state(sm, NCI_RFST_POLL_ACTIVE, NULL);
    nci_sm_switch_to(sm, NCI_RFST_DISCOVERY);
}

/*==========================================================================*
 * Interface
 *==========================================================================*/

NciState*
nci_state_discovery_new(
    NciSm* sm)
{
    NciState* self = g_object_new(THIS_TYPE, NULL);

    nci_state_init_base(self, sm, NCI_RFST_DISCOVERY, "RFST_DISCOVERY");
    return self;
}

/*==========================================================================*
 * Methods
 *==========================================================================*/

static
void
nci_state_discovery_handle_ntf(
    NciState* self,
    guint8 gid,
    guint8 oid,
    const GUtilData* payload)
{
    switch (gid) {
    case NCI_GID_RF:
        switch (oid) {
        case NCI_OID_RF_INTF_ACTIVATED:
            nci_state_discovery_intf_activated_ntf(self, payload);
            return;
        case NCI_OID_RF_DEACTIVATE:
            nci_sm_handle_rf_deactivate_ntf(nci_state_sm(self), payload);
            return;
        }
        break;
    }
    NCI_STATE_CLASS(PARENT_CLASS)->handle_ntf(self, gid, oid, payload);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nci_state_discovery_init(
    NciStateDiscovery* self)
{
}

static
void
nci_state_discovery_class_init(
    NciStateDiscoveryClass* klass)
{
    NCI_STATE_CLASS(klass)->handle_ntf = nci_state_discovery_handle_ntf;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
