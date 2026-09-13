/*****************************************************************************
 * jf_api.c: Jellyfin REST API client for the Jellyfin VLC plugin
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cJSON.h>

#include "jf_api.h"
#include "version.h"
#include "jf_http.h"

#define JF_PAGE_SIZE 500
#define JF_ERR(...) snprintf(errbuf, errlen, __VA_ARGS__)

/* --- small helpers ------------------------------------------------------- */

static char *jf_strdup_nonnull(const char *s)
{
    return strdup((s != NULL && *s) ? s : "");
}

/* Like strdup() but returns NULL for NULL/empty input. */
static char *jf_strdup_opt(const char *s)
{
    return (s != NULL && *s) ? strdup(s) : NULL;
}

static char *jf_trim_slash(char *s)
{
    if (s == NULL)
        return NULL;
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == '/')
        s[--len] = '\0';
    return s;
}

static char *jf_authorization_header(const jf_client_t *c, bool with_token)
{
    size_t cap = 256 + strlen(c->device_id) + (with_token ? strlen(c->token) : 0);
    char *h = malloc(cap);
    if (h == NULL)
        return NULL;
    if (with_token)
        snprintf(h, cap,
                 "X-Emby-Authorization: MediaBrowser Client=\"jellyfin4vlc\", "
                 "Device=\"VLC\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"",
                 c->device_id, JF_VERSION, c->token);
    else
        snprintf(h, cap,
                 "X-Emby-Authorization: MediaBrowser Client=\"jellyfin4vlc\", "
                 "Device=\"VLC\", DeviceId=\"%s\", Version=\"%s\"",
                 c->device_id, JF_VERSION);
    return h;
}

/* Performs a GET returning a parsed JSON object, or NULL on error.
 * `extra_headers` may be NULL. */
