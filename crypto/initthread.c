/*
 * Copyright 2019 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/crypto.h>
#include <openssl/core_numbers.h>
#include "internal/cryptlib_int.h"
#include "internal/providercommon.h"
#include "internal/thread_once.h"

#ifdef FIPS_MODE
/*
 * Thread aware code may want to be told about thread stop events. We register
 * to hear about those thread stop events when we see a new thread has started.
 * We call the ossl_init_thread_start function to do that. In the FIPS provider
 * we have our own copy of ossl_init_thread_start, which cascades notifications
 * about threads stopping from libcrypto to all the code in the FIPS provider
 * that needs to know about it.
 *
 * The FIPS provider tells libcrypto about which threads it is interested in
 * by calling "c_thread_start" which is a function pointer created during
 * provider initialisation (i.e. OSSL_init_provider).
 */
extern OSSL_core_thread_start_fn *c_thread_start;
#endif

typedef struct thread_event_handler_st THREAD_EVENT_HANDLER;
struct thread_event_handler_st {
    const void *index;
    void *arg;
    OSSL_thread_stop_handler_fn handfn;
    THREAD_EVENT_HANDLER *next;
};

#ifndef FIPS_MODE
DEFINE_SPECIAL_STACK_OF(THREAD_EVENT_HANDLER_PTR, THREAD_EVENT_HANDLER *)

typedef struct global_tevent_register_st GLOBAL_TEVENT_REGISTER;
struct global_tevent_register_st {
    STACK_OF(THREAD_EVENT_HANDLER_PTR) *skhands;
    CRYPTO_RWLOCK *lock;
};

static GLOBAL_TEVENT_REGISTER *glob_tevent_reg = NULL;

static CRYPTO_ONCE tevent_register_runonce = CRYPTO_ONCE_STATIC_INIT;

DEFINE_RUN_ONCE_STATIC(create_global_tevent_register)
{
    glob_tevent_reg = OPENSSL_zalloc(sizeof(*glob_tevent_reg));
    if (glob_tevent_reg == NULL)
        return 0;

    glob_tevent_reg->skhands = sk_THREAD_EVENT_HANDLER_PTR_new_null();
    glob_tevent_reg->lock = CRYPTO_THREAD_lock_new();
    if (glob_tevent_reg->skhands == NULL || glob_tevent_reg->lock == NULL) {
        sk_THREAD_EVENT_HANDLER_PTR_free(glob_tevent_reg->skhands);
        CRYPTO_THREAD_lock_free(glob_tevent_reg->lock);
        OPENSSL_free(glob_tevent_reg);
        glob_tevent_reg = NULL;
        return 0;
    }

    return 1;
}

static GLOBAL_TEVENT_REGISTER *get_global_tevent_register(void)
{
    if (!RUN_ONCE(&tevent_register_runonce, create_global_tevent_register))
        return NULL;
    return glob_tevent_reg;
}
#endif

static void init_thread_stop(void *arg, THREAD_EVENT_HANDLER **hands);

static THREAD_EVENT_HANDLER **
init_get_thread_local(CRYPTO_THREAD_LOCAL *local, int alloc, int keep)
{
    THREAD_EVENT_HANDLER **hands = CRYPTO_THREAD_get_local(local);

    if (alloc) {
        if (hands == NULL) {
#ifndef FIPS_MODE
            GLOBAL_TEVENT_REGISTER *gtr;
#endif

            if ((hands = OPENSSL_zalloc(sizeof(*hands))) == NULL) {
                OPENSSL_free(hands);
                return NULL;
            }

#ifndef FIPS_MODE
            /*
             * The thread event handler is thread specific and is a linked
             * list of all handler functions that should be called for the
             * current thread. We also keep a global reference to that linked
             * list, so that we can deregister handlers if necessary before all
             * the threads are stopped.
             */
            gtr = get_global_tevent_register();
            if (gtr == NULL) {
                OPENSSL_free(hands);
                return NULL;
            }
            CRYPTO_THREAD_write_lock(gtr->lock);
            if (!sk_THREAD_EVENT_HANDLER_PTR_push(gtr->skhands, hands)) {
                OPENSSL_free(hands);
                CRYPTO_THREAD_unlock(gtr->lock);
                return NULL;
            }
            CRYPTO_THREAD_unlock(gtr->lock);
#endif
            if (!CRYPTO_THREAD_set_local(local, hands)) {
                OPENSSL_free(hands);
                return NULL;
            }
        }
    } else if (!keep) {
        CRYPTO_THREAD_set_local(local, NULL);
    }

    return hands;
}

