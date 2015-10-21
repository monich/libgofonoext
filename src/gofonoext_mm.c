/*
 * Copyright (C) 2015 Jolla Ltd.
 * Contact: Slava Monich <slava.monich@jolla.com>
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
 *   3. Neither the name of the Jolla Ltd nor the names of its contributors
 *      may be used to endorse or promote products derived from this software
 *      without specific prior written permission.
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

#include "gofonoext_mm.h"
#include "gofonoext_log.h"

#include <gofono_modem.h>
#include <gofono_names.h>

/* Generated headers */
#include "org.nemomobile.ofono.ModemManager.h"

/* Log module */
GLOG_MODULE_DEFINE("ofonoext");

/* Object definition */
enum proxy_handler_id {
    PROXY_SIGNAL_ENABLED_MODEMS_CHANGED,
    PROXY_SIGNAL_DATA_IMSI_CHANGED,
    PROXY_SIGNAL_DATA_MODEM_CHANGED,
    PROXY_SIGNAL_VOICE_IMSI_CHANGED,
    PROXY_SIGNAL_VOICE_MODEM_CHANGED,
    PROXY_SIGNAL_COUNT
};

struct ofonoext_mm_priv {
    GDBusConnection* bus;
    OrgNemomobileOfonoModemManager* proxy;
    gulong proxy_signal_id[PROXY_SIGNAL_COUNT];
    guint ofono_watch_id;
    GCancellable* cancel;
    char** available;
    char** enabled;
    char* data_imsi;
    char* voice_imsi;
};

typedef GObjectClass OfonoExtModemManagerClass;
G_DEFINE_TYPE(OfonoExtModemManager, ofonoext_mm, G_TYPE_OBJECT)

enum ofonoext_mm_signal {
    SIGNAL_VALID_CHANGED,
    SIGNAL_ENABLED_MODEMS_CHANGED,
    SIGNAL_DATA_IMSI_CHANGED,
    SIGNAL_DATA_MODEM_CHANGED,
    SIGNAL_VOICE_IMSI_CHANGED,
    SIGNAL_VOICE_MODEM_CHANGED,
    SIGNAL_COUNT
};

#define SIGNAL_VALID_CHANGED_NAME           "valid-changed"
#define SIGNAL_ENABLED_MODEMS_CHANGED_NAME  "enabled-modems-changed"
#define SIGNAL_DATA_IMSI_CHANGED_NAME       "data-imsi-changed"
#define SIGNAL_DATA_MODEM_CHANGED_NAME      "data-modem-changed"
#define SIGNAL_VOICE_IMSI_CHANGED_NAME      "voice-imsi-changed"
#define SIGNAL_VOICE_MODEM_CHANGED_NAME     "voice-modem-changed"

static guint ofonoext_mm_signals[SIGNAL_COUNT] = { 0 };