static cJSON *jf_get_json(jf_client_t *c, const char *url_path_fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Performs an HTTP request. Returns a parsed JSON object, or NULL.
 * Transport / HTTP / JSON failures set *ok = false. An empty body with a
 * 2xx status (Jellyfin's playback endpoints reply 204 No Content) sets
 * *ok = true and returns NULL. */
static cJSON *jf_request_json(jf_client_t *c, const char *url, bool post,
                              const char *body, bool *ok,
                              char *errbuf, size_t errlen)
{
    *ok = false;
    const char *ctype = "Content-Type: application/json";
    char *auth_owned = (c->token != NULL && *c->token)
                     ? jf_authorization_header(c, true)
                     : NULL;

    const char *headers[2];
    size_t nheaders = 0;
    if (post)
        headers[nheaders++] = ctype;
    if (auth_owned != NULL)
        headers[nheaders++] = auth_owned;

    jf_http_reply_t reply;
    int rc = post ? jf_http_post(url, headers, nheaders,
                                 body ? body : "", body ? strlen(body) : 0,
                                 &reply)
                  : jf_http_get(url, headers, nheaders, &reply);
    free(auth_owned);

    if (rc != 0)
    {
        JF_ERR("network error contacting %s", url);
        return NULL;
    }
    if (reply.code < 200 || reply.code >= 300)
    {
        JF_ERR("HTTP %ld from %s", reply.code, url);
        jf_http_reply_clear(&reply);
        return NULL;
    }

    /* 2xx: an empty (or whitespace-only) body is a valid success for
     * endpoints like /Sessions/Playing that return 204 No Content. */
    const char *p = reply.body;
    while (p != NULL && *p != '\0' && isspace((unsigned char)*p))
        p++;
    if (p == NULL || *p == '\0')
    {
        jf_http_reply_clear(&reply);
        *ok = true;
        return NULL;
    }

    cJSON *json = cJSON_Parse(reply.body);
    jf_http_reply_clear(&reply);
    if (json == NULL)
        JF_ERR("invalid JSON response from %s", url);
    else
        *ok = true;
    return json;
}

static cJSON *jf_get_json(jf_client_t *c, const char *url_path_fmt, ...)
{
    /* Full URL build + GET. Errors are silent here (callers of the single
     * page fetch tolerate empty pages). */
    va_list ap;
    va_start(ap, url_path_fmt);
    char path[2048];
    vsnprintf(path, sizeof(path), url_path_fmt, ap);
    va_end(ap);

    char url[4096];
    snprintf(url, sizeof(url), "%s%s", c->server, path);

    char err[256];
    bool ok = false;
    cJSON *r = jf_request_json(c, url, false, NULL, &ok, err, sizeof(err));
    return (ok && r == NULL) ? cJSON_CreateObject() : r; /* empty page */
}

/* --- client lifecycle ----------------------------------------------------- */

int jf_client_login(jf_client_t *c, const char *server, const char *username,
                    const char *password, const char *token,
                    const char *user_id, const char *device_id,
                    char *errbuf, size_t errlen)
{
    memset(c, 0, sizeof(*c));

    if (server == NULL || *server == '\0')
    {
        JF_ERR("no server URL configured");
        return -1;
    }

    c->server    = jf_trim_slash(strdup(server));
    c->token     = jf_strdup_opt(token);
    c->user_id   = jf_strdup_opt(user_id);
    c->device_id = jf_strdup_nonnull(device_id);
    if (c->server == NULL || c->device_id == NULL)
    {
        JF_ERR("out of memory");
        jf_client_close(c);
        return -1;
    }

    if (c->token != NULL && c->user_id != NULL)
        return 0; /* cached credentials, assume valid */

    if (username == NULL || *username == '\0')
    {
        JF_ERR("no username/password and no cached token + user id");
        jf_client_close(c);
        return -1;
    }

    /* Authenticate by name */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "Username", username);
    if (password != NULL && *password)
        cJSON_AddStringToObject(body, "Pw", password);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char url[4096];
    snprintf(url, sizeof(url), "%s/Users/AuthenticateByName", c->server);

    /* POST without token */
    char *saved_token = c->token;
    c->token = NULL;
    bool ok = false;
    cJSON *resp = jf_request_json(c, url, true, body_str, &ok, errbuf, errlen);
    free(body_str);
    c->token = saved_token; /* restore (NULL unless cached; we get new below) */
    if (resp == NULL || !ok)
    {
        if (ok && resp == NULL)
            JF_ERR("empty authentication response from %s", url);
        cJSON_Delete(resp);
        jf_client_close(c);
        return -1;
    }

    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(resp, "AccessToken"));
    cJSON *user = cJSON_GetObjectItem(resp, "User");
    const char *uid = user ? cJSON_GetStringValue(cJSON_GetObjectItem(user, "Id")) : NULL;

    if (t == NULL || uid == NULL)
    {
        JF_ERR("authentication response missing AccessToken/User Id");
        cJSON_Delete(resp);
        jf_client_close(c);
        return -1;
    }

    free(c->token);
    free(c->user_id);
    c->token   = strdup(t);
    c->user_id = strdup(uid);
    cJSON_Delete(resp);

    if (c->token == NULL || c->user_id == NULL)
    {
        JF_ERR("out of memory");
        jf_client_close(c);
        return -1;
    }
    return 0;
}

void jf_client_close(jf_client_t *c)
{
    if (c == NULL)
        return;
    free(c->server);
    free(c->token);
    free(c->user_id);
    free(c->device_id);
    memset(c, 0, sizeof(*c));
}

/* --- library / browse ------------------------------------------------------ */

