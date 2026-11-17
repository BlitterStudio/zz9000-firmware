#ifndef ZLIB_H
#define ZLIB_H
typedef unsigned char Bytef;
typedef unsigned long uLong;
typedef unsigned long uLongf;
#define Z_OK 0
static inline uLong compressBound(uLong source_len) { return source_len; }
static inline int compress2(Bytef *dst, uLongf *dst_len,
                            const Bytef *src, uLong src_len, int level)
{
    (void)dst;
    (void)dst_len;
    (void)src;
    (void)src_len;
    (void)level;
    return -1;
}
#endif