#ifndef FIPS_MODE
/*
 * Since per-thread-specific-data destructors are not universally
 * available, i.e. not on Windows, only below CRYPTO_THREAD_LOCAL key
 * is assumed to have destructor associated. And then an effort is made
 * to call this single destructor on non-pthread platform[s].
 *
 * Initial value is "impossible". It is used as guard value to shortcut
 * destructor for threads terminating before libcrypto is initialized or
 * after it's de-initialized. Access to the key doesn't have to be
 * serialized for the said threads, because they didn't use libcrypto
 * and it doesn't matter if they pick "impossible" or dereference real
 * key value and pull NULL past initialization in the first thread that
 * intends to use libcrypto.
 */
static union {
    long sane;
    CRYPTO_THREAD_LOCAL value;
} destructor_key = { -1 };

static void init_thread_remove_handlers(THREAD_EVENT_HANDLER **handsin)
{
    GLOBAL_TEVENT_REGISTER *gtr;
    int i;

    gtr = get_global_tevent_register();
    if (gtr == NULL)
        return;
    CRYPTO_THREAD_write_lock(gtr->lock);
    for (i = 0; i < sk_THREAD_EVENT_HANDLER_PTR_num(gtr->skhands); i++) {
        THREAD_EVENT_HANDLER **hands
            = sk_THREAD_EVENT_HANDLER_PTR_value(gtr->skhands, i);

        if (hands == handsin) {
            hands = sk_THREAD_EVENT_HANDLER_PTR_delete(gtr->skhands, i);
            CRYPTO_THREAD_unlock(gtr->lock);
            return;
        }
    }
    CRYPTO_THREAD_unlock(gtr->lock);
    return;
}

static void init_thread_destructor(void *hands)
{
    init_thread_stop(NULL, (THREAD_EVENT_HANDLER **)hands);
    init_thread_remove_handlers(hands);
    OPENSSL_free(hands);
}

int ossl_init_thread(void)
{
    if (!CRYPTO_THREAD_init_local(&destructor_key.value,
                                  init_thread_destructor))
        return 0;

    return 1;
}

static int init_thread_deregister(void *arg, int all);

void ossl_cleanup_thread(void)
{
    init_thread_deregister(NULL, 1);
    CRYPTO_THREAD_cleanup_local(&destructor_key.value);
    destructor_key.sane = -1;
}

void OPENSSL_thread_stop_ex(OPENSSL_CTX *ctx)
{
    ctx = openssl_ctx_get_concrete(ctx);
    /*
     * TODO(3.0). It would be nice if we could figure out a way to do this on
     * all threads that have used the OPENSSL_CTX when the OPENSSL_CTX is freed.
     * This is currently not possible due to the use of thread local variables.
     */
    ossl_ctx_thread_stop(ctx);
}

void OPENSSL_thread_stop(void)
{
    if (destructor_key.sane != -1) {
        THREAD_EVENT_HANDLER **hands
            = init_get_thread_local(&destructor_key.value, 0, 0);
        init_thread_stop(NULL, hands);

        init_thread_remove_handlers(hands);
        OPENSSL_free(hands);
    }
}

void ossl_ctx_thread_stop(void *arg)
{
    if (destructor_key.sane != -1) {
        THREAD_EVENT_HANDLER **hands
            = init_get_thread_local(&destructor_key.value, 0, 1);
        init_thread_stop(arg, hands);
    }
}

#else

static void *thread_event_ossl_ctx_new(OPENSSL_CTX *libctx)
{
    THREAD_EVENT_HANDLER **hands = NULL;
    CRYPTO_THREAD_LOCAL *tlocal = OPENSSL_zalloc(sizeof(*tlocal));

    if (tlocal == NULL)
        return NULL;

    if (!CRYPTO_THREAD_init_local(tlocal,  NULL)) {
        goto err;
    }

    hands = OPENSSL_zalloc(sizeof(*hands));
    if (hands == NULL)
        goto err;

    if (!CRYPTO_THREAD_set_local(tlocal, hands))
        goto err;

    return tlocal;
 err:
    OPENSSL_free(hands);
    OPENSSL_free(tlocal);
    return NULL;
}

static void thread_event_ossl_ctx_free(void *tlocal)
{
    OPENSSL_free(tlocal);
}

static const OPENSSL_CTX_METHOD thread_event_ossl_ctx_method = {
    thread_event_ossl_ctx_new,
    thread_event_ossl_ctx_free,
};

