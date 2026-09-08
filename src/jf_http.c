/*****************************************************************************
 * jf_http.c: minimal libcurl HTTP client for the Jellyfin VLC plugin
 *****************************************************************************
 * Copyright (C) 2024 jellyfin4vlc authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "jf_http.h"

typedef struct jf_http_buf
{
    char  *data;
    size_t size;
} jf_http_buf_t;

static size_t jf_write_cb(char *ptr, size_t size, size_t nmemb, void *opaque)
{
    jf_http_buf_t *buf = opaque;
    size_t len = size * nmemb;

    char *n = realloc(buf->data, buf->size + len + 1);
    if (n == NULL)
        return 0; /* signal failure to curl */
    buf->data = n;
    memcpy(buf->data + buf->size, ptr, len);
    buf->size += len;
    buf->data[buf->size] = '\0';
    return len;
}

static int jf_http_perform(const char *url, const char *const *headers,
                           size_t nheaders, const char *post_body,
                           size_t post_len, jf_http_reply_t *reply)
{
    memset(reply, 0, sizeof(*reply));

    CURL *curl = curl_easy_init();
    if (curl == NULL)
        return -1;

    jf_http_buf_t buf = { .data = NULL, .size = 0 };
    struct curl_slist *hl = NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "jellyfin4vlc/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, jf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* enable gzip */

    for (size_t i = 0; i < nheaders; i++)
        hl = curl_slist_append(hl, headers[i]);
    if (hl != NULL)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);

    if (post_body != NULL)
    {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)post_len);
    }

    CURLcode rc = curl_easy_perform(curl);

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply->code);
    reply->body = buf.data;
    reply->size = buf.size;

    if (hl != NULL)
        curl_slist_free_all(hl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
    {
        jf_http_reply_clear(reply);
        return -1;
    }
    return 0;
}

int jf_http_get(const char *url, const char *const *headers, size_t nheaders,
                jf_http_reply_t *reply)
{
    return jf_http_perform(url, headers, nheaders, NULL, 0, reply);
}

int jf_http_post(const char *url, const char *const *headers, size_t nheaders,
                 const char *body, size_t body_len, jf_http_reply_t *reply)
{
    return jf_http_perform(url, headers, nheaders, body, body_len, reply);
}

void jf_http_reply_clear(jf_http_reply_t *reply)
{
    free(reply->body);
    reply->body = NULL;
    reply->size = 0;
    reply->code = 0;
}