#define OFONOEXT_SIGNAL_NEW(NAME) \
    ofonoext_mm_signals[SIGNAL_##NAME##_CHANGED] = \
        g_signal_new(SIGNAL_##NAME##_CHANGED_NAME, \
            G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, 0, \
            NULL, NULL, NULL, G_TYPE_NONE, 0)

/* Weak reference to the single instance of OfonoExtModemManager */
static OfonoExtModemManager* ofonoext_mm_instance = NULL;

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
void
ofonoext_mm_destroyed(
    gpointer arg,
    GObject* object)
{
    GASSERT(object == (GObject*)ofonoext_mm_instance);
    ofonoext_mm_instance = NULL;
    GVERBOSE_("%p", object);
}

static
void
ofonoext_mm_set_valid(
    OfonoExtModemManager* self,
    gboolean valid)
{
    if (self->valid != valid) {
        self->valid = valid;
        g_signal_emit(self, ofonoext_mm_signals[SIGNAL_VALID_CHANGED], 0);
    }
}

static
void
ofonoext_mm_reset(
    OfonoExtModemManager* self)
{
    OfonoExtModemManagerPriv* priv = self->priv;
    if (priv->proxy) {
        unsigned int i;
        for (i=0; i<G_N_ELEMENTS(priv->proxy_signal_id); i++) {
            if (priv->proxy_signal_id[i]) {
                g_signal_handler_disconnect(priv->proxy,
                    priv->proxy_signal_id[i]);
                priv->proxy_signal_id[i] = 0;
            }
        }
        g_object_unref(priv->proxy);
        priv->proxy = NULL;
    }
    if (self->available) {
        g_strfreev(priv->available);
        self->available = priv->available = NULL;
    }
    if (self->enabled) {
        g_strfreev(priv->enabled);
        self->enabled = priv->enabled = NULL;
    }
    if (self->data_imsi) {
        g_free(priv->data_imsi);
        self->data_imsi = priv->data_imsi = NULL;
    }
    if (self->voice_imsi) {
        g_free(priv->voice_imsi);
        self->voice_imsi = priv->voice_imsi = NULL;
    }
    if (self->data_modem) {
        ofono_modem_unref(self->data_modem);
        self->data_modem = NULL;
    }
    if (self->voice_modem) {
        ofono_modem_unref(self->voice_modem);
        self->voice_modem = NULL;
    }
}

static
void
ofonoext_mm_get_all_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer data)
{
    GError* error = NULL;
    OfonoExtModemManager* self = data;
    OfonoExtModemManagerPriv* priv = self->priv;
    int version = 0;
    char** available = NULL;
    char** enabled = NULL;
    char* data_imsi = NULL;
    char* voice_imsi = NULL;
    char* data_modem = NULL;
    char* voice_modem = NULL;

    GASSERT(!self->valid);
    GASSERT(priv->cancel);
    g_object_unref(priv->cancel);
    priv->cancel = NULL;
    if (org_nemomobile_ofono_modem_manager_call_get_all_finish(
        ORG_NEMOMOBILE_OFONO_MODEM_MANAGER(proxy), &version, &available,
        &enabled, &data_imsi, &voice_imsi, &data_modem, &voice_modem,
        result, &error)) {
        OfonoModem* modem;

        g_strfreev(priv->available);
        g_strfreev(priv->enabled);
        g_free(priv->data_imsi);
        g_free(priv->voice_imsi);
        self->available = priv->available = available;
        self->enabled = priv->enabled = enabled;
        self->data_imsi = priv->data_imsi = data_imsi;
        self->voice_imsi = priv->voice_imsi = voice_imsi;

        modem = self->data_modem;
        self->data_modem = (data_modem && data_modem[0]) ?
            ofono_modem_new(data_modem) : NULL;
        ofono_modem_unref(modem);

        modem = self->voice_modem;
        self->voice_modem = (voice_modem && voice_modem[0]) ?
            ofono_modem_new(voice_modem) : NULL;
        ofono_modem_unref(modem);

        ofonoext_mm_set_valid(self, TRUE);
    } else {
        g_strfreev(available);
        g_strfreev(enabled);
        g_free(data_imsi);
        g_free(voice_imsi);
#if GUTIL_LOG_ERR
        if (error->code == G_IO_ERROR_CANCELLED) {
            GDEBUG("%s", GERRMSG(error));
        } else {
            GERR("%s", GERRMSG(error));
        }
#endif
    }
    g_free(data_modem);
    g_free(voice_modem);
    ofonoext_mm_unref(self);
    if (error) g_error_free(error);
}

static
void
ofonoext_mm_enabled_modems_changed(
    OrgNemomobileOfonoModemManager* proxy,
    char** modems,
    gpointer data)
{
    OfonoExtModemManager* self = data;
    OfonoExtModemManagerPriv* priv = self->priv;
    g_strfreev(priv->enabled);
    self->enabled = priv->enabled = g_strdupv(modems);
    g_signal_emit(self, ofonoext_mm_signals[SIGNAL_ENABLED_MODEMS_CHANGED], 0);
}

