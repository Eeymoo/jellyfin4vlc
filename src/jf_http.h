/*****************************************************************************
 * jf_http.h: minimal libcurl HTTP client for the Jellyfin VLC plugin
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

#ifndef JF_HTTP_H
#define JF_HTTP_H

#include <stddef.h>

typedef struct jf_http_reply
{
    long  code;       /* HTTP status code, 0 on transport error */
    char *body;       /* NUL-terminated response body (malloc'd), may be NULL */
    size_t size;      /* body length in bytes */
} jf_http_reply_t;

/* Both functions return 0 on transport success (check reply->code for HTTP
 * status), -1 on transport failure.  Caller must call jf_http_reply_clear()
 * on the reply. */
int jf_http_get (const char *url, const char *const *headers, size_t nheaders,
                 jf_http_reply_t *reply);
int jf_http_post(const char *url, const char *const *headers, size_t nheaders,
                 const char *body, size_t body_len, jf_http_reply_t *reply);

void jf_http_reply_clear(jf_http_reply_t *reply);

#endif