static void jf_item_fill(jf_item_t *it, const cJSON *ji)
{
    memset(it, 0, sizeof(*it));
    const char *id     = cJSON_GetStringValue(cJSON_GetObjectItem(ji, "Id"));
    const char *name   = cJSON_GetStringValue(cJSON_GetObjectItem(ji, "Name"));
    const char *path   = cJSON_GetStringValue(cJSON_GetObjectItem(ji, "Path"));
    const char *type   = cJSON_GetStringValue(cJSON_GetObjectItem(ji, "Type"));
    const char *series = cJSON_GetStringValue(cJSON_GetObjectItem(ji, "SeriesName"));
    it->id      = strdup(id ? id : "");
    it->name    = strdup(name ? name : "");
    it->path    = path ? strdup(path) : NULL;
    it->type    = strdup(type ? type : "");
    it->series  = series ? strdup(series) : NULL;
    it->season  = (int)cJSON_GetNumberValue(
                      cJSON_GetObjectItem(ji, "ParentIndexNumber"));
    it->episode = (int)cJSON_GetNumberValue(
                      cJSON_GetObjectItem(ji, "IndexNumber"));
}

static bool jf_item_ok(const jf_item_t *it)
{
    return it->id != NULL && it->name != NULL && it->type != NULL;
}

