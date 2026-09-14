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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "jf_http.h"
#include "version.h"

#ifdef _WIN32
# include <wchar.h>
# include <windows.h>
#endif

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

/* Locate a CA certificate bundle for HTTPS peer verification.
 *
 * The Windows build statically links libcurl against OpenSSL (MSYS2
 * MINGW64), whose compiled-in default trust store points to MSYS2 paths
 * that do not exist on end-user machines.  Without a CA store every
 * https:// request fails with a certificate verification error, while
 * plain http:// keeps working — a confusing half-broken state.
 *
 * Resolution order:
 *   1. JELLYFIN4VLC_CAINFO environment variable (full override)
 *   2. cacert.pem sitting next to the plugin DLL (shipped in the zip)
 * Returns true and fills `out` if a candidate exists, false otherwise
 * (system trust store is left to its own defaults, e.g. Linux). */
static bool jf_find_cacert(char *out, size_t outlen)
{
    const char *env = getenv("JELLYFIN4VLC_CAINFO");
    if (env != NULL && *env != '\0')
    {
        FILE *f = fopen(env, "rb");
        if (f != NULL)
        {
            fclose(f);
            snprintf(out, outlen, "%s", env);
            return true;
        }
    }

#ifdef _WIN32
    /* Resolve our own DLL path so we can look for cacert.pem next to it.
     * GetModuleHandleEx with FROM_ADDRESS returns the HMODULE of the DLL
     * that contains this function. */
    HMODULE mod;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(void *)&jf_find_cacert, &mod))
    {
        wchar_t wpath[MAX_PATH];
        DWORD n = GetModuleFileNameW(mod, wpath, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            /* strip the file name, keep the directory */
            for (DWORD i = n; i-- > 0; )
                if (wpath[i] == L'\\' || wpath[i] == L'/')
                {
                    wpath[i] = L'\0';
                    break;
                }
            const wchar_t *file = L"\\cacert.pem";
            if (wcslen(wpath) + wcslen(file) < MAX_PATH)
            {
                wcscat(wpath, file);
                int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                              NULL, 0, NULL, NULL);
                if (len > 0 && (size_t)len <= outlen)
                {
                    char *tmp = malloc(len);
                    if (tmp != NULL)
                    {
                        WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                            tmp, len, NULL, NULL);
                        FILE *f = fopen(tmp, "rb");
                        if (f != NULL)
                        {
                            fclose(f);
                            snprintf(out, outlen, "%s", tmp);
                            free(tmp);
                            return true;
                        }
                        free(tmp);
                    }
                }
            }
        }
    }
#endif
    return false;
}

/* Insecure TLS mode (--jellyfin-insecure): skip certificate and hostname
 * verification.  Global because jf_http has no per-client context; set
 * once at module startup before any request. */
static bool s_insecure;

void jf_http_set_insecure(bool insecure)
{
    s_insecure = insecure;
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
    char errbuf[CURL_ERROR_SIZE] = "";

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "jellyfin4vlc/" JF_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, jf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* enable gzip */
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    /* Windows: point OpenSSL at a CA bundle we ship next to the DLL
     * (see jf_find_cacert).  Without this, https:// fails on machines
     * that lack the MSYS2 trust store the static OpenSSL was built for. */
    char cacert[1024];
    if (jf_find_cacert(cacert, sizeof(cacert)))
        curl_easy_setopt(curl, CURLOPT_CAINFO, cacert);

    if (s_insecure)
    {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

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
        snprintf(reply->err, sizeof(reply->err), "%s",
                 errbuf[0] != '\0' ? errbuf : curl_easy_strerror(rc));
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