void ossl_ctx_thread_stop(void *arg)
{
    THREAD_EVENT_HANDLER **hands;
    OPENSSL_CTX *ctx = arg;
    CRYPTO_THREAD_LOCAL *local
        = openssl_ctx_get_data(ctx, OPENSSL_CTX_THREAD_EVENT_HANDLER_INDEX,
                               &thread_event_ossl_ctx_method);

    if (local == NULL)
        return;
    hands = init_get_thread_local(local, 0, 0);
    init_thread_stop(arg, hands);
    OPENSSL_free(hands);
}
#endif /* FIPS_MODE */


static void init_thread_stop(void *arg, THREAD_EVENT_HANDLER **hands)
{
    THREAD_EVENT_HANDLER *curr, *prev = NULL;

    /* Can't do much about this */
    if (hands == NULL)
        return;

    curr = *hands;
    while (curr != NULL) {
        if (arg != NULL && curr->arg != arg) {
            curr = curr->next;
            continue;
        }
        curr->handfn(curr->arg);
        prev = curr;
        curr = curr->next;
        if (prev == *hands)
            *hands = curr;
        OPENSSL_free(prev);
    }
}

int ossl_init_thread_start(const void *index, void *arg,
                           OSSL_thread_stop_handler_fn handfn)
{
    THREAD_EVENT_HANDLER **hands;
    THREAD_EVENT_HANDLER *hand;
#ifdef FIPS_MODE
    OPENSSL_CTX *ctx = arg;

    /*
     * In FIPS mode the list of THREAD_EVENT_HANDLERs is unique per combination
     * of OPENSSL_CTX and thread. This is because in FIPS mode each OPENSSL_CTX
     * gets informed about thread stop events individually.
     */
    CRYPTO_THREAD_LOCAL *local
        = openssl_ctx_get_data(ctx, OPENSSL_CTX_THREAD_EVENT_HANDLER_INDEX,
                               &thread_event_ossl_ctx_method);
#else
    /*
     * Outside of FIPS mode the list of THREAD_EVENT_HANDLERs is unique per
     * thread, but may hold multiple OPENSSL_CTXs. We only get told about
     * thread stop events globally, so we have to ensure all affected
     * OPENSSL_CTXs are informed.
     */
    CRYPTO_THREAD_LOCAL *local = &destructor_key.value;
#endif

    hands = init_get_thread_local(local, 1, 0);
    if (hands == NULL)
        return 0;

#ifdef FIPS_MODE
    if (*hands == NULL) {
        /*
         * We've not yet registered any handlers for this thread. We need to get
         * libcrypto to tell us about later thread stop events. c_thread_start
         * is a callback to libcrypto defined in fipsprov.c
         */
        if (!c_thread_start(FIPS_get_provider(ctx), ossl_ctx_thread_stop))
            return 0;
    }
#endif

    hand = OPENSSL_malloc(sizeof(*hand));
    if (hand == NULL)
        return 0;

    hand->handfn = handfn;
    hand->arg = arg;
    hand->index = index;
    hand->next = *hands;
    *hands = hand;

    return 1;
}

#ifndef FIPS_MODE
static int init_thread_deregister(void *index, int all)
{
    GLOBAL_TEVENT_REGISTER *gtr;
    int i;

    gtr = get_global_tevent_register();
    if (!all)
        CRYPTO_THREAD_write_lock(gtr->lock);
    for (i = 0; i < sk_THREAD_EVENT_HANDLER_PTR_num(gtr->skhands); i++) {
        THREAD_EVENT_HANDLER **hands
            = sk_THREAD_EVENT_HANDLER_PTR_value(gtr->skhands, i);
        THREAD_EVENT_HANDLER *curr = *hands, *prev = NULL, *tmp;

        if (hands == NULL) {
            if (!all)
                CRYPTO_THREAD_unlock(gtr->lock);
            return 0;
        }
        while (curr != NULL) {
            if (all || curr->index == index) {
                if (prev != NULL)
                    prev->next = curr->next;
                else
                    *hands = curr->next;
                tmp = curr;
                curr = curr->next;
                OPENSSL_free(tmp);
                continue;
            }
            prev = curr;
            curr = curr->next;
        }
        if (all)
            OPENSSL_free(hands);
    }
    if (all) {
        CRYPTO_THREAD_lock_free(gtr->lock);
        sk_THREAD_EVENT_HANDLER_PTR_free(gtr->skhands);
        OPENSSL_free(gtr);
    } else {
        CRYPTO_THREAD_unlock(gtr->lock);
    }
    return 1;
}

int ossl_init_thread_deregister(void *index)
{
    return init_thread_deregister(index, 0);
}
#endif
