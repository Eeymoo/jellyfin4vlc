/*****************************************************************************
 * jellyfin.c: VLC interface module syncing playback with a Jellyfin server
 *****************************************************************************
 * Copyright (C) 2024 jellyfin4vlc authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*
 * Minimal Jellyfin integration for VLC 3.0.x:
 *   1. configure server URL + user, login (AuthenticateByName)
 *   2. optionally browse: load the whole Movie/Episode library into the
 *      playlist as direct-stream URLs
 *   3. watch the playlist input; when a Jellyfin-backed file/stream is
 *      playing, report Playing/Progress/Stopped to /Sessions/Playing*
 *
 * Login and library fetching happen on the background thread (with retry),
 * so a slow/unreachable server never delays VLC startup.
 *
 * Modeled after modules/misc/audioscrobbler.c.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_input.h>
#include <vlc_playlist.h>
#include <vlc_services_discovery.h>
#include <vlc_url.h>
#include <vlc_threads.h>

#include "jf_api.h"

#define CFG_PREFIX "jellyfin-"

/* VLC builds normally get these from the build system's config.h */
#ifndef N_
# define N_(str) (str)
#endif
#ifndef MODULE_STRING
# define MODULE_STRING "jellyfin"
#endif

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open (vlc_object_t *);
static void Close(vlc_object_t *);
static int  OpenSD (vlc_object_t *);
static void CloseSD(vlc_object_t *);

vlc_module_begin()
    set_shortname(N_("Jellyfin"))
    set_description(N_("Jellyfin playback sync"))
    set_capability("interface", 0)
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)

    set_section(N_("Server"), NULL)
    add_string(CFG_PREFIX"server", "",
               N_("Jellyfin server URL"),
               N_("Base URL of the Jellyfin server, e.g. http://192.168.1.10:8096 "
                  "or http://example.com/jellyfin (reverse proxy subpath)."),
               false)
    add_string(CFG_PREFIX"username", "",
               N_("Jellyfin user name"),
               N_("User name used to authenticate against the Jellyfin server. "
                  "Only needed on the first run; a token is cached afterwards."),
               false)
    add_password(CFG_PREFIX"password", "",
                 N_("Jellyfin password"),
                 N_("Password used to authenticate against the Jellyfin server. "
                    "Only needed on the first run; a token is cached afterwards."),
                 false)

    set_section(N_("Behaviour"), NULL)
    add_bool(CFG_PREFIX"browse", false,
             N_("Load media library into the playlist on startup"),
             N_("Fetch all movies/episodes from the server and append them "
                "to the playlist as direct stream entries."),
             false)
    add_bool(CFG_PREFIX"basename-fallback", true,
             N_("Match local files by filename when the full path differs"),
             N_("Needed when the client cannot see the server's mount layout "
                "(e.g. Windows client, Linux server). May mismatch "
                "identically-named files in different folders; disable for "
                "strict full-path matching only."),
             false)
    add_integer_with_range(CFG_PREFIX"interval", 10, 5, 120,
             N_("Progress report interval (seconds)"),
             N_("How often playback progress is reported to the server."),
             false)

    set_section(N_("Advanced (auto-cached, no need to edit)"), NULL)
    add_string(CFG_PREFIX"token", "",
               N_("Jellyfin access token (cached)"),
               N_("Access token saved from a previous login; avoids re-sending "
                  "the password. Filled in automatically after a successful "
                  "login with user/password."),
               true)
    add_string(CFG_PREFIX"userid", "",
               N_("Jellyfin user id (cached)"),
               N_("User id belonging to the cached access token. "
                  "Filled in automatically after a successful login."),
               true)
    add_string(CFG_PREFIX"device-id", "",
               N_("Device id reported to the server"),
               N_("Stable identifier of this installation; generated once "
                  "and cached automatically if left empty."),
               true)

    set_callbacks(Open, Close)

    /* Sidebar "Internet" node: browse the Jellyfin library as a tree
     * (movies grouped under 电影, episodes grouped per series). */
    add_submodule()
        set_shortname(N_("Jellyfin"))
        set_description(N_("Jellyfin media library"))
        set_capability("services_discovery", 0)
        set_category(CAT_PLAYLIST)
        set_subcategory(SUBCAT_PLAYLIST_SD)
        set_callbacks(OpenSD, CloseSD)
