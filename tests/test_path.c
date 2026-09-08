/* Host unit tests for the non-VLC logic (path matching, stream URL). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jf_api.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
                   failures++; } \
} while (0)

static jf_item_t mk(const char *id, const char *name, const char *path)
{
    jf_item_t it = { strdup(id), strdup(name), path ? strdup(path) : NULL,
                     strdup("Movie") };
    return it;
}

int main(void)
{
    jf_item_list_t l = {0};
    jf_item_t items[3] = {
        mk("aaa-111", "Movie One", "/media/movies/Movie One (2020)/movie.mkv"),
        mk("bbb-222", "Episode", "/media/tv/Show S01E01/s01e01.mkv"),
        mk("ccc-333", "No Path", NULL),
    };
    l.items = items;
    l.count = 3;

    char *id;

    id = jf_item_id_for_path(&l, "/media/movies/Movie One (2020)/movie.mkv");
    CHECK(id && !strcmp(id, "aaa-111")); free(id);

    /* case-insensitive + trailing slash */
    id = jf_item_id_for_path(&l, "/MEDIA/MOVIES/movie one (2020)/Movie.MKV/");
    CHECK(id && !strcmp(id, "aaa-111")); free(id);

    /* basename fallback for a different root layout */
    id = jf_item_id_for_path(&l, "/mnt/jf/remount/s01e01.mkv");
    CHECK(id && !strcmp(id, "bbb-222")); free(id);

    /* no match */
    id = jf_item_id_for_path(&l, "/tmp/unrelated.mp4");
    CHECK(id == NULL); free(id);

    /* NULL handling */
    CHECK(jf_item_id_for_path(&l, NULL) == NULL);
    CHECK(jf_item_id_for_path(NULL, "/x") == NULL);

    /* stream URL */
    jf_client_t c = { .server = strdup("http://jf:8096"),
                      .token = strdup("tok"), .user_id = strdup("u"),
                      .device_id = strdup("d") };
    char *url = jf_stream_url(&c, "abc-def");
    CHECK(url && !strcmp(url,
          "http://jf:8096/Videos/abc-def/stream?static=true&api_key=tok"));
    free(url);
    free(c.server); free(c.token); free(c.user_id); free(c.device_id);

    for (size_t i = 0; i < 3; i++)
    {
        free(items[i].id); free(items[i].name);
        free(items[i].path); free(items[i].type);
    }

    if (failures == 0)
        printf("all tests passed\n");
    return failures ? 1 : 0;
}