static
void
ofonoext_mm_default_data_sim_changed(
    OrgNemomobileOfonoModemManager* proxy,
    const char* imsi,
    gpointer data)
{
    OfonoExtModemManager* self = data;
    OfonoExtModemManagerPriv* priv = self->priv;
    g_free(priv->data_imsi);
    self->data_imsi = priv->data_imsi = g_strdup(imsi);
    g_signal_emit(self, ofonoext_mm_signals[
        SIGNAL_DATA_IMSI_CHANGED], 0);
}

static
void
ofonoext_mm_default_data_modem_changed(
    OrgNemomobileOfonoModemManager* proxy,
    const char* path,
    gpointer data)
{
    OfonoExtModemManager* self = data;
    ofono_modem_unref(self->data_modem);
    self->data_modem = (path && path[0]) ? ofono_modem_new(path) : NULL;
    g_signal_emit(self, ofonoext_mm_signals[
        SIGNAL_DATA_MODEM_CHANGED], 0);
}

static
void
ofonoext_mm_default_voice_sim_changed(
    OrgNemomobileOfonoModemManager* proxy,
    const char* imsi,
    gpointer data)
{
    OfonoExtModemManager* self = data;
    OfonoExtModemManagerPriv* priv = self->priv;
    g_free(priv->voice_imsi);
    self->voice_imsi = priv->voice_imsi = g_strdup(imsi);
    g_signal_emit(self, ofonoext_mm_signals[
        SIGNAL_VOICE_IMSI_CHANGED], 0);
}

static
void
ofonoext_mm_default_voice_modem_changed(
    OrgNemomobileOfonoModemManager* proxy,
    const char* path,
    gpointer data)
{
    OfonoExtModemManager* self = data;
    ofono_modem_unref(self->voice_modem);
    self->voice_modem = (path && path[0]) ? ofono_modem_new(path) : NULL;
    g_signal_emit(self, ofonoext_mm_signals[
        SIGNAL_VOICE_MODEM_CHANGED], 0);
}

static
void
ofonoext_mm_proxy_created(
    GObject* proxy,
    GAsyncResult* result,
    gpointer data)
{
    GError* error = NULL;
    OfonoExtModemManager* self = data;
    OfonoExtModemManagerPriv* priv = self->priv;

    GASSERT(priv->cancel);
    GASSERT(!self->valid);
    GASSERT(!priv->proxy);
    g_object_unref(priv->cancel);
    priv->cancel = NULL;
    priv->proxy =
        org_nemomobile_ofono_modem_manager_proxy_new_finish(result, &error);
    
    if (priv->proxy) {
        priv->cancel = g_cancellable_new();

        /* Subscribe for notifications */
        priv->proxy_signal_id[PROXY_SIGNAL_ENABLED_MODEMS_CHANGED] =
            g_signal_connect(proxy, "enabled-modems-changed",
            G_CALLBACK(ofonoext_mm_enabled_modems_changed), self);
        priv->proxy_signal_id[PROXY_SIGNAL_DATA_IMSI_CHANGED] =
            g_signal_connect(proxy, "default-data-sim-changed",
            G_CALLBACK(ofonoext_mm_default_data_sim_changed), self);
        priv->proxy_signal_id[PROXY_SIGNAL_DATA_MODEM_CHANGED] =
            g_signal_connect(proxy, "default-data-modem-changed",
            G_CALLBACK(ofonoext_mm_default_data_modem_changed), self);
        priv->proxy_signal_id[PROXY_SIGNAL_VOICE_IMSI_CHANGED] =
            g_signal_connect(proxy, "default-voice-sim-changed",
            G_CALLBACK(ofonoext_mm_default_voice_sim_changed), self);
        priv->proxy_signal_id[PROXY_SIGNAL_VOICE_MODEM_CHANGED] =
            g_signal_connect(proxy, "default-voice-modem-changed",
            G_CALLBACK(ofonoext_mm_default_voice_modem_changed), self);

        /* Request current settings */
        org_nemomobile_ofono_modem_manager_call_get_all(priv->proxy,
            priv->cancel, ofonoext_mm_get_all_done, self);
    } else {
#if GUTIL_LOG_ERR
        if (error->code == G_IO_ERROR_CANCELLED) {
            GDEBUG("%s", GERRMSG(error));
        } else {
            GERR("%s", GERRMSG(error));
        }
#endif
        ofonoext_mm_reset(self);
        ofonoext_mm_unref(self);
    }
    if (error) g_error_free(error);
}

