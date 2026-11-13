/* Mbed TLS's mutex on Windows (nx_mbedtls_config.h: MBEDTLS_THREADING_ALT): an SRWLOCK, which is one pointer that
   starts at zero (SRWLOCK_INIT), kept as a void* so that the TLS headers don't pull in <windows.h>. The functions that
   lock it are in hle_ssl.cpp. */
#ifndef NX_MBEDTLS_THREADING_ALT_H
#define NX_MBEDTLS_THREADING_ALT_H

typedef struct mbedtls_threading_mutex_t {
    void* srw;   /* an SRWLOCK */
    char valid;  /* set by init, cleared by free: lock and unlock fail on a mutex that was never set up */
} mbedtls_threading_mutex_t;

#endif
