/* GCC inline callers for zz9k.library ARM app-client vectors. */
#ifndef INLINE_ZZ9K_H
#define INLINE_ZZ9K_H

#include <clib/zz9k_protos.h>

extern struct Library *ZZ9KBase;
#include <exec/libraries.h>

#if defined(__GNUC__) && (defined(__amigaos__) || defined(__amiga__) || defined(__AMIGA__))
#define ZZ9K_APP_INLINE_CLOBBERS_A0_A1 "cc", "memory", "d1"
#define ZZ9K_APP_INLINE_CLOBBERS_A0 "cc", "memory", "d1", "a1"
#define ZZ9K_APP_INLINE_CLOBBERS_A0_D0 "cc", "memory", "d0", "d1", "a1"

#define ZZ9K_APP_INLINE_2A(name, offset, type0, arg0, type1, arg1) \
static __inline int __##name(type0 arg0, type1 arg1) { \
  register int d0 __asm("d0"); \
  register struct Library *a6 __asm("a6") = ZZ9KBase; \
  register type0 a0 __asm("a0") = arg0; \
  register type1 a1 __asm("a1") = arg1; \
  __asm volatile("jsr " #offset "(a6)" : "=r"(d0), "+r"(a0), "+r"(a1) : "r"(a6) : ZZ9K_APP_INLINE_CLOBBERS_A0_A1); \
  return d0; }

ZZ9K_APP_INLINE_2A(ZZ9KAppOpenInline, -354, ZZ9KAppHandle **, handle, struct MsgPort *, notify_port)
#define ZZ9KAppOpen(handle, notify_port) __ZZ9KAppOpenInline((handle), (notify_port))
ZZ9K_APP_INLINE_2A(ZZ9KAppLoadFileInline, -360, ZZ9KAppHandle *, handle, const char *, path)
#define ZZ9KAppLoadFile(handle, path) __ZZ9KAppLoadFileInline((handle), (path))

static __inline int __ZZ9KAppLoadMemoryInline(ZZ9KAppHandle *handle, const void *data, uint32_t length) {
  register uint32_t d0 __asm("d0") = length; register struct Library *a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAppHandle *a0 __asm("a0") = handle; register const void *a1 __asm("a1") = data;
  __asm volatile("jsr -366(a6)" : "+r"(d0), "+r"(a0), "+r"(a1) : "r"(a6) : ZZ9K_APP_INLINE_CLOBBERS_A0_A1);
  return (int)d0; }
#define ZZ9KAppLoadMemory(handle, data, length) __ZZ9KAppLoadMemoryInline((handle), (data), (length))
static __inline int __ZZ9KAppStartInline(ZZ9KAppHandle *handle, const char *const *argv, uint32_t argc) {
  register uint32_t d0 __asm("d0") = argc; register struct Library *a6 __asm("a6") = ZZ9KBase;
  register ZZ9KAppHandle *a0 __asm("a0") = handle; register const char *const *a1 __asm("a1") = argv;
  __asm volatile("jsr -372(a6)" : "+r"(d0), "+r"(a0), "+r"(a1) : "r"(a6) : ZZ9K_APP_INLINE_CLOBBERS_A0_A1); return (int)d0; }
#define ZZ9KAppStart(handle, argv, argc) __ZZ9KAppStartInline((handle), (argv), (argc))
#define ZZ9K_APP_INLINE_1A(name, offset, type0, arg0) \
static __inline int __##name(type0 arg0) { register int d0 __asm("d0"); register struct Library *a6 __asm("a6") = ZZ9KBase; register type0 a0 __asm("a0") = arg0; __asm volatile("jsr " #offset "(a6)" : "=r"(d0), "+r"(a0) : "r"(a6) : ZZ9K_APP_INLINE_CLOBBERS_A0); return d0; }
ZZ9K_APP_INLINE_1A(ZZ9KAppStopInline, -378, ZZ9KAppHandle *, handle)
#define ZZ9KAppStop(handle) __ZZ9KAppStopInline((handle))
ZZ9K_APP_INLINE_2A(ZZ9KAppStatusInline, -384, ZZ9KAppHandle *, handle, ZZ9KAppStatusInfo *, status)
#define ZZ9KAppStatus(handle, status) __ZZ9KAppStatusInline((handle), (status))
ZZ9K_APP_INLINE_2A(ZZ9KAppSendMessageInline, -390, ZZ9KAppHandle *, handle, const ZZ9KAppMessage *, message)
#define ZZ9KAppSendMessage(handle, message) __ZZ9KAppSendMessageInline((handle), (message))
ZZ9K_APP_INLINE_2A(ZZ9KAppGetMessageInline, -396, ZZ9KAppHandle *, handle, ZZ9KAppMessage *, message)
#define ZZ9KAppGetMessage(handle, message) __ZZ9KAppGetMessageInline((handle), (message))
static __inline uint32_t __ZZ9KAppMessageSignalInline(const ZZ9KAppHandle *handle) {
  register uint32_t d0 __asm("d0"); register struct Library *a6 __asm("a6") = ZZ9KBase; register const ZZ9KAppHandle *a0 __asm("a0") = handle;
  __asm volatile("jsr -402(a6)" : "=r"(d0), "+r"(a0) : "r"(a6) : ZZ9K_APP_INLINE_CLOBBERS_A0); return d0; }
#define ZZ9KAppMessageSignal(handle) __ZZ9KAppMessageSignalInline((handle))
static __inline void __ZZ9KAppCloseInline(ZZ9KAppHandle *handle) {
  register struct Library *a6 __asm("a6") = ZZ9KBase; register ZZ9KAppHandle *a0 __asm("a0") = handle;
  __asm volatile("jsr -408(a6)" : "+r"(a0) : "r"(a6) : ZZ9K_APP_INLINE_CLOBBERS_A0_D0); }
#define ZZ9KAppClose(handle) __ZZ9KAppCloseInline((handle))
#endif
#endif