vlc_module_end()

/*****************************************************************************
 * Local state
 *****************************************************************************/

/* intf_sys_t is forward-declared (opaque) in vlc_interface.h, so we only
 * provide the struct body here. */
struct intf_sys_t
{
    vlc_thread_t   thread;
    vlc_mutex_t    lock;
    bool           b_die;

    playlist_t    *playlist;
    jf_client_t    client;
    jf_item_list_t library;       /* path -> item cache, may be empty      */

    /* connection settings, owned until the thread is done with them */
    char          *cfg_server;
    char          *cfg_username;
    char          *cfg_password;
    char          *cfg_token;
    char          *cfg_userid;
    char          *cfg_device;

    bool           basename_fallback;
    bool           browse_on_start;
    bool           ready;         /* login + library done                  */

    /* currently tracked playback (owned by the background thread) */
    input_thread_t *last_input;   /* pointer identity: new input = replay  */
    char          *last_uri;      /* URI of the tracked input item         */
    char          *last_item_id;  /* Jellyfin item id currently reported   */
    int64_t        last_ticks;    /* last known position in Jellyfin ticks */
    vlc_tick_t     last_report;   /* last progress report time             */
    int            interval;      /* seconds between progress reports      */
};

/*****************************************************************************
 * Helpers
 *****************************************************************************/

/* Sleep for the given seconds while watching a b_die flag; true = exit. */
static bool jf_die_wait(vlc_mutex_t *lock, bool *die_flag, int seconds)
{
    for (int i = 0; i < seconds * 2; i++)
    {
        vlc_mutex_lock(lock);
        bool die = *die_flag;
        vlc_mutex_unlock(lock);
        if (die)
            return true;
        msleep(MS_FROM_VLC_TICK(500));
    }
    return false;
}

/* Return a malloc'd copy of the file path of a "file://" URI (percent
 * decoded), or NULL for non-file URIs. */
static char *jf_uri_to_path(const char *uri)
{
    if (uri == NULL || strncmp(uri, "file://", 7) != 0)
        return NULL;
    const char *enc = uri + 7;
    /* skip authority (usually empty for local files) */
    const char *slash = strchr(enc, '/');
    if (slash == NULL)
        return NULL;
    char *path = strdup(slash);
    if (path == NULL)
        return NULL;
    if (vlc_uri_decode(path) == NULL)
    {
        free(path);
        return NULL;
    }
    return path;
}

/* If the URI is one of our direct stream URLs
 * ({server}/Videos/{id}/stream...), extract the item id. */
static char *jf_uri_to_item_id(const intf_sys_t *sys, const char *uri)
{
    const char *base = sys->client.server;
    if (base == NULL || uri == NULL)
        return NULL;

    size_t blen = strlen(base);
    if (strncmp(uri, base, blen) != 0)
        return NULL;
    const char *rest = uri + blen;
    if (strncmp(rest, "/Videos/", 8) != 0)
        return NULL;
    rest += 8;

    const char *end = rest;
    while (*end && *end != '/')
        end++;
    if (end == rest)
        return NULL;

    return strndup(rest, end - rest);
}

/* Resolve a playing input item to a Jellyfin item id (malloc'd or NULL). */
static char *jf_resolve_input(intf_sys_t *sys, input_item_t *item)
{
    char *uri = input_item_GetURI(item);
    if (uri == NULL)
        return NULL;

    /* 1. direct stream URL from the browse feature */
    char *id = jf_uri_to_item_id(sys, uri);
    if (id == NULL)
    {
        /* 2. local file path matched against the library Paths */
        char *path = jf_uri_to_path(uri);
        if (path != NULL)
        {
            id = jf_item_id_for_path(&sys->library, path,
                                     sys->basename_fallback);
            free(path);
        }
    }
    free(uri);
    return id;
}

static void jf_report_stopped_and_clear(intf_sys_t *sys)
{
    if (sys->last_item_id != NULL)
    {
        /* best effort final report with the last known position */
        jf_report_stopped(&sys->client, sys->last_item_id, sys->last_ticks);
        free(sys->last_item_id);
        sys->last_item_id = NULL;
    }
    free(sys->last_uri);
    sys->last_uri = NULL;
    sys->last_input = NULL;
    sys->last_ticks = 0;
}

/* Append library entries to the playlist as direct stream URLs. */
static void jf_browse(intf_sys_t *sys)
{
    msg_Dbg(sys->playlist, "jellyfin: browsing library (%zu items)",
            sys->library.count);

    playlist_Lock(sys->playlist);
    for (size_t i = 0; i < sys->library.count; i++)
    {
        const jf_item_t *it = &sys->library.items[i];
        char *url = jf_stream_url(&sys->client, it->id);
        if (url == NULL)
            continue;
        input_item_t *node = input_item_New(url, it->name);
        if (node != NULL)
        {
            playlist_AddInput(sys->playlist, node, false, false);
            input_item_Release(node);
        }
        free(url);
    }
    playlist_Unlock(sys->playlist);
}

/* Generate/cached a stable device id: use the config value if present,
 * otherwise derive one from the config dir path. */
static char *jf_device_id(vlc_object_t *obj)
{
    char *id = var_InheritString(obj, CFG_PREFIX"device-id");
    if (id != NULL && *id != '\0')
        return id;
    free(id);

    char *path = config_GetUserDir(VLC_CACHE_DIR);
    if (path == NULL)
        return strdup("vlc-jellyfin-default");

    /* cheap FNV-1a over the path */
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = path; *p; p++)
    {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    free(path);

    char *out;
    if (asprintf(&out, "vlc-jf-%016" PRIx64, h) < 0)
        return NULL;
    return out;
}

static char *jf_strdup_opt(const char *s)
{
    return (s != NULL && *s) ? strdup(s) : NULL;
}

/*****************************************************************************
 * Background thread
 *****************************************************************************/
/* All playback and connection fields (last_*, cfg_*) are owned exclusively
 * by this thread; the mutex only protects b_die (set from Close()). */
