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

#include "binder_nfc.h"

#include <nci_core.h>

#include <nfc_tag.h>
#include <nfc_target_impl.h>

#include <gbinder.h>

#define T2T_CMD_READ (0x30)

enum {
    EVENT_DATA_PACKET,
    EVENT_COUNT
};

typedef struct binder_nfc_target_presence_check {
    BinderNfcTargetPresenseCheckFunc done;
    void* user_data;
} BinderNfcTargetPresenceCheck;

typedef
guint
(*BinderNfcTargetPresenceCheckFunc)(
    BinderNfcTarget* self,
    BinderNfcTargetPresenceCheck* check);

typedef NfcTargetClass BinderNfcTargetClass;
struct binder_nfc_target {
    NfcTarget target;
    GBinderRemoteObject* remote;
    NciCore* nci;
    NCI_RF_INTERFACE rf_intf;
    gulong event_id[EVENT_COUNT];
    guint send_in_progress;
    gboolean transmit_in_progress;
    BinderNfcTargetPresenceCheckFunc presence_check_fn;
};

G_DEFINE_TYPE(BinderNfcTarget, binder_nfc_target, NFC_TYPE_TARGET)

static
BinderNfcTargetPresenceCheck*
binder_nfc_target_presence_check_new(
    BinderNfcTargetPresenseCheckFunc fn,
    void* user_data)
{
    BinderNfcTargetPresenceCheck* check =
        g_slice_new(BinderNfcTargetPresenceCheck);

    check->done = fn;
    check->user_data = user_data;
    return check;
}

static
void
binder_nfc_target_presence_check_free(
    BinderNfcTargetPresenceCheck* check)
{
    g_slice_free(BinderNfcTargetPresenceCheck, check);
}

static
void
binder_nfc_target_presence_check_free1(
    gpointer data)
{
    binder_nfc_target_presence_check_free(data);
}

static
void
binder_nfc_target_drop_nci(
    BinderNfcTarget* self)
{
    if (self->send_in_progress) {
        nci_core_cancel(self->nci, self->send_in_progress);
        self->send_in_progress = 0;
    }
    nci_core_remove_all_handlers(self->nci, self->event_id);
    self->nci = NULL;
}

static
void
binder_nfc_target_data_sent(
    NciCore* nci,
    gboolean success,
    void* user_data)
{
    BinderNfcTarget* self = BINDER_NFC_TARGET(user_data);

    GASSERT(self->send_in_progress);
    self->send_in_progress = 0;
}

static
void
binder_nfc_target_data_packet_handler(
    NciCore* nci,
    guint8 cid,
    const void* data,
    guint len,
    void* user_data)
{
    BinderNfcTarget* self = BINDER_NFC_TARGET(user_data);

    if (cid == NCI_STATIC_RF_CONN_ID &&
        self->transmit_in_progress && !self->send_in_progress) {
        NfcTarget* target = &self->target;

        self->transmit_in_progress = FALSE;
        if (len > 0) {
            if (self->rf_intf == NCI_RF_INTERFACE_FRAME) {
                const guint8* payload = data;
                const guint8 status = payload[len - 1];

                /*
                 * 8.2 Frame RF Interface
                 * 8.2.1.2 Data from RF to the DH
                 */
                if (status == NCI_STATUS_OK) {
                    nfc_target_transmit_done(target, NFC_TRANSMIT_STATUS_OK,
                        data, len - 1);
                    return;
                }
                GDEBUG("Transmission status 0x%02x", status);
            } else if (self->rf_intf == NCI_RF_INTERFACE_ISO_DEP) {
                /*
                 * 8.3 ISO-DEP RF Interface
                 * 8.3.1.2 Data from RF to the DH
                 */
                nfc_target_transmit_done(target, NFC_TRANSMIT_STATUS_OK,
                    data, len);
                return;
            }
        }
        nfc_target_transmit_done(target, NFC_TRANSMIT_STATUS_ERROR, NULL, 0);
        return;
    }
    GDEBUG("Unhandled data packet, cid=0x%02x %u byte(s)", cid, len);
}

static
void
binder_nfc_target_presence_check_complete(
    NfcTarget* target,
    NFC_TRANSMIT_STATUS status,
    const void* data,
    guint len,
    void* user_data)
{
    BinderNfcTargetPresenceCheck* check = user_data;

    check->done(target, status == NFC_TRANSMIT_STATUS_OK, check->user_data);
}

static
guint
binder_nfc_target_presence_check_t2(
    BinderNfcTarget* self,
    BinderNfcTargetPresenceCheck* check)
{
    static const guint8 cmd_data[] = { T2T_CMD_READ, 0x00 };

    return nfc_target_transmit(&self->target, cmd_data, sizeof(cmd_data),
        NULL, binder_nfc_target_presence_check_complete,
        binder_nfc_target_presence_check_free1, check);
}

static
guint
binder_nfc_target_presence_check_t4(
    BinderNfcTarget* self,
    BinderNfcTargetPresenceCheck* check)
{
    return nfc_target_transmit(&self->target, NULL, 0,
        NULL, binder_nfc_target_presence_check_complete,
        binder_nfc_target_presence_check_free1, check);
}

/*==========================================================================*
 * Interface
 *==========================================================================*/

