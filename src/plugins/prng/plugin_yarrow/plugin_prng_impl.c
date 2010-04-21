/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Copyright (C) 2001, 2002, 2004, 2007, 2008 by the Massachusetts Institute of Technology.
 * All rights reserved.
 *
 *
 * Export of this software from the United States of America may require
 * a specific license from the United States Government.  It is the
 * responsibility of any person or organization contemplating export to
 * obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 */

#include "k5-int.h"
#include "enc_provider.h"
#include <assert.h>
#include "k5-thread.h"

#include <plugin_manager.h>
#include <plugin_prng.h>
#include "plugin_prng.h"
#include "plugin_prng_impl.h"

#include "yarrow.h"

static Yarrow_CTX y_ctx;
#define yarrow_lock krb5int_yarrow_lock
k5_mutex_t yarrow_lock = K5_MUTEX_PARTIAL_INITIALIZER;


/* Helper function to estimate entropy based on sample length
 * and where it comes from.
 */

static size_t
entropy_estimate(unsigned int randsource, size_t length)
{
    switch (randsource) {
    case KRB5_C_RANDSOURCE_OLDAPI:
        return 4 * length;
    case KRB5_C_RANDSOURCE_OSRAND:
        return 8 * length;
    case KRB5_C_RANDSOURCE_TRUSTEDPARTY:
        return 4 * length;
    case KRB5_C_RANDSOURCE_TIMING:
        return 2;
    case KRB5_C_RANDSOURCE_EXTERNAL_PROTOCOL:
        return 0;
    default:
        abort();
    }
    return 0;
}


/*
 * Routines to get entropy from the OS.  For UNIX we try /dev/urandom
 * and /dev/random.  Currently we don't do anything for Windows.
 */
#if defined(_WIN32)

static krb5_error_code
_plugin_prng_os_seed(krb5_context context, int strong, int *success)
{
    if (success)
        *success = 0;
    return 0;
}

#else /*Windows*/
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

/*
 * Helper function to read entropy from  a random device.  Takes the
 * name of a device, opens it, makes sure it is a device and if so,
 * reads entropy.  Returns  a boolean indicating whether entropy was
 * read.
 */

static krb5_error_code
read_entropy_from_device(krb5_context context, const char *device)
{
    krb5_data data;
    struct stat sb;
    int fd;
    unsigned char buf[YARROW_SLOW_THRESH/8], *bp;
    int left;

    fd = open (device, O_RDONLY);
    if (fd == -1)
        return 0;
    set_cloexec_fd(fd);
    if (fstat(fd, &sb) == -1 || S_ISREG(sb.st_mode)) {
        close(fd);
        return 0;
    }

    for (bp = buf, left = sizeof(buf); left > 0;) {
        ssize_t count;
        count = read(fd, bp, (unsigned) left);
        if (count <= 0) {
            close(fd);
            return 0;
        }
        left -= count;
        bp += count;
    }
    close(fd);
    data.length = sizeof (buf);
    data.data = (char *) buf;
    return (krb5_c_random_add_entropy(context, KRB5_C_RANDSOURCE_OSRAND,
                                      &data) == 0);
}

static krb5_error_code
_plugin_prng_os_seed(krb5_context context, int strong, int *success)
{
    int unused;
    int *oursuccess = success ? success : &unused;

    *oursuccess = 0;
    /* If we are getting strong data then try that first.  We are
       guaranteed to cause a reseed of some kind if strong is true and
       we have both /dev/random and /dev/urandom.  We want the strong
       data included in the reseed so we get it first.*/
    if (strong) {
        if (read_entropy_from_device(context, "/dev/random"))
            *oursuccess = 1;
    }
    if (read_entropy_from_device(context, "/dev/urandom"))
        *oursuccess = 1;
    return 0;
}

#endif /*Windows or pre-OSX Mac*/

static krb5_error_code
_plugin_prng_seed(krb5_context context, unsigned int randsource,
                          const krb5_data *data)
{
    int yerr;

    /* Make sure the mutex got initialized.  */
    yerr = krb5int_crypto_init();
    if (yerr)
        return yerr;
    /* Now, finally, feed in the data.  */
    yerr = krb5int_yarrow_input(&y_ctx, randsource,
                                data->data, data->length,
                                entropy_estimate(randsource, data->length));
    if (yerr != YARROW_OK)
        return KRB5_CRYPTO_INTERNAL;
    return 0;
}

static krb5_error_code
_plugin_prng_rand(krb5_context context, krb5_data *data)
{
    int yerr;
    yerr = krb5int_yarrow_output(&y_ctx, data->data, data->length);
    if (yerr == YARROW_NOT_SEEDED) {
        yerr = krb5int_yarrow_reseed(&y_ctx, YARROW_SLOW_POOL);
        if (yerr == YARROW_OK)
            yerr = krb5int_yarrow_output(&y_ctx, data->data, data->length);
    }
    if (yerr != YARROW_OK)
        return KRB5_CRYPTO_INTERNAL;
    return 0;
}


static void
_plugin_prng_destroy(plugin_prng* api)
{
        if (api != NULL) {
                free(api);
        }
}

static krb5_error_code
_plugin_prng_init(void)
{
    unsigned i, source_id;
    int yerr;

    yerr = k5_mutex_finish_init(&yarrow_lock);
    if (yerr)
        return yerr;

    yerr = krb5int_yarrow_init (&y_ctx, NULL);
    if (yerr != YARROW_OK && yerr != YARROW_NOT_SEEDED)
        return KRB5_CRYPTO_INTERNAL;

    for (i=0; i < KRB5_C_RANDSOURCE_MAX; i++ ) {
        if (krb5int_yarrow_new_source(&y_ctx, &source_id) != YARROW_OK)
            return KRB5_CRYPTO_INTERNAL;
        assert (source_id == i);
    }

    return 0;
}

static void
_plugin_prng_cleanup (void)
{
    krb5int_yarrow_final (&y_ctx);
    k5_mutex_destroy(&yarrow_lock);
}


plhandle
plugin_yarrow_prng_create()
{
        plhandle handle;
        plugin_prng* api = malloc(sizeof(plugin_prng));

        memset(api, 0, sizeof(plugin_prng));
        api->version = 0;
        api->prng_rand = _plugin_prng_rand;
        api->prng_seed = _plugin_prng_seed;
        api->prng_os_seed = _plugin_prng_os_seed;
        api->prng_init = _plugin_prng_init;
        api->prng_cleanup = _plugin_prng_cleanup;
        handle.api = api;

        return handle;
}
