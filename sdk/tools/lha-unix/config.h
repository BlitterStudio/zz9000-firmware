/*
 * Local configuration for the embedded LHa for UNIX decoder subset.
 *
 * The source files in this directory come from jca02266/lha
 * 1.14i-ac20220213. This config keeps only the portable decoder path used by
 * zz9k-archive.
 */

#ifndef ZZ9K_LHA_UNIX_CONFIG_H
#define ZZ9K_LHA_UNIX_CONFIG_H

#define HAVE_CONFIG_H 1
#define STDC_HEADERS 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_STRDUP 1
#define HAVE_MEMSET 1
#define HAVE_STRCASECMP 1
#define HAVE_DECL_BASENAME 1
#define HAVE_LIMITS_H 1
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
#define HAVE_UID_T 1
#define HAVE_GID_T 1
#define HAVE_SSIZE_T 1
#define HAVE_UINT64_T 1
#define HAVE_LONG_LONG 1
#define SIZEOF_LONG 4
#define SIZEOF_OFF_T 4
#define HAVE_FSEEKO 0
#define HAVE_FTELLO 0
#define SUPPORT_LH7 1
#define RETSIGTYPE void
#define interrupt lha_interrupt

#endif
