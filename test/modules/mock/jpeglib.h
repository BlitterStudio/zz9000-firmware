#ifndef JPEGLIB_H
#define JPEGLIB_H
typedef int J_COLOR_SPACE;
struct jpeg_decompress_struct { unsigned char opaque[256]; };
struct jpeg_error_mgr { unsigned char opaque[64]; };
struct jpeg_source_mgr { unsigned char opaque[64]; };
struct jpeg_progress_mgr { unsigned char opaque[64]; };
#endif
