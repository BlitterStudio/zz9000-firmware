/*
 * Source guards for AmigaOS library metadata files.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/library_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ExpectedFunction {
  const char *name;
  const char *fd_line;
  const char *proto_decl;
  const char *lvo_token;
} ExpectedFunction;

static char *read_file(const char *path)
{
  FILE *file;
  long length;
  char *data;

  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  length = ftell(file);
  if (length < 0) {
    fclose(file);
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  data = (char *)malloc((size_t)length + 1U);
  if (!data) {
    fclose(file);
    return 0;
  }
  if (fread(data, 1U, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return 0;
  }

  data[length] = '\0';
  fclose(file);
  return data;
}

static int expect_contains(const char *label, const char *source,
                           const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }

  printf("%s: missing %s\n", label, needle);
  return 0;
}

static int expect_count(const char *label, unsigned long actual,
                        unsigned long expected)
{
  if (actual == expected) {
    return 1;
  }

  printf("%s: got %lu expected %lu\n", label, actual, expected);
  return 0;
}

int main(int argc, char **argv)
{
  static const ExpectedFunction expected[] = {
    {"ZZ9KQueryCaps", "ZZ9KQueryCaps(caps)(a0)",
     "int ZZ9KQueryCaps(ZZ9KCaps *caps);", "ZZ9K_INLINE_CALL1(-30"},
    {"ZZ9KQueryService", "ZZ9KQueryService(service_id,service)(d0/a0)",
     "int ZZ9KQueryService(uint32_t service_id, ZZ9KServiceInfo *service);",
     "jsr -36(a6)"},
    {"ZZ9KPing",
     "ZZ9KPing(payload,payload_len,reply_payload,reply_len)(a0/d0/a1/a2)",
     "int ZZ9KPing(const uint8_t *payload, uint32_t payload_len, "
     "uint8_t *reply_payload, uint32_t *reply_len);",
     "jsr -42(a6)"},
    {"ZZ9KCall", "ZZ9KCall(request,reply,timeout_ticks)(a0/a1/d0)",
     "int ZZ9KCall(ZZ9KRequest *request, ZZ9KMailboxEntry *reply, "
     "uint32_t timeout_ticks);",
     "jsr -48(a6)"},
    {"ZZ9KCallAsync",
     "ZZ9KCallAsync(async,request,callback,user_data)(a0/a1/a2/a3)",
     "int ZZ9KCallAsync(ZZ9KAsyncRequest *async, "
     "const ZZ9KRequest *request, ZZ9KAsyncCallback callback, "
     "void *user_data);",
     "jsr -54(a6)"},
    {"ZZ9KCallAsyncBatch",
     "ZZ9KCallAsyncBatch(asyncs,requests,count,callback,user_data,queued)"
     "(a0/a1/d0/a2/a3/a4)",
     "int ZZ9KCallAsyncBatch(ZZ9KAsyncRequest *asyncs, "
     "const ZZ9KRequest *requests, uint32_t count, "
     "ZZ9KAsyncCallback callback, void *user_data, uint32_t *queued);",
     "jsr -60(a6)"},
    {"ZZ9KPoll", "ZZ9KPoll(max_completions,completed)(d0/a0)",
     "int ZZ9KPoll(uint32_t max_completions, uint32_t *completed);",
     "jsr -66(a6)"},
    {"ZZ9KAllocShared", "ZZ9KAllocShared(length,alignment,flags,buffer)"
     "(d0/d1/d2/a0)",
     "int ZZ9KAllocShared(uint32_t length, uint32_t alignment, "
     "uint32_t flags, ZZ9KSharedBuffer *buffer);",
     "jsr -72(a6)"},
    {"ZZ9KFreeShared", "ZZ9KFreeShared(handle)(d0)",
     "int ZZ9KFreeShared(uint32_t handle);", "jsr -78(a6)"},
    {"ZZ9KMemFill", "ZZ9KMemFill(handle,offset,length,value)(d0/d1/d2/d3)",
     "int ZZ9KMemFill(uint32_t handle, uint32_t offset, uint32_t length, "
     "uint8_t value);",
     "jsr -84(a6)"},
    {"ZZ9KMemCopy",
     "ZZ9KMemCopy(dst_handle,dst_offset,src_handle,src_offset,length)"
     "(d0/d1/d2/d3/d4)",
     "int ZZ9KMemCopy(uint32_t dst_handle, uint32_t dst_offset, "
     "uint32_t src_handle, uint32_t src_offset, uint32_t length);",
     "jsr -90(a6)"},
    {"ZZ9KAllocSurface",
     "ZZ9KAllocSurface(width,height,format,flags,surface)(d0/d1/d2/d3/a0)",
     "int ZZ9KAllocSurface(uint32_t width, uint32_t height, "
     "uint32_t format, uint32_t flags, ZZ9KSurface *surface);",
     "jsr -96(a6)"},
    {"ZZ9KAllocSurfaceEx",
     "ZZ9KAllocSurfaceEx(width,height,format,flags,pitch,surface)"
     "(d0/d1/d2/d3/d4/a0)",
     "int ZZ9KAllocSurfaceEx(uint32_t width, uint32_t height, "
     "uint32_t format, uint32_t flags, uint32_t pitch, "
     "ZZ9KSurface *surface);",
     "jsr -102(a6)"},
    {"ZZ9KFreeSurface", "ZZ9KFreeSurface(handle)(d0)",
     "int ZZ9KFreeSurface(uint32_t handle);", "jsr -108(a6)"},
    {"ZZ9KMapFramebufferSurface", "ZZ9KMapFramebufferSurface(surface)(a0)",
     "int ZZ9KMapFramebufferSurface(ZZ9KSurface *surface);",
     "ZZ9K_INLINE_CALL1(-114"},
    {"ZZ9KScaleImage", "ZZ9KScaleImage(desc)(a0)",
     "int ZZ9KScaleImage(const ZZ9KScaleImageDesc *desc);",
     "ZZ9K_INLINE_CALL1(-120"},
    {"ZZ9KReadDiag", "ZZ9KReadDiag(diag)(a0)",
     "int ZZ9KReadDiag(ZZ9KDiagInfo *diag);", "ZZ9K_INLINE_CALL1(-126"},
    {"ZZ9KCallAsyncMsg", "ZZ9KCallAsyncMsg(async,request,reply_port)"
     "(a0/a1/a2)",
     "int ZZ9KCallAsyncMsg(ZZ9KAsyncRequest *async, "
     "const ZZ9KRequest *request, struct MsgPort *reply_port);",
     "jsr -132(a6)"},
    {"ZZ9KCallAsyncBatchMsg",
     "ZZ9KCallAsyncBatchMsg(asyncs,requests,count,reply_port,queued)"
     "(a0/a1/d0/a2/a3)",
     "int ZZ9KCallAsyncBatchMsg(ZZ9KAsyncRequest *asyncs, "
     "const ZZ9KRequest *requests, uint32_t count, "
     "struct MsgPort *reply_port, uint32_t *queued);",
     "jsr -138(a6)"},
    {"ZZ9KCancelAsync", "ZZ9KCancelAsync(async)(a0)",
     "int ZZ9KCancelAsync(ZZ9KAsyncRequest *async);", "jsr -144(a6)"},
    {"ZZ9KWaitAsync", "ZZ9KWaitAsync(async,timeout_polls,polls_run)"
     "(a0/d0/a1)",
     "int ZZ9KWaitAsync(ZZ9KAsyncRequest *async, "
     "uint32_t timeout_polls, uint32_t *polls_run);",
     "jsr -150(a6)"},
    {"ZZ9KWaitAsyncBatch",
     "ZZ9KWaitAsyncBatch(asyncs,count,timeout_polls,completed,polls_run)"
     "(a0/d0/d1/a1/a2)",
     "int ZZ9KWaitAsyncBatch(ZZ9KAsyncRequest *asyncs, uint32_t count, "
     "uint32_t timeout_polls, uint32_t *completed, uint32_t *polls_run);",
     "jsr -156(a6)"},
    {"ZZ9KDecodeImage", "ZZ9KDecodeImage(opcode,desc,result)(d0/a0/a1)",
     "int ZZ9KDecodeImage(uint32_t opcode, "
     "const ZZ9KImageDecodeDesc *desc, ZZ9KImageDecodeResult *result);",
     "jsr -162(a6)"},
    {"ZZ9KCryptoHash", "ZZ9KCryptoHash(desc,result)(a0/a1)",
     "int ZZ9KCryptoHash(const ZZ9KCryptoHashDesc *desc, "
     "ZZ9KCryptoResult *result);",
     "jsr -168(a6)"},
    {"ZZ9KCryptoHashBatch",
     "ZZ9KCryptoHashBatch(descs,results,count,max_in_flight,timeout_ticks)"
     "(a0/a1/d0/d1/d2)",
     "int ZZ9KCryptoHashBatch(const ZZ9KCryptoHashDesc *descs, "
     "ZZ9KCryptoResult *results, uint32_t count, uint32_t max_in_flight, "
     "uint32_t timeout_ticks);",
     "jsr -174(a6)"},
    {"ZZ9KCryptoStream", "ZZ9KCryptoStream(desc,result)(a0/a1)",
     "int ZZ9KCryptoStream(const ZZ9KCryptoStreamDesc *desc, "
     "ZZ9KCryptoResult *result);",
     "jsr -180(a6)"},
    {"ZZ9KCryptoStreamBatch",
     "ZZ9KCryptoStreamBatch(descs,results,count,max_in_flight,timeout_ticks)"
     "(a0/a1/d0/d1/d2)",
     "int ZZ9KCryptoStreamBatch(const ZZ9KCryptoStreamDesc *descs, "
     "ZZ9KCryptoResult *results, uint32_t count, uint32_t max_in_flight, "
     "uint32_t timeout_ticks);",
     "jsr -186(a6)"},
    {"ZZ9KCryptoAead", "ZZ9KCryptoAead(desc,result)(a0/a1)",
     "int ZZ9KCryptoAead(const ZZ9KCryptoAeadDesc *desc, "
     "ZZ9KCryptoResult *result);",
     "jsr -192(a6)"},
    {"ZZ9KCryptoAeadBatch",
     "ZZ9KCryptoAeadBatch(descs,results,count,max_in_flight,timeout_ticks)"
     "(a0/a1/d0/d1/d2)",
     "int ZZ9KCryptoAeadBatch(const ZZ9KCryptoAeadDesc *descs, "
     "ZZ9KCryptoResult *results, uint32_t count, uint32_t max_in_flight, "
     "uint32_t timeout_ticks);",
     "jsr -198(a6)"},
    {"ZZ9KFillSurface", "ZZ9KFillSurface(desc)(a0)",
     "int ZZ9KFillSurface(const ZZ9KSurfaceFillDesc *desc);",
     "jsr -204(a6)"},
    {"ZZ9KCopySurface", "ZZ9KCopySurface(desc)(a0)",
     "int ZZ9KCopySurface(const ZZ9KSurfaceCopyDesc *desc);",
     "jsr -210(a6)"},
    {"ZZ9KImageSessionBegin",
     "ZZ9KImageSessionBegin(desc,result)(a0/a1)",
     "int ZZ9KImageSessionBegin(const ZZ9KImageSessionBeginDesc *desc, "
     "ZZ9KImageSessionResult *result);",
     "jsr -216(a6)"},
    {"ZZ9KImageSessionFeed",
     "ZZ9KImageSessionFeed(desc,result)(a0/a1)",
     "int ZZ9KImageSessionFeed(const ZZ9KImageSessionFeedDesc *desc, "
     "ZZ9KImageSessionResult *result);",
     "jsr -222(a6)"},
    {"ZZ9KImageSessionClose",
     "ZZ9KImageSessionClose(session,flags)(d0/d1)",
     "int ZZ9KImageSessionClose(uint32_t session, uint32_t flags);",
     "jsr -228(a6)"},
    {"ZZ9KScaleImageClipped", "ZZ9KScaleImageClipped(desc)(a0)",
     "int ZZ9KScaleImageClipped(const ZZ9KScaleImageClippedDesc *desc);",
     "jsr -234(a6)"},
    {"ZZ9KDecodeJpeg", "ZZ9KDecodeJpeg(desc,result)(a0/a1)",
     "int ZZ9KDecodeJpeg(const ZZ9KImageDecodeDesc *desc, "
     "ZZ9KImageDecodeResult *result);",
     "jsr -240(a6)"},
    {"ZZ9KDecodePng", "ZZ9KDecodePng(desc,result)(a0/a1)",
     "int ZZ9KDecodePng(const ZZ9KImageDecodeDesc *desc, "
     "ZZ9KImageDecodeResult *result);",
     "jsr -246(a6)"},
    {"ZZ9KDecodeGif", "ZZ9KDecodeGif(desc,result)(a0/a1)",
     "int ZZ9KDecodeGif(const ZZ9KImageDecodeDesc *desc, "
     "ZZ9KImageDecodeResult *result);",
     "jsr -252(a6)"},
    {"ZZ9KDecodeMp3", "ZZ9KDecodeMp3(desc,result)(a0/a1)",
     "int ZZ9KDecodeMp3(const ZZ9KAudioDecodeDesc *desc, "
     "ZZ9KAudioDecodeResult *result);",
     "jsr -258(a6)"},
    {"ZZ9KAudioStreamBegin",
     "ZZ9KAudioStreamBegin(desc,result)(a0/a1)",
     "int ZZ9KAudioStreamBegin(const ZZ9KAudioStreamBeginDesc *desc, "
     "ZZ9KAudioStreamResult *result);",
     "jsr -264(a6)"},
    {"ZZ9KAudioStreamFeed",
     "ZZ9KAudioStreamFeed(desc,result)(a0/a1)",
     "int ZZ9KAudioStreamFeed(const ZZ9KAudioStreamFeedDesc *desc, "
     "ZZ9KAudioStreamResult *result);",
     "jsr -270(a6)"},
    {"ZZ9KAudioStreamRead",
     "ZZ9KAudioStreamRead(session,pcm_read,flags,result)(d0/d1/d2/a0)",
     "int ZZ9KAudioStreamRead(uint32_t session, uint32_t pcm_read, "
     "uint32_t flags, ZZ9KAudioStreamResult *result);",
     "jsr -276(a6)"},
    {"ZZ9KAudioStreamClose",
     "ZZ9KAudioStreamClose(session,flags,result)(d0/d1/a0)",
     "int ZZ9KAudioStreamClose(uint32_t session, uint32_t flags, "
     "ZZ9KAudioStreamResult *result);",
     "jsr -282(a6)"},
    {"ZZ9KCryptoKeyExchange", "ZZ9KCryptoKeyExchange(desc,result)(a0/a1)",
      "int ZZ9KCryptoKeyExchange(const ZZ9KCryptoKxDesc *desc, "
      "ZZ9KCryptoResult *result);",
      "jsr -288(a6)"},
    {"ZZ9KCryptoVerify", "ZZ9KCryptoVerify(desc,valid)(a0/a1)",
      "int ZZ9KCryptoVerify(const ZZ9KCryptoVerifyDesc *desc, int *valid);",
      "jsr -294(a6)"},
    {"ZZ9KAudioStreamPlay",
     "ZZ9KAudioStreamPlay(session,flags,result)(d0/d1/a0)",
     "int ZZ9KAudioStreamPlay(uint32_t session, uint32_t flags, "
     "ZZ9KAudioStreamResult *result);",
     "jsr -300(a6)"},
    {"ZZ9KAudioStreamStop",
     "ZZ9KAudioStreamStop(session,flags,result)(d0/d1/a0)",
     "int ZZ9KAudioStreamStop(uint32_t session, uint32_t flags, "
     "ZZ9KAudioStreamResult *result);",
     "jsr -306(a6)"},
    {"ZZ9KQueryPalette", "ZZ9KQueryPalette(desc)(a0)",
      "int ZZ9KQueryPalette(const ZZ9KPaletteQueryDesc *desc);",
      "jsr -312(a6)"}
  };
  char *fd;
  char *clib;
  char *proto;
  size_t i;
  int ok;

  if (argc != 4) {
    printf("usage: %s <zz9k_lib.fd> <clib/zz9k_protos.h> "
           "<proto/zz9k.h>\n", argv[0]);
    return 2;
  }

  fd = read_file(argv[1]);
  clib = read_file(argv[2]);
  proto = read_file(argv[3]);
  if (!fd || !clib || !proto) {
    printf("failed to read metadata inputs\n");
    free(fd);
    free(clib);
    free(proto);
    return 2;
  }

  ok = 1;
  ok &= expect_contains("fd", fd, "##base ZZ9KBase");
  ok &= expect_contains("fd", fd, "##bias 30");
  ok &= expect_contains("fd", fd, "##public");
  ok &= expect_contains("fd", fd, "##end");
  ok &= expect_contains("clib", clib, "#ifndef CLIB_ZZ9K_PROTOS_H");
  ok &= expect_contains("clib", clib, "#include \"zz9k/library.h\"");
  ok &= expect_contains("proto", proto, "#include \"clib/zz9k_protos.h\"");
  ok &= expect_count("function_count",
                     sizeof(expected) / sizeof(expected[0]),
                     ZZ9K_LVO_FUNCTION_COUNT);

  for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
    ok &= expect_contains("fd", fd, expected[i].fd_line);
    ok &= expect_contains("clib", clib, expected[i].proto_decl);
    ok &= expect_contains("proto", proto, expected[i].lvo_token);
  }

  free(fd);
  free(clib);
  free(proto);
  return ok ? 0 : 1;
}