static
void
ofonoext_mm_name_appeared(
    GDBusConnection* bus,
    const gchar* name,
    const gchar* owner,
    gpointer arg)
{
    OfonoExtModemManager* self = OFONOEXT_MODEM_MANAGER(arg);
    OfonoExtModemManagerPriv* priv = self->priv;
    GDEBUG("Name '%s' is owned by %s", name, owner);

    /* Start the initialization sequence */
    GASSERT(!priv->cancel);
    priv->cancel = g_cancellable_new();
    org_nemomobile_ofono_modem_manager_proxy_new(bus, G_DBUS_PROXY_FLAGS_NONE,
        OFONO_SERVICE, "/", priv->cancel, ofonoext_mm_proxy_created,
        ofonoext_mm_ref(self));
}

static
void
ofonoext_mm_name_vanished(
    GDBusConnection* bus,
    const gchar* name,
    gpointer arg)
{
    OfonoExtModemManager* self = OFONOEXT_MODEM_MANAGER(arg);
    GDEBUG("Name '%s' has disappeared", name);
    ofonoext_mm_reset(self);
    ofonoext_mm_set_valid(self, FALSE);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static
OfonoExtModemManager*
ofonoext_mm_create()
{
    GError* error = NULL;
    GDBusConnection* bus = g_bus_get_sync(OFONO_BUS_TYPE, NULL, &error);
    if (bus) {
        OfonoExtModemManager* self =
            g_object_new(OFONOEXT_TYPE_MODEM_MANAGER, NULL);
        OfonoExtModemManagerPriv* priv = self->priv;
        priv->bus = bus;
        priv->ofono_watch_id = g_bus_watch_name_on_connection(bus,
            OFONO_SERVICE, G_BUS_NAME_WATCHER_FLAGS_NONE,
            ofonoext_mm_name_appeared,
            ofonoext_mm_name_vanished,
            self, NULL);
        return self;
    } else {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
    }
    return NULL;
}

OfonoExtModemManager*
ofonoext_mm_new()
{
    OfonoExtModemManager* mm;
    if (ofonoext_mm_instance) {
        g_object_ref(mm = ofonoext_mm_instance);
    } else {
        mm = ofonoext_mm_create();
        ofonoext_mm_instance = mm;
        g_object_weak_ref(G_OBJECT(mm), ofonoext_mm_destroyed, mm);
    }
    return mm;
}

OfonoExtModemManager*
ofonoext_mm_ref(
    OfonoExtModemManager* self)
{
    if (G_LIKELY(self)) {
        g_object_ref(OFONOEXT_MODEM_MANAGER(self));
        return self;
    } else {
        return NULL;
    }
}

void
ofonoext_mm_unref(
    OfonoExtModemManager* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(OFONOEXT_MODEM_MANAGER(self));
    }
}
gulong
ofonoext_mm_add_valid_changed_handler(
    OfonoExtModemManager* self,
    OfonoExtModemManagerHandler fn,
    void* data)
{
    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_VALID_CHANGED_NAME, G_CALLBACK(fn), data) : 0;
}

gulong
ofonoext_mm_add_enabled_modems_changed_handler(
    OfonoExtModemManager* self,
    OfonoExtModemManagerHandler fn,
    void* data)
{
    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_ENABLED_MODEMS_CHANGED_NAME, G_CALLBACK(fn), data) : 0;
}

gulong
ofonoext_mm_add_data_imsi_changed_handler(
    OfonoExtModemManager* self,
    OfonoExtModemManagerHandler fn,
    void* data)
{
    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_DATA_IMSI_CHANGED_NAME, G_CALLBACK(fn), data) : 0;
}

