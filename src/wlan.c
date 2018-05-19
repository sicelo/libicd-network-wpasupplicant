/***********************************************************************************
*  libicd-network-wpasupplicant: Open source implementation of wpa_supplicant
*  integration icd2 on Maemo Leste
*  Copyright (C) 2018 Merlijn B. W. Wajer
*
*  This program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program; if not, write to the Free Software
*  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
***********************************************************************************/

/**
 * Some (but most is gone) code taken from older versions, which has:
 *
 * Copyright (C) 2007 Nokia Corporation. All rights reserved.
 *
 * @author patrik.flykt@nokia.com
 * @author jukka.rissanen@nokia.com
 *
 * @file wlan.c
 */

/** @addtogroup wlan WLAN network api implementation
 * @ingroup wlan_network_plugin
 * @{ */

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>

#include "../config.h"

#include <glib.h>
#include <dbus/dbus.h>

#include <osso-ic.h>
#include <osso-ic-dbus.h>
#include <osso-ic-gconf.h>
#include <icd/support/icd_log.h>
#include <icd/icd_wlan_defs.h>

#include <maemosec_certman.h>

#include <connui/libicd-network-wlan-dev.h>
#include "wlan.h"
#include "icd-common-utils.h"
#include "wpaicd.h"
#include "gconfmap.h"

#ifdef DMALLOC_ENABLE
#include <dmalloc.h>
#endif

#ifndef MINVAL
#define MINVAL(a, b) ((a) < (b) ? (a) : (b))
#endif

#if 0
#define SCAN_TIMEOUT 10
#else
#define SCAN_TIMEOUT 30         /* temporarily increased to 30 secs because of the problems in r0v3r scanning */
#endif
#define WLAN_PRIORITY 20

static void wlan_bring_up(const gchar * network_type,
                          const guint network_attrs,
                          const gchar * network_id,
                          icd_nw_link_up_cb_fn link_up_cb,
                          const gpointer link_up_cb_token, gpointer * private);
static gboolean wlan_scan_timeout(struct wlan_context *ctx);
static void wlan_set_state(struct wlan_context *ctx, iap_state state);

/* ------------------------------------------------------------------------- */
static enum icd_nw_levels map_rssi(int rssi)
{
    enum icd_nw_levels val = ICD_NW_LEVEL_NONE;

    if (rssi > -5)
        val = ICD_NW_LEVEL_10;
    else if (rssi > -10)
        val = ICD_NW_LEVEL_9;
    else if (rssi > -20)
        val = ICD_NW_LEVEL_8;
    else if (rssi > -30)
        val = ICD_NW_LEVEL_7;
    else if (rssi > -40)
        val = ICD_NW_LEVEL_6;
    else if (rssi > -50)
        val = ICD_NW_LEVEL_5;
    else if (rssi > -60)
        val = ICD_NW_LEVEL_4;
    else if (rssi > -70)
        val = ICD_NW_LEVEL_3;
    else if (rssi > -80)
        val = ICD_NW_LEVEL_2;
    else if (rssi > -90)
        val = ICD_NW_LEVEL_1;

    return val;
}

/* ------------------------------------------------------------------------- */
/**
 * Called from icd when wlan is to be taken down.
 * @param network_type network type
 * @param network_attrs attributes, such as type of network_id, security, etc.
 * @param network_id IAP name or local id, e.g. SSID
 * @param interface_name interface that was enabled
 * @param link_down_cb callback function for notifying ICd when the IP address
 *        is configured
 * @param link_down_cb_token token to pass to the callback function
 * @param private a reference to the icd_nw_api private memeber
 */
static void wlan_take_down(const gchar * network_type,
                           const guint network_attrs,
                           const gchar * network_id,
                           const gchar * interface_name,
                           icd_nw_link_down_cb_fn link_down_cb,
                           const gpointer link_down_cb_token,
                           gpointer * private)
{
    struct wlan_context *ctx = get_wlan_context_from_icd(private);

    (void)ctx;
    fprintf(stderr, "WLAN TAKE DOWN\n");

    wpaicd_remove_all_networks();
    link_down_cb(ICD_NW_SUCCESS_NEXT_LAYER, link_down_cb_token);

    return;
}

/* ------------------------------------------------------------------------- */
/** 
 * Called from icd when link should be brought up.
 * @param network_type network type
 * @param network_attrs attributes, such as type of network_id, security, etc.
 * @param network_id IAP name or local id, e.g. SSID
 * @param interface_name interface that was enabled
 * @param link_up_cb callback function for notifying ICd when the IP address
 *        is configured
 * @param link_up_cb_token token to pass to the callback function
 * @param private a reference to the icd_nw_api private memeber
 */

/* TODO: Doc mentions interface_name but function doesn't take it as argument */
static void wlan_bring_up(const gchar * network_type,
                          const guint network_attrs,
                          const gchar * network_id,
                          icd_nw_link_up_cb_fn link_up_cb,
                          const gpointer link_up_cb_token, gpointer * private)
{
    struct wlan_context *ctx = get_wlan_context_from_icd(private);

    /* I think that network_id here always has to come from gconf.
     * So we'll have to create a network for wpa_supplicant based on gconf here;
     * we should not (or, do not, I think?) need to scan for networks that match
     * the ssid, and based on that, create a map/profile */

    ENTER;

    fprintf(stderr, "wlan_bring_up: %s\n", network_id);

    GConfNetwork *net = get_gconf_network_iapname(ctx->gconf_client, network_id);
    if (net == NULL) {
        fprintf(stderr, "Cannot connect to network: net == NULL\n");
        link_up_cb(ICD_NW_ERROR, NULL, NULL, link_up_cb_token, NULL);

        return;
    }
    fprintf(stderr, "Got network: %s, %s\n", net->name, net->wlan_ssid);

    ctx->link_up_cb = link_up_cb;
    ctx->link_up_cb_token = link_up_cb_token;

    char *path = NULL;
    path = wpaicd_add_network(net);
    if (path == NULL) {
        fprintf(stderr, "wpaicd_add_network failed\n");
        link_up_cb(ICD_NW_ERROR, NULL, NULL, link_up_cb_token, NULL);
        goto fail1;
    }

    if (wpaicd_select_network(path)) {
        fprintf(stderr, "wpaicd_select_network failed\n");
        link_up_cb(ICD_NW_ERROR, NULL, NULL, link_up_cb_token, NULL);
        goto fail2;
    }

    wlan_set_state(ctx, STATE_CONNECTING);

    ctx->stored_network_type = network_type;
    ctx->stored_network_attrs = network_attrs;
    ctx->stored_network_id = network_id;

fail2:
    free(path);
fail1:
    free_gconf_network(net);

    EXIT;
    return;
}

/**
 * Callback to after WLAN active scan has been running too long.
 * @param data The IAP data.
 * @retval TRUE Success.
 * @retval FALSE Error.
 */
static gboolean wlan_scan_timeout(struct wlan_context *ctx)
{
    ENTER;

    fprintf(stderr, "wlan_scan_timeout\n");

    ctx->search_cb(ICD_NW_SEARCH_COMPLETE,
                   NULL,
                   NULL,
                   0, NULL, ICD_NW_LEVEL_NONE, NULL, 0, ctx->search_cb_token);

    ctx->scanning = FALSE;

    ctx->search_cb = NULL;
    ctx->search_cb_token = NULL;

    EXIT;
    return FALSE;
}

/* ----------------------------------------------------------------------- */
/**
 * Get link statistics.
 * @param network_type network type
 * @param network_attrs attributes, such as type of network_id, security, etc.
 * @param network_id IAP name or local id, e.g. SSID
 * @param private a reference to the icd_nw_api private memeber
 * @param stats_cb callback function when delivering the data
 * @param link_stats_cb_token token to pass to the callback function
 */
static void wlan_statistics(const gchar * network_type,
                            const guint network_attrs,
                            const gchar * network_id,
                            gpointer * private,
                            icd_nw_link_stats_cb_fn stats_cb,
                            const gpointer link_stats_cb_token)
{
    struct wlan_context *ctx = get_wlan_context_from_icd(private);

    ENTER;

    fprintf(stderr, "WLAN STATISTICS\n");

    // XXX: Also support stats for other states (?)
    if (ctx->state == STATE_IDLE) {
        return;
    }

    /* TODO: Check if network_id matches our current ssid */
    BssInfo *info = wpaicd_current_bss_info();

    if (info == NULL) {
        fprintf(stderr, "ctx->state != STATE_IDLE, but we have no bss?\n");
        return;
    }

    /* Dependent on the state, return info
     * Can we use 'CurrentNetwork' on the dbus interface here to get network
     * properties? Do we need them, aren't they already in gconf?
     */

    enum icd_nw_levels signal = map_rssi(info->signal);

    stats_cb(link_stats_cb_token, network_type, network_attrs, network_id, 0,   /* time_active */
             signal,            /* signal */
             "AAAAAA",          /* TODO: station id */
             info->signal,      /* dB */
             0,                 /* rx bytes */
             0 /* tx bytes */ );

    wpaicd_destroy_bss_info(info);

    EXIT;
    return;
}

static void wlan_state_change_cb(const char *state, void *data)
{
    struct wlan_context *ctx = get_wlan_context_from_wpaicd(data);

    fprintf(stderr, "wlan_state_change_cb: current state: %s\n", state);

    /*
       wpa_supplicant state of the interface. Possible values are:
       * "disconnected"
       * "inactive"
       * "scanning"
       * "authenticating"
       * "associating"
       * "associated"
       * "4way_handshake"
       * "group_handshake"
       * "completed"
       * "unknown".
     */

    /* TODO:
     * - remove network properties if disconnected/disconnecting*/

    /* XXX: wpa supplicant can on its own decide to reassociate without telling
     * us, I guess ? */

    /* XXX: Also look at current state */
    if (strcmp(state, "associating") == 0) {
        wlan_set_state(ctx, STATE_CONNECTING);
    } else if (strcmp(state, "disconnected") == 0) {
        wlan_set_state(ctx, STATE_IDLE);

        ctx->close_cb(ICD_NW_ERROR,
                      ICD_DBUS_ERROR_NETWORK_ERROR,
                      ctx->stored_network_type,
                      ctx->stored_network_attrs, ctx->stored_network_id);

        ctx->stored_network_type = NULL;
        ctx->stored_network_id = NULL;
        ctx->stored_network_attrs = 0;

    } else if (strcmp(state, "inactive") == 0) {
        wlan_set_state(ctx, STATE_IDLE);
    } else if (strcmp(state, "completed") == 0) {
        if (ctx->link_up_cb) {
            ILOG_DEBUG(WLAN "SENDING SUCCESS NEXT LAYER");
            ctx->link_up_cb(ICD_NW_SUCCESS_NEXT_LAYER, NULL, "wlan0",   /* FIXME */
                            ctx->link_up_cb_token, NULL);
            ctx->link_up_cb = NULL;
        }

        wlan_set_state(ctx, STATE_CONNECTED);
    }

    /*
     * close_cb function to inform ICd that the network connection is to be
     * closed
     */

    return;
}

static void wlan_search_network_added_cb(BssInfo * info, void *data)
{
    struct wlan_context *ctx = get_wlan_context_from_wpaicd(data);

    if (!ctx->scanning)
        return;

    /* TODO: Check all allocation */

    guint network_attrs = 0;
    char *network_id = NULL;
    char *network_name = NULL;

    gchar *ssid = calloc((info->ssid_len + 1), sizeof(char));
    memcpy(ssid, info->ssid, info->ssid_len);
    ssid[info->ssid_len] = '\0';

    enum icd_nw_levels signal = map_rssi(info->signal);

    network_attrs = wpaicd_bssinfo_to_network_attrs(info);

    GSList *iaps = get_gconf_networks(ctx->gconf_client);
    GSList *iap_iter = iaps;

    while (iap_iter) {
        GConfNetwork *net = (GConfNetwork *) iap_iter->data;
        if (net) {
            if (strncmp(net->type, "WLAN_INFRA", 10) != 0) {    // FIXME
                free_gconf_network(net);
                iap_iter = g_slist_next(iap_iter);
                continue;
            }

            /* TODO: Extend matching to include other attributes */
            if (strcmp(net->wlan_ssid, ssid) == 0) {
                fprintf(stderr, "MATCH FOR: %s\n", ssid);
                network_id = strdup(net->id);

                /* XXX: network_name can be NULL if added via the dialog?? */
                network_name = strdup(net->name);
                network_attrs |= ICD_NW_ATTR_IAPNAME;
                network_attrs |= ICD_NW_ATTR_AUTOCONNECT;
                free_gconf_network(net);
                break;
            }

            free_gconf_network(net);
        }

        iap_iter = g_slist_next(iap_iter);
    }
    g_slist_free(iaps);

    if (network_id == NULL) {
        network_id = strdup(ssid);
    }
    if (network_name == NULL) {
        network_name = strdup(ssid);
    }

    ctx->search_cb(ICD_NW_SEARCH_CONTINUE, network_name, 
                   info->infrastructure ? WLAN_TYPE_INFRA : WLAN_TYPE_ADHOC,
                   network_attrs, network_id, signal,
                   "AAAAAA", /* TODO station_id */
                   info->signal, ctx->search_cb_token);

    free(ssid);
    free(network_name);
    free(network_id);
}

static void wlan_search_scan_done_cb(int ret, void *data)
{
    struct wlan_context *ctx = get_wlan_context_from_wpaicd(data);

    fprintf(stderr, "SCAN DONE\n");
    if (!ctx->scanning)
        return;

    wlan_scan_timeout(ctx);
}

/* ----------------------------------------------------------------------- */
/**
 * Start wlan search
 * @param network_type network type to search for or NULL for all networks
 * @param search_scope scope for network search, not used currently
 * @param search_cb the search callback
 * @param search_cb_token token from the ICd to pass to the callback
 * @private a reference to the icd_nw_api private member
 */
static void wlan_start_search(const gchar * network_type,
                              guint search_scope,
                              icd_nw_search_cb_fn search_cb,
                              const gpointer search_cb_token,
                              gpointer * private)
{
    struct wlan_context *ctx = get_wlan_context_from_icd(private);

    fprintf(stderr, "STARTING SEARCH\n");

    if (ctx->scanning) {
        goto scan_failed;
    }

    ENTER;

    ctx->scanning = TRUE;
    ctx->search_cb = search_cb;
    ctx->search_cb_token = search_cb_token;

    int ret = wpaicd_initiate_scan();
    if (ret == 0)
        goto done;

 scan_failed:
    fprintf(stderr, "Starting scan failed.\n");
    wlan_scan_timeout(ctx);

    ctx->search_cb = NULL;
    ctx->search_cb_token = NULL;

    ctx->scanning = FALSE;

 done:
    EXIT;
    return;
}

/* ----------------------------------------------------------------------- */
/**
 * Function for stopping an ongoing search
 * @param search_cb the search callback
 * @param search_cb_token token from the ICd to pass to the callback
 * @private a reference to the icd_nw_api private member
 */
static void wlan_stop_search(gpointer * private)
{
    struct wlan_context *ctx = get_wlan_context_from_icd(private);

    ENTER;

    wlan_scan_timeout(ctx);

    EXIT;
}

/* ------------------------------------------------------------------------- */
/**
 * Initialize GConfClient connection for icd_context.
 * Creates default GConf client and preloads some of the required keys.
 * @param ctx ICD context to initialize
 * @retval TRUE Success
 * @retval FALSE Failure
 */
static gboolean wlan_gconf_init(struct wlan_context *ctx)
{
    /* GConf init */
    ctx->gconf_client = gconf_client_get_default();
    if (ctx->gconf_client == NULL) {
        ILOG_ERR(WLAN "%s", "Failed to connect to GConf");
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/**
 * Clean up wlan module.
 */
static void wlan_destruct(gpointer * private)
{
    fprintf(stderr, "DESTRUCT\n");

    ENTER;

    wpaicd_free();
    /* TODO: Free context->gconf_client */

    EXIT;
}

static void wlan_set_state(struct wlan_context *ctx, iap_state state)
{
    fprintf(stderr, "wlan_set_state: %d -> %d\n", ctx->state, state);
    ctx->state = state;
}

/* ------------------------------------------------------------------------- */
/**
 * Initialize WLAN network module
 * @param network_api icd_nw_api structure filled in by the module
 * @param watch_cb function to inform ICd that a child process is to be
 *        monitored for exit status
 * @param watch_cb_token token to pass to the watch pid function
 * @param close_cb function to inform ICd that the network connection is to be
 *        closed
 * @return TRUE on succes; FALSE on failure whereby the module is unloaded
 */
gboolean icd_nw_init(struct icd_nw_api *network_api,
                     icd_nw_watch_pid_fn watch_cb,
                     gpointer watch_cb_token, icd_nw_close_fn close_cb)
{
    struct wlan_context *context;

    fprintf(stderr, "%s initializing\n", PACKAGE_STRING);

    network_api->version = ICD_NW_MODULE_VERSION;
    network_api->link_down = wlan_take_down;
    network_api->link_up = wlan_bring_up;
    network_api->start_search = wlan_start_search;
    network_api->stop_search = wlan_stop_search;
    network_api->network_destruct = wlan_destruct;
    network_api->link_stats = wlan_statistics;

    context = (struct wlan_context *)g_malloc0(sizeof(struct wlan_context));
    if (!context) {
        errno = ENOMEM;
        return FALSE;
    }

    network_api->search_interval = 10;
    network_api->search_lifetime = 2 * network_api->search_interval;    // XXX: fixme

#if 0
    /* TODO: watch_cb is never used? */
    context->watch_cb = watch_cb;
#endif
    context->close_cb = close_cb;

    network_api->private = context;

    if (wpaicd_init()) {
        fprintf(stderr, "Failed to set up wpaicd\n");
        g_free(context);
        return FALSE;
    }

    if (!wlan_gconf_init(context)) {
        g_free(context);
        return FALSE;
    }

    wpaicd_set_network_added_cb(wlan_search_network_added_cb, (void *)context);
    wpaicd_set_scan_done_cb(wlan_search_scan_done_cb, (void *)context);
    wpaicd_set_state_change_cb(wlan_state_change_cb, (void *)context);

    wlan_set_state(context, STATE_IDLE);

    /* TODO: Check if we can communicate with wpa_supplicant? */
    return TRUE;
}

/** @} */