static void *Run(void *data)
{
    intf_thread_t *intf = data;
    intf_sys_t *sys = intf->p_sys;
    playlist_t *pl = sys->playlist;

    /* ---- phase 1: connect (login + library), retrying in background ---- */
    for (;;)
    {
        char err[256] = "unknown error";
        if (jf_client_login(&sys->client, sys->cfg_server, sys->cfg_username,
                            sys->cfg_password, sys->cfg_token, sys->cfg_userid,
                            sys->cfg_device ? sys->cfg_device : "vlc-jellyfin",
                            err, sizeof(err)) == 0)
            break;
        msg_Warn(intf, "jellyfin: login failed: %s (retrying in 30s)", err);
        if (jf_die_wait(&sys->lock, &sys->b_die, 30))
            return NULL;
    }
    msg_Info(intf, "jellyfin: logged in as user %s", sys->client.user_id);

    /* Cache token/user id so the password is only needed on the first run */
    if (sys->cfg_token == NULL || strcmp(sys->cfg_token, sys->client.token))
        config_PutPsz(VLC_OBJECT(intf), CFG_PREFIX"token", sys->client.token);
    if (sys->cfg_userid == NULL || strcmp(sys->cfg_userid, sys->client.user_id))
        config_PutPsz(VLC_OBJECT(intf), CFG_PREFIX"userid", sys->client.user_id);
    if (sys->cfg_device != NULL && *sys->cfg_device)
        config_PutPsz(VLC_OBJECT(intf), CFG_PREFIX"device-id", sys->cfg_device);

    free(sys->cfg_server);   sys->cfg_server   = NULL;
    free(sys->cfg_username); sys->cfg_username = NULL;
    free(sys->cfg_password); sys->cfg_password = NULL;
    free(sys->cfg_token);    sys->cfg_token    = NULL;
    free(sys->cfg_userid);   sys->cfg_userid   = NULL;
    free(sys->cfg_device);   sys->cfg_device   = NULL;

    char err[256] = "unknown error";
    if (jf_library_fetch(&sys->client, &sys->library, err, sizeof(err)) != 0)
        msg_Warn(intf, "jellyfin: could not fetch library (%s); "
                       "path-based matching disabled", err);
    else
        msg_Info(intf, "jellyfin: loaded %zu library items",
                 sys->library.count);

    sys->ready = true;

    if (sys->browse_on_start && sys->library.count > 0)
        jf_browse(sys);

    /* ---- phase 2: watch the playlist input and report playback --------- */
    for (;;)
    {
        input_thread_t *input = playlist_CurrentInput(pl);
        if (input == NULL)
        {
            jf_report_stopped_and_clear(sys);
        }
        else
        {
            input_item_t *item = input_GetItem(input);
            int state = var_GetInteger(input, "state");
            vlc_tick_t pos_us = var_GetInteger(input, "time");
            char *uri = item ? input_item_GetURI(item) : NULL;

            /* new playback = new input thread OR different URI
             * (covers replaying the same file) */
            bool changed = (uri != NULL)
                        && (input != sys->last_input || sys->last_uri == NULL
                            || strcmp(uri, sys->last_uri));

            if (changed)
            {
                /* media changed: close the old session, open a new one */
                char *old = sys->last_item_id;
                int64_t old_ticks = sys->last_ticks;
                sys->last_item_id = NULL;
                free(sys->last_uri);
                sys->last_uri = uri;
                uri = NULL; /* ownership moved */
                sys->last_input = input;
                sys->last_report = 0;
                sys->last_ticks = 0;

                if (old != NULL)
                    jf_report_stopped(&sys->client, old, old_ticks);
                free(old);

                char *id = jf_resolve_input(sys, item);
                if (id != NULL)
                {
                    int64_t ticks = pos_us > 0 ? pos_us * 10 : 0;
                    if (jf_report_playing(&sys->client, id, ticks) == 0)
                        msg_Info(intf, "jellyfin: reporting playback of %s", id);
                    else
                        msg_Warn(intf, "jellyfin: Playing report failed");
                    sys->last_item_id = id;
                    sys->last_report = mdate();
                    sys->last_ticks = ticks;
                }
                else
                {
                    msg_Dbg(intf, "jellyfin: media is not a Jellyfin item, "
                                  "skipping");
                }
            }
            else if (item != NULL)
            {
                sys->last_input = input;
                if (sys->last_item_id != NULL)
                {
                    bool due = (mdate() - sys->last_report)
                               >= VLC_TICK_FROM_SEC(sys->interval);
                    if (due)
                    {
                        sys->last_report = mdate();
                        bool paused = (state == PAUSE_S);
                        int64_t ticks = pos_us > 0 ? pos_us * 10 : 0;
                        if (jf_report_progress(&sys->client, sys->last_item_id,
                                               ticks, paused))
                            msg_Warn(intf, "jellyfin: Progress report failed");
                        else
                            sys->last_ticks = ticks;
                    }
                }
            }

            free(uri);
            vlc_object_release(input);
        }

        vlc_mutex_lock(&sys->lock);
        bool die = sys->b_die;
        vlc_mutex_unlock(&sys->lock);
        if (die)
            break;

        msleep(MS_FROM_VLC_TICK(500));
    }

    /* final Stopped report */
    jf_report_stopped_and_clear(sys);
    return NULL;
}

/*****************************************************************************
 * Open / Close
 *****************************************************************************/
