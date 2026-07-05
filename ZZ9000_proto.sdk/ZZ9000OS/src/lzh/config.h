/*
 * Firmware-local configuration for the embedded LHa for UNIX decoder core.
 *
 * The source files in this directory are vendored verbatim (byte-for-byte)
 * from the SDK's tools/lha-unix/ (itself sourced from jca02266/lha
 * 1.14i-ac20220213). This config is trimmed further than the SDK's own
 * config.h for a freestanding firmware build: only the decoder path used
 * by the ZZ9000 LZH decode-offload feature is enabled. Host-only, text
 * mode, EUC/SJIS filename conversion, and filesystem/archive-management
 * features are compiled out.
 *
 * text_mode is always FALSE in firmware use (binary decode only), so the
 * text/EUC conversion paths in the core are dead code here, not exercised.
 *
 * I/O (bitio.c/crcio.c) and the zz9k_lha_unix* glue are NOT part of this
 * directory -- Task 5 provides firmware buffer-backed replacements for
 * those, which is why this core compiles but does not yet link.
 */

#ifndef ZZ9K_FW_LHA_UNIX_CONFIG_H
#define ZZ9K_FW_LHA_UNIX_CONFIG_H

#define HAVE_CONFIG_H 1

/* Freestanding ARM newlib build: standard C headers are present. */
#define STDC_HEADERS 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_LIMITS_H 1

/* No libc strdup/strcasecmp/basename declaration assumed; the firmware
 * core does not call them (those live in the host-only archiver driver
 * files that are not vendored here). */
#define HAVE_STRDUP 0
#define HAVE_STRCASECMP 0
#define HAVE_DECL_BASENAME 0

/* No POSIX filesystem / user-database / signal-drama headers. */
#define HAVE_SYS_PARAM_H 0
#define HAVE_SYS_FILE_H 0
#define HAVE_UNISTD_H 0
#define HAVE_PWD_H 0
#define HAVE_GRP_H 0
#define HAVE_DIRENT_H 0
#define HAVE_FNMATCH_H 0
#define HAVE_LIBAPPLEFILE 0
#define HAVE_UTIME_H 0
#define HAVE_SYS_TIME_H 0
#define TIME_WITH_SYS_TIME 0

/* uid_t/gid_t/ssize_t/uint64_t: provided by the newlib headers pulled in
 * via sys/types.h, so no shims needed. */
#define HAVE_UID_T 1
#define HAVE_GID_T 1
#define HAVE_SSIZE_T 1
#define HAVE_UINT64_T 1
#define HAVE_LONG_LONG 1

/* ARM Cortex-A9 (ILP32): long is 32-bit. off_t is 32-bit in the firmware's
 * newlib (no large-file support needed -- archive members fit in RAM). */
#define SIZEOF_LONG 4
#define SIZEOF_OFF_T 4

#define HAVE_FSEEKO 0
#define HAVE_FTELLO 0

/* -lh7- support (64KB dictionary) -- keep parity with the SDK core. */
#define SUPPORT_LH7 1

#define RETSIGTYPE void
#define interrupt lha_interrupt

#endif /* ZZ9K_FW_LHA_UNIX_CONFIG_H */
