#ifndef FF_H
#define FF_H
#include <stdint.h>
typedef unsigned int UINT;
typedef struct { uint32_t size; } FIL;
typedef int FRESULT;
#define FR_OK 0
#define FR_NOT_READY 3
#define FR_NO_FILE 4
#define FR_NO_PATH 5
#define FR_NO_FILESYSTEM 13
#define f_size(fp) ((fp)->size)
FRESULT f_open(FIL *, const char *, unsigned char);
FRESULT f_read(FIL *, void *, UINT, UINT *);
FRESULT f_close(FIL *);
#endif