static int Open(vlc_object_t *obj)
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;

    intf->p_sys = sys;
    sys->interval = var_InheritInteger(obj, CFG_PREFIX"interval");
    if (sys->interval < 5)
        sys->interval = 5;
    sys->basename_fallback = var_InheritBool(obj, CFG_PREFIX"basename-fallback");
    sys->browse_on_start = var_InheritBool(obj, CFG_PREFIX"browse");
    vlc_mutex_init(&sys->lock);

    /* Copy the connection settings for the background thread; all network
     * I/O (login, library) happens there so Open never blocks. */
    char *server   = var_InheritString(obj, CFG_PREFIX"server");
    if (server == NULL || *server == '\0')
    {
        msg_Err(intf, "jellyfin: no server URL configured "
                      "(--jellyfin-server)");
        free(server);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return VLC_EGENERIC;
    }
    sys->cfg_server   = server;
    sys->cfg_username = var_InheritString(obj, CFG_PREFIX"username");
    sys->cfg_password = var_InheritString(obj, CFG_PREFIX"password");
    char *tmp;
    tmp = var_InheritString(obj, CFG_PREFIX"token");
    sys->cfg_token = jf_strdup_opt(tmp); free(tmp);
    tmp = var_InheritString(obj, CFG_PREFIX"userid");
    sys->cfg_userid = jf_strdup_opt(tmp); free(tmp);
    sys->cfg_device   = jf_device_id(obj);

    if ((sys->cfg_username == NULL && (sys->cfg_token == NULL ||
                                       sys->cfg_userid == NULL)))
    {
        msg_Err(intf, "jellyfin: no credentials configured "
                      "(--jellyfin-username/--jellyfin-password)");
        free(sys->cfg_server); free(sys->cfg_username); free(sys->cfg_password);
        free(sys->cfg_token); free(sys->cfg_userid); free(sys->cfg_device);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return VLC_EGENERIC;
    }

    sys->playlist = pl_Get(intf);

    if (vlc_clone(&sys->thread, Run, intf, VLC_THREAD_PRIORITY_LOW))
    {
        free(sys->cfg_server); free(sys->cfg_username); free(sys->cfg_password);
        free(sys->cfg_token); free(sys->cfg_userid); free(sys->cfg_device);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return VLC_ENOMEM;
    }

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = intf->p_sys;

    vlc_mutex_lock(&sys->lock);
    sys->b_die = true;
    vlc_mutex_unlock(&sys->lock);

    vlc_join(sys->thread, NULL);

    free(sys->cfg_server); free(sys->cfg_username); free(sys->cfg_password);
    free(sys->cfg_token); free(sys->cfg_userid); free(sys->cfg_device);
    jf_item_list_clear(&sys->library);
    jf_client_close(&sys->client);
    free(sys->last_item_id);
    free(sys->last_uri);
    vlc_mutex_destroy(&sys->lock);
    free(sys);
}

/*****************************************************************************
 * Services discovery: Jellyfin library as a sidebar tree
 *****************************************************************************/

/* services_discovery_sys_t is opaque in vlc_services_discovery.h */
struct services_discovery_sys_t
{
    vlc_thread_t   thread;
    vlc_mutex_t    lock;
    bool           b_die;

    services_discovery_t *sd;
    jf_client_t    client;
    jf_item_list_t library;

    /* connection settings, owned until the thread is done with them */
    char *cfg_server, *cfg_username, *cfg_password;
    char *cfg_token, *cfg_userid, *cfg_device;
};

static void *RunSD(void *data)
{
    services_discovery_t *sd = data;
    struct services_discovery_sys_t *sys = sd->p_sys;

    /* login with retry */
    for (;;)
    {
        char err[256] = "unknown error";
        if (jf_client_login(&sys->client, sys->cfg_server, sys->cfg_username,
                            sys->cfg_password, sys->cfg_token, sys->cfg_userid,
                            sys->cfg_device ? sys->cfg_device : "vlc-jellyfin",
                            err, sizeof(err)) == 0)
            break;
        msg_Warn(sd, "jellyfin: login failed: %s (retrying in 30s)", err);
        if (jf_die_wait(&sys->lock, &sys->b_die, 30))
            return NULL;
    }
    msg_Info(sd, "jellyfin: logged in as user %s", sys->client.user_id);

    /* cache the token like the interface module does */
    if (sys->cfg_token == NULL || strcmp(sys->cfg_token, sys->client.token))
        config_PutPsz(VLC_OBJECT(sd), CFG_PREFIX"token", sys->client.token);
    if (sys->cfg_userid == NULL || strcmp(sys->cfg_userid, sys->client.user_id))
        config_PutPsz(VLC_OBJECT(sd), CFG_PREFIX"userid", sys->client.user_id);
    if (sys->cfg_device != NULL && *sys->cfg_device)
        config_PutPsz(VLC_OBJECT(sd), CFG_PREFIX"device-id", sys->cfg_device);

    char err[256] = "unknown error";
    if (jf_library_fetch(&sys->client, &sys->library, err, sizeof(err)) != 0)
    {
        msg_Warn(sd, "jellyfin: could not fetch library: %s", err);
        return NULL;
    }
    msg_Info(sd, "jellyfin: %zu library items available", sys->library.count);

    for (size_t i = 0; i < sys->library.count; i++)
    {
        const jf_item_t *it = &sys->library.items[i];
        char *url = jf_stream_url(&sys->client, it->id);
        if (url == NULL)
            continue;

        /* episodes: "SxxEyy - name" under a category per series;
         * everything else under 电影 */
        char name[512];
        char cat[512];
        if (!strcmp(it->type, "Episode"))
        {
            if (it->season > 0 && it->episode > 0)
                snprintf(name, sizeof(name), "S%02dE%02d - %s",
                         it->season, it->episode, it->name);
            else
                snprintf(name, sizeof(name), "%s", it->name);
            if (it->series != NULL)
                snprintf(cat, sizeof(cat), "%s", it->series);
            else
                snprintf(cat, sizeof(cat), "Episodes");
        }
        else
        {
            snprintf(name, sizeof(name), "%s", it->name);
            snprintf(cat, sizeof(cat), "Movies");
        }

        input_item_t *item = input_item_New(url, name);
        if (item != NULL)
        {
            services_discovery_AddItemCat(sd, item, cat);
            input_item_Release(item);
        }
        free(url);
    }
    return NULL;
}