gulong
ofonoext_mm_add_data_modem_changed_handler(
    OfonoExtModemManager* self,
    OfonoExtModemManagerHandler fn,
    void* data)
{
    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_DATA_MODEM_CHANGED_NAME, G_CALLBACK(fn), data) : 0;
}

gulong
ofonoext_mm_add_voice_imsi_changed_handler(
    OfonoExtModemManager* self,
    OfonoExtModemManagerHandler fn,
    void* data)
{
    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_VOICE_IMSI_CHANGED_NAME, G_CALLBACK(fn), data) : 0;
}

gulong
ofonoext_mm_add_voice_modem_changed_handler(
    OfonoExtModemManager* self,
    OfonoExtModemManagerHandler fn,
    void* data)
{
    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_VOICE_MODEM_CHANGED_NAME, G_CALLBACK(fn), data) : 0;
}

void
ofonoext_mm_remove_handler(
    OfonoExtModemManager* self,
    gulong id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        g_signal_handler_disconnect(self, id);
    }
}

void
ofonoext_mm_remove_handlers(
    OfonoExtModemManager* self,
    gulong* ids,
    unsigned int count)
{
    if (G_LIKELY(self)) {
        unsigned int i;
        for (i=0; i<count; i++) {
            if (G_LIKELY(ids[i])) {
                g_signal_handler_disconnect(self, ids[i]);
                ids[i] = 0;
            }
        }
    }
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

/**
 * Per instance initializer
 */
static
void
ofonoext_mm_init(
    OfonoExtModemManager* self)
{
    OfonoExtModemManagerPriv* priv = G_TYPE_INSTANCE_GET_PRIVATE(self,
        OFONOEXT_TYPE_MODEM_MANAGER, OfonoExtModemManagerPriv);
    self->priv = priv;
}

/**
 * First stage of deinitialization (release all references).
 * May be called more than once in the lifetime of the object.
 */
static
void
ofonoext_mm_dispose(
    GObject* object)
{
    OfonoExtModemManager* self = OFONOEXT_MODEM_MANAGER(object);
    OfonoExtModemManagerPriv* priv = self->priv;
    ofonoext_mm_reset(self);
    if (priv->ofono_watch_id) {
        g_bus_unwatch_name(priv->ofono_watch_id);
        priv->ofono_watch_id = 0;
    }
    if (priv->bus) {
        g_object_unref(priv->bus);
        priv->bus = NULL;
    }
    G_OBJECT_CLASS(ofonoext_mm_parent_class)->dispose(object);
}

/**
 * Final stage of deinitialization
 */
static
void
ofonoext_mm_finalize(
    GObject* object)
{
    OfonoExtModemManager* self = OFONOEXT_MODEM_MANAGER(object);
    OfonoExtModemManagerPriv* priv = self->priv;
    GASSERT(!priv->cancel);
    GASSERT(!priv->bus);
    GASSERT(!priv->proxy);
    g_strfreev(priv->available);
    g_strfreev(priv->enabled);
    g_free(priv->data_imsi);
    g_free(priv->voice_imsi);
    G_OBJECT_CLASS(ofonoext_mm_parent_class)->finalize(object);
}

/**
 * Per class initializer
 */
static
void
ofonoext_mm_class_init(
    OfonoExtModemManagerClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = ofonoext_mm_dispose;
    object_class->finalize = ofonoext_mm_finalize;
    g_type_class_add_private(klass, sizeof(OfonoExtModemManagerPriv));
    OFONOEXT_SIGNAL_NEW(VALID);
    OFONOEXT_SIGNAL_NEW(ENABLED_MODEMS);
    OFONOEXT_SIGNAL_NEW(DATA_IMSI);
    OFONOEXT_SIGNAL_NEW(DATA_MODEM);
    OFONOEXT_SIGNAL_NEW(VOICE_IMSI);
    OFONOEXT_SIGNAL_NEW(VOICE_MODEM);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */