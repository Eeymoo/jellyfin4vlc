/* Dummy libcurl implementations for the host unit tests. */
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

CURL *curl_easy_init(void) { return (CURL *)1; }
void curl_easy_cleanup(CURL *c) { (void)c; }
CURLcode curl_easy_setopt(CURL *c, CURLoption o, ...) { (void)c; (void)o; return CURLE_OK; }
CURLcode curl_easy_perform(CURL *c) { (void)c; return CURLE_OK; }
CURLcode curl_easy_getinfo(CURL *c, CURLoption o, ...) { (void)c; (void)o; return CURLE_OK; }
struct curl_slist *curl_slist_append(struct curl_slist *l, const char *s) { (void)s; return l; }
void curl_slist_free_all(struct curl_slist *l) { (void)l; }
char *curl_easy_escape(CURL *c, const char *s, int len)
{ (void)c; (void)len; return strdup(s); }
void curl_free(void *p) { free(p); }
