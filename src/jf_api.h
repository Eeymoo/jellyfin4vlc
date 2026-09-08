/*****************************************************************************
 * jf_api.h: Jellyfin REST API client for the Jellyfin VLC plugin
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

#ifndef JF_API_H
#define JF_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JF_TICKS_PER_SEC INT64_C(10000000)

typedef struct jf_client
{
    char *server;    /* base URL, e.g. http://jellyfin:8096 (no trailing /) */
    char *token;     /* access token */
    char *user_id;   /* authenticated user id */
    char *device_id; /* stable device identifier */
} jf_client_t;

/* A lightweight media item record used for browsing and path matching. */
typedef struct jf_item
{
    char *id;    /* Jellyfin ItemId (GUID string) */
    char *name;  /* display name */
    char *path;  /* server-side file path (may be NULL) */
    char *type;  /* "Movie", "Episode", ... */
} jf_item_t;

typedef struct jf_item_list
{
    jf_item_t *items;
    size_t     count;
    size_t     capacity;
} jf_item_list_t;

/* --- client lifecycle --------------------------------------------------- */

/* Authenticates against the server.  If username/password are given, a new
 * token is obtained via /Users/AuthenticateByName.  If a cached token and
 * user id are given, they are used directly (faster, no password needed).
 * Returns 0 on success, -1 on error (errbuf gets a human readable reason). */
int jf_client_login(jf_client_t *c, const char *server, const char *username,
                    const char *password, const char *token,
                    const char *user_id, const char *device_id,
                    char *errbuf, size_t errlen);

void jf_client_close(jf_client_t *c);

/* --- library / browse ---------------------------------------------------- */

/* Fetches all Movie/Episode items (with their server file paths) using
 * paged /Users/{uid}/Items queries.  Caller frees with jf_item_list_clear(). */
int jf_library_fetch(jf_client_t *c, jf_item_list_t *out,
                     char *errbuf, size_t errlen);

void jf_item_list_clear(jf_item_list_t *l);

/* Finds the item whose server-side Path matches the local playing file
 * (case-insensitive, trailing slashes ignored, basename fallback).
 * Returns a malloc'd item id or NULL. */
char *jf_item_id_for_path(const jf_item_list_t *l, const char *local_path);

/* Builds the direct-stream URL for an item (owned by caller). */
char *jf_stream_url(const jf_client_t *c, const char *item_id);

/* --- playback sync ------------------------------------------------------- */

int jf_report_playing (jf_client_t *c, const char *item_id,
                       int64_t position_ticks);
int jf_report_progress(jf_client_t *c, const char *item_id,
                       int64_t position_ticks, bool paused);
int jf_report_stopped(jf_client_t *c, const char *item_id,
                      int64_t position_ticks);

#endif