void jf_item_list_clear(jf_item_list_t *l)
{
    if (l == NULL || l->items == NULL)
        return;
    for (size_t i = 0; i < l->count; i++)
    {
        free(l->items[i].id);
        free(l->items[i].name);
        free(l->items[i].path);
        free(l->items[i].type);
        free(l->items[i].series);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

static int jf_items_reserve(jf_item_list_t *l, size_t need)
{
    if (l->items != NULL && l->count + need <= l->capacity)
        return 0;
    size_t ncap = l->capacity ? l->capacity : 64;
    while (ncap < l->count + need)
        ncap *= 2;
    jf_item_t *n = realloc(l->items, ncap * sizeof(*n));
    if (n == NULL)
        return -1;
    l->items = n;
    l->capacity = ncap;
    return 0;
}

int jf_library_fetch(jf_client_t *c, jf_item_list_t *out,
                     char *errbuf, size_t errlen)
{
    memset(out, 0, sizeof(*out));

    char *escaped_uid = curl_easy_escape(NULL, c->user_id, 0);
    if (escaped_uid == NULL)
    {
        JF_ERR("out of memory");
        return -1;
    }

    size_t start = 0;
    for (;;)
    {
        cJSON *page = jf_get_json(c,
            "/Users/%s/Items?Recursive=true"
            "&IncludeItemTypes=Movie,Episode"
            "&Fields=Path,SeriesName,ParentIndexNumber,IndexNumber&IsMissing=false"
            "&EnableImages=false&EnableUserData=false"
            "&Limit=%d&StartIndex=%zu",
            escaped_uid, JF_PAGE_SIZE, start);
        if (page == NULL)
        {
            curl_free(escaped_uid);
            jf_item_list_clear(out);
            JF_ERR("failed to fetch library page at offset %zu", start);
            return -1;
        }

        const cJSON *items = cJSON_GetObjectItem(page, "Items");
        int n = cJSON_GetArraySize(items);
        size_t total = (size_t)cJSON_GetNumberValue(
            cJSON_GetObjectItem(page, "TotalRecordCount"));

        if (n <= 0)
        {
            cJSON_Delete(page);
            break;
        }

        if (jf_items_reserve(out, (size_t)n) != 0)
        {
            cJSON_Delete(page);
            curl_free(escaped_uid);
            jf_item_list_clear(out);
            JF_ERR("out of memory");
            return -1;
        }

        const cJSON *ji;
        cJSON_ArrayForEach(ji, items)
        {
            jf_item_t it;
            jf_item_fill(&it, ji);
            if (!jf_item_ok(&it))
            {
                free(it.id); free(it.name); free(it.path); free(it.type); free(it.series);
                continue;
            }
            out->items[out->count++] = it;
        }
        cJSON_Delete(page);

        start += (size_t)n;
        if (start >= total || (size_t)n < JF_PAGE_SIZE)
            break;
    }

    curl_free(escaped_uid);
    return 0;
}

/* --- path matching --------------------------------------------------------- */

static char *jf_norm_path(const char *path)
{
    if (path == NULL)
        return NULL;
    char *n = strdup(path);
    if (n == NULL)
        return NULL;
    size_t len = strlen(n);
    while (len > 0 && n[len - 1] == '/')
        n[--len] = '\0';
    for (char *p = n; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return n;
}

static const char *jf_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char *jf_item_id_for_path(const jf_item_list_t *l, const char *local_path,
                          bool allow_basename)
{
    if (l == NULL || local_path == NULL)
        return NULL;

    /* strip file:// scheme if present */
    if (!strncmp(local_path, "file://", 7))
        local_path += 7;

    char *want = jf_norm_path(local_path);
    if (want == NULL)
        return NULL;

    /* Pass 1: full path equality */
    for (size_t i = 0; i < l->count; i++)
    {
        if (l->items[i].path == NULL)
            continue;
        char *have = jf_norm_path(l->items[i].path);
        if (have != NULL && !strcmp(have, want))
        {
            free(have);
            char *id = strdup(l->items[i].id);
            free(want);
            return id;
        }
        free(have);
    }

    /* Pass 2 (optional): basename equality only. Useful when the client
     * cannot see the server's mount layout, but can mismatch
     * identically-named files in different folders. */
    if (allow_basename)
    {
        const char *want_base = jf_basename(want);
        for (size_t i = 0; i < l->count; i++)
        {
            if (l->items[i].path == NULL)
                continue;
            char *have = jf_norm_path(l->items[i].path);
            if (have != NULL && !strcmp(jf_basename(have), want_base))
            {
                free(have);
                char *id = strdup(l->items[i].id);
                free(want);
                return id;
            }
            free(have);
        }
    }

    free(want);
    return NULL;
}

char *jf_stream_url(const jf_client_t *c, const char *item_id)
{
    char *esc_id = curl_easy_escape(NULL, item_id, 0);
    if (esc_id == NULL)
        return NULL;
    char *url;
    if (asprintf(&url, "%s/Videos/%s/stream?static=true&api_key=%s",
                 c->server, esc_id, c->token) < 0)
        url = NULL;
    curl_free(esc_id);
    return url;
}

/* --- playback sync ---------------------------------------------------------- */

static int jf_report(jf_client_t *c, const char *endpoint_suffix,
                     const char *item_id, int64_t position_ticks,
                     bool has_paused, bool paused)
{
    if (c == NULL || item_id == NULL || c->token == NULL)
        return -1;

    char url[4096];
    snprintf(url, sizeof(url), "%s%s", c->server, endpoint_suffix);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "ItemId", item_id);
    cJSON_AddNumberToObject(body, "PositionTicks", (double)position_ticks);
    cJSON_AddStringToObject(body, "PlayMethod", "DirectPlay");
    if (has_paused)
        cJSON_AddBoolToObject(body, "IsPaused", paused);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char err[256];
    bool ok = false;
    cJSON *resp = jf_request_json(c, url, true, body_str, &ok, err, sizeof(err));
    free(body_str);
    if (resp != NULL)
        cJSON_Delete(resp);
    /* 204 No Content (the normal success reply) yields ok=true, resp=NULL */
    return ok ? 0 : -1;
}

int jf_report_playing(jf_client_t *c, const char *item_id, int64_t ticks)
{
    return jf_report(c, "/Sessions/Playing", item_id, ticks, false, false);
}

int jf_report_progress(jf_client_t *c, const char *item_id, int64_t ticks,
                       bool paused)
{
    return jf_report(c, "/Sessions/Playing/Progress", item_id, ticks,
                     true, paused);
}

int jf_report_stopped(jf_client_t *c, const char *item_id, int64_t ticks)
{
    return jf_report(c, "/Sessions/Playing/Stopped", item_id, ticks,
                     false, false);
}
