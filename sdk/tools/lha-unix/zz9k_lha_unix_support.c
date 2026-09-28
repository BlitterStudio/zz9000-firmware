/*
 * Compatibility glue for embedding the LHa for UNIX decoder subset.
 *
 * The bundled decoder sources are from jca02266/lha 1.14i-ac20220213 and
 * retain their original redistribution terms.
 */

#define HAVE_CONFIG_H 1
#define LHA_MAIN_SRC
#include "lha.h"

#include <stdarg.h>

int zz9k_lha_unix_error;

void lha_exit(int status)
{
    zz9k_lha_unix_error = status == 0 ? 1 : status;
}

void lha_interrupt(int signo)
{
    (void)signo;
    zz9k_lha_unix_error = 1;
}

void
warning(char *fmt, ...)
{
    (void)fmt;
}

void
error(char *fmt, ...)
{
    (void)fmt;
    zz9k_lha_unix_error = 1;
}

void
fatal_error(char *fmt, ...)
{
    (void)fmt;
    zz9k_lha_unix_error = 1;
}

void *
xmalloc(size_t size)
{
    void *p = malloc(size == 0 ? 1 : size);
    if (!p) {
        fatal_error("Not enough memory");
    }
    return p;
}

char *
xstrdup(char *str)
{
    size_t len;
    char *p;

    if (!str) {
        return NULL;
    }
    len = strlen(str);
    p = (char *)xmalloc(len + 1);
    if (p) {
        memcpy(p, str, len + 1);
    }
    return p;
}

void
init_sp(struct string_pool *sp)
{
    if (sp) {
        sp->used = 0;
        sp->size = 0;
        sp->n = 0;
        sp->buffer = NULL;
    }
}

void
add_sp(struct string_pool *sp, char *str, int len)
{
    (void)sp;
    (void)str;
    (void)len;
}

void
finish_sp(register struct string_pool *sp, int *v_count, char ***v_vector)
{
    (void)sp;
    if (v_count) {
        *v_count = 0;
    }
    if (v_vector) {
        *v_vector = NULL;
    }
}

void
start_indicator(char *name, long size, char *msg, long def_indicator_threshold)
{
    (void)name;
    (void)size;
    (void)msg;
    (void)def_indicator_threshold;
}

void
finish_indicator(char *name, char *msg)
{
    (void)name;
    (void)msg;
}

off_t
copyfile(FILE *f1, FILE *f2, off_t size, int text_flg, unsigned int *crcp)
{
    unsigned char buf[4096];
    off_t copied = 0;
    unsigned int crc;

    (void)text_flg;
    if (!f1 || !crcp) {
        zz9k_lha_unix_error = 1;
        return copied;
    }
    crc = *crcp;
    while (copied < size) {
        off_t remaining = size - copied;
        size_t want = remaining > (off_t)sizeof(buf) ?
            sizeof(buf) : (size_t)remaining;
        size_t got = fread(buf, 1, want, f1);
        if (got == 0) {
            zz9k_lha_unix_error = 1;
            break;
        }
        {
            size_t i;
            for (i = 0; i < got; i++) {
                crc = UPDATE_CRC(crc, buf[i]);
            }
        }
        if (f2 && fwrite(buf, 1, got, f2) != got) {
            zz9k_lha_unix_error = 1;
            break;
        }
        copied += (off_t)got;
    }
    *crcp = crc;
    return copied;
}