static int OpenSD(vlc_object_t *obj)
{
    services_discovery_t *sd = (services_discovery_t *)obj;
    struct services_discovery_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;

    sd->p_sys = sys;
    sys->sd = sd;
    vlc_mutex_init(&sys->lock);

    char *server = var_InheritString(obj, CFG_PREFIX"server");
    if (server == NULL || *server == '\0')
    {
        msg_Err(sd, "jellyfin: no server URL configured "
                    "(--jellyfin-server)");
        free(server);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return VLC_EGENERIC;
    }
    sys->cfg_server   = server;
    sys->cfg_username = var_InheritString(obj, CFG_PREFIX"username");
    sys->cfg_password = var_InheritString(obj, CFG_PREFIX"password");
    char *tmp;
    tmp = var_InheritString(obj, CFG_PREFIX"token");
    sys->cfg_token = jf_strdup_opt(tmp); free(tmp);
    tmp = var_InheritString(obj, CFG_PREFIX"userid");
    sys->cfg_userid = jf_strdup_opt(tmp); free(tmp);
    sys->cfg_device = jf_device_id(obj);

    if (sys->cfg_username == NULL && (sys->cfg_token == NULL ||
                                      sys->cfg_userid == NULL))
    {
        msg_Err(sd, "jellyfin: no credentials configured");
        free(sys->cfg_server); free(sys->cfg_username); free(sys->cfg_password);
        free(sys->cfg_token); free(sys->cfg_userid); free(sys->cfg_device);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return VLC_EGENERIC;
    }

    if (vlc_clone(&sys->thread, RunSD, sd, VLC_THREAD_PRIORITY_LOW))
    {
        free(sys->cfg_server); free(sys->cfg_username); free(sys->cfg_password);
        free(sys->cfg_token); free(sys->cfg_userid); free(sys->cfg_device);
        vlc_mutex_destroy(&sys->lock);
        free(sys);
        return VLC_ENOMEM;
    }
    return VLC_SUCCESS;
}

static void CloseSD(vlc_object_t *obj)
{
    services_discovery_t *sd = (services_discovery_t *)obj;
    struct services_discovery_sys_t *sys = sd->p_sys;

    vlc_mutex_lock(&sys->lock);
    sys->b_die = true;
    vlc_mutex_unlock(&sys->lock);

    vlc_join(sys->thread, NULL);

    free(sys->cfg_server); free(sys->cfg_username); free(sys->cfg_password);
    free(sys->cfg_token); free(sys->cfg_userid); free(sys->cfg_device);
    jf_item_list_clear(&sys->library);
    jf_client_close(&sys->client);
    vlc_mutex_destroy(&sys->lock);
    free(sys);
}