NfcTarget*
binder_nfc_target_new(
    GBinderRemoteObject* remote,
    const NciIntfActivationNtf* ntf,
    NciCore* nci)
{
     BinderNfcTarget* self = g_object_new(BINDER_NFC_TYPE_TARGET, NULL);
     NfcTarget* target = &self->target;

     switch (ntf->mode) {
     case NCI_MODE_PASSIVE_POLL_A:
     case NCI_MODE_ACTIVE_POLL_A:
     case NCI_MODE_PASSIVE_LISTEN_A:
     case NCI_MODE_ACTIVE_LISTEN_A:
         target->technology = NFC_TECHNOLOGY_A;
         break;
     case NCI_MODE_PASSIVE_POLL_B:
     case NCI_MODE_PASSIVE_LISTEN_B:
         target->technology = NFC_TECHNOLOGY_B;
         break;
     case NCI_MODE_PASSIVE_POLL_F:
     case NCI_MODE_PASSIVE_LISTEN_F:
     case NCI_MODE_ACTIVE_LISTEN_F:
         target->technology = NFC_TECHNOLOGY_F;
         break;
     default:
         break;
     }

     switch (ntf->protocol) {
     case NCI_PROTOCOL_T1T:
         target->protocol = NFC_PROTOCOL_T1_TAG;
         break;
     case NCI_PROTOCOL_T2T:
         target->protocol = NFC_PROTOCOL_T2_TAG;
         self->presence_check_fn = binder_nfc_target_presence_check_t2;
         break;
     case NCI_PROTOCOL_T3T:
         target->protocol = NFC_PROTOCOL_T3_TAG;
         break;
     case NCI_PROTOCOL_ISO_DEP:
         self->presence_check_fn = binder_nfc_target_presence_check_t4;
         switch (target->technology) {
         case NFC_TECHNOLOGY_A:
             target->protocol = NFC_PROTOCOL_T4A_TAG;
             break;
         case NFC_TECHNOLOGY_B:
             target->protocol = NFC_PROTOCOL_T4B_TAG;
             break;
         default:
             GDEBUG("Unexpected ISO_DEP technology");
             break;
         }
         break;
     case NCI_PROTOCOL_NFC_DEP:
         target->protocol = NFC_PROTOCOL_NFC_DEP;
         break;
     default:
         GDEBUG("Unexpected protocol 0x%02x", ntf->protocol);
         break;
     }

     self->remote = gbinder_remote_object_ref(remote);
     self->rf_intf = ntf->rf_intf;
     self->nci = nci;
     self->event_id[EVENT_DATA_PACKET] = nci_core_add_data_packet_handler(nci,
         binder_nfc_target_data_packet_handler, self);
     return target;
}

guint
binder_nfc_target_presence_check(
    BinderNfcTarget* self,
    BinderNfcTargetPresenseCheckFunc fn,
    void* user_data)
{
    if (G_LIKELY(self) && self->presence_check_fn) {
        BinderNfcTargetPresenceCheck* check =
            binder_nfc_target_presence_check_new(fn, user_data);
        const guint id = self->presence_check_fn(self, check);

        if (id) {
            return id;
        }
        binder_nfc_target_presence_check_free(check);
    }
    return 0;
}

/*==========================================================================*
 * Methods
 *==========================================================================*/

static
gboolean
binder_nfc_target_transmit(
    NfcTarget* target,
    const void* data,
    guint len)
{
    BinderNfcTarget* self = BINDER_NFC_TARGET(target);

    GASSERT(!self->send_in_progress);
    GASSERT(!self->transmit_in_progress);
    if (self->nci) {
        GBytes* bytes = g_bytes_new(data, len);

        self->send_in_progress = nci_core_send_data_msg(self->nci,
            NCI_STATIC_RF_CONN_ID, bytes, binder_nfc_target_data_sent,
            NULL, self);
        g_bytes_unref(bytes);
        if (self->send_in_progress) {
            self->transmit_in_progress = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

static
void
binder_nfc_target_cancel_transmit(
    NfcTarget* target)
{
    BinderNfcTarget* self = BINDER_NFC_TARGET(target);

    self->transmit_in_progress = FALSE;
    if (self->send_in_progress) {
        nci_core_cancel(self->nci, self->send_in_progress);
        self->send_in_progress = 0;
    }
}

static
void
binder_nfc_target_deactivate(
    NfcTarget* target)
{
    BinderNfcTarget* self = BINDER_NFC_TARGET(target);

    nci_core_set_state(self->nci, NCI_RFST_IDLE);
}

static
void
binder_nfc_target_gone(
    NfcTarget* target)
{
    binder_nfc_target_drop_nci(BINDER_NFC_TARGET(target));
    NFC_TARGET_CLASS(binder_nfc_target_parent_class)->gone(target);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_nfc_target_init(
    BinderNfcTarget* self)
{
}

static
void
binder_nfc_target_finalize(
    GObject* object)
{
    BinderNfcTarget* self = BINDER_NFC_TARGET(object);

    binder_nfc_target_drop_nci(self);
    gbinder_remote_object_unref(self->remote);
    G_OBJECT_CLASS(binder_nfc_target_parent_class)->finalize(object);
}

static
void
binder_nfc_target_class_init(
    NfcTargetClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = binder_nfc_target_finalize;
    klass->deactivate = binder_nfc_target_deactivate;
    klass->transmit = binder_nfc_target_transmit;
    klass->cancel_transmit = binder_nfc_target_cancel_transmit;
    klass->gone = binder_nfc_target_gone;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
