/*
 * Stream playlists (.pls and .m3u) fetched from a URL.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-streamlist.h"

static ZZPlayStreamList list;

static uint32_t parse(const char *text, const char *base)
{
  return zzplay_streamlist_parse(text, strlen(text), base, &list);
}

static int test_detect(void)
{
  if (!zzplay_streamlist_is_playlist("http://h/listen.pls")) return 1;
  if (!zzplay_streamlist_is_playlist("HTTP://h/a/B.M3U?sid=1")) return 2;
  if (!zzplay_streamlist_is_playlist("https://h/x.m3u8#frag")) return 3;
  if (zzplay_streamlist_is_playlist("http://h/stream.mp3")) return 4;
  if (zzplay_streamlist_is_playlist("http://h/pls")) return 5;
  if (zzplay_streamlist_is_playlist("Work:Music/list.m3u")) return 6;
  return 0;
}

static int test_pls(void)
{
  /* Order follows the File numbers, not the line order; CRLF, blanks and
   * a missing NumberOfEntries are fine. */
  if (parse("[playlist]\r\n"
            "File2=http://b.example:8000/live\r\n"
            "Title2=B\r\n"
            "File1= http://a.example/stream \r\n"
            "Title1=A\r\n"
            "Length1=-1\r\n"
            "\r\n"
            "Version=2\r\n",
            "http://dir.example/listen.pls") != 2U)
    return 1;
  if (strcmp(list.url[0], "http://a.example/stream") != 0) return 2;
  if (strcmp(list.url[1], "http://b.example:8000/live") != 0) return 3;
  /* Gaps close up; numbers beyond the limit and local paths are skipped. */
  if (parse("\xEF\xBB\xBF[Playlist]\nFile3=/rel\nFile99=http://x/y\n"
            "File1=Work:a.mp3\n",
            "http://dir.example/a/listen.pls") != 1U)
    return 4;
  if (strcmp(list.url[0], "http://dir.example/rel") != 0) return 5;
  if (parse("[playlist]\nNumberOfEntries=0\n", "http://h/x.pls") != 0U)
    return 6;
  return 0;
}

static int test_m3u(void)
{
  if (parse("#EXTM3U\n#EXTINF:-1,Station\nhttp://s.example/live.mp3\n"
            "\n  relative/backup.mp3  \r\n/abs\rWork:local.mp3\n",
            "https://h:8443/dir/list.m3u") != 3U)
    return 1;
  if (strcmp(list.url[0], "http://s.example/live.mp3") != 0) return 2;
  if (strcmp(list.url[1], "https://h:8443/dir/relative/backup.mp3") != 0)
    return 3;
  if (strcmp(list.url[2], "https://h:8443/abs") != 0) return 4;
  /* At most eight addresses. */
  {
    char text[1024];
    size_t n = 0U;
    int i;

    for (i = 0; i < 12; i++) {
      n += (size_t)snprintf(text + n, sizeof(text) - n, "http://h/%d\n", i);
    }
    if (parse(text, "http://h/x.m3u") != ZZPLAY_STREAMLIST_MAX) return 5;
    if (strcmp(list.url[7], "http://h/7") != 0) return 6;
  }
  /* Without a usable base only absolute addresses survive. */
  if (parse("rel.mp3\nhttp://ok/x\n", NULL) != 1U ||
      strcmp(list.url[0], "http://ok/x") != 0)
    return 7;
  return 0;
}

int main(void)
{
  int rc;

  rc = test_detect();
  if (rc != 0) { printf("detect %d\n", rc); return 10 + rc; }
  rc = test_pls();
  if (rc != 0) { printf("pls %d\n", rc); return 30 + rc; }
  rc = test_m3u();
  if (rc != 0) { printf("m3u %d\n", rc); return 50 + rc; }
  printf("zzplay_streamlist_test: all checks passed\n");
  return 0;
}
