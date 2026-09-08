/* Minimal libcurl stub header - ONLY for syntax-checking this project
 * on machines without libcurl development files. Never used in real builds. */
#ifndef STUB_CURL_CURL_H
#define STUB_CURL_CURL_H

#include <stddef.h>

typedef void CURL;
typedef enum { CURLE_OK = 0 } CURLcode;
typedef enum {
    CURLOPT_URL, CURLOPT_FOLLOWLOCATION, CURLOPT_NOSIGNAL, CURLOPT_TIMEOUT,
    CURLOPT_CONNECTTIMEOUT, CURLOPT_USERAGENT, CURLOPT_WRITEFUNCTION,
    CURLOPT_WRITEDATA, CURLOPT_ACCEPT_ENCODING, CURLOPT_HTTPHEADER,
    CURLOPT_POST, CURLOPT_POSTFIELDS, CURLOPT_POSTFIELDSIZE,
    CURLINFO_RESPONSE_CODE,
} CURLoption;

struct curl_slist { char *data; struct curl_slist *next; };

typedef size_t (*curl_write_callback)(char *ptr, size_t size, size_t nmemb,
                                      void *userdata);

CURL   *curl_easy_init(void);
void    curl_easy_cleanup(CURL *curl);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *curl);
CURLcode curl_easy_getinfo(CURL *curl, CURLoption info, ...);
struct curl_slist *curl_slist_append(struct curl_slist *l, const char *s);
void    curl_slist_free_all(struct curl_slist *l);
char   *curl_easy_escape(CURL *curl, const char *s, int len);
void    curl_free(void *p);

#endif
