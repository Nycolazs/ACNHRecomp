/* Mbed TLS options on top of its default configuration (CMakeLists.txt: MBEDTLS_USER_CONFIG_FILE, which the library
   and everything that includes its headers see alike).

   The threading layer: the game's nn::ssl connections, the npns poller (hle_npns.cpp) and the id_token signer
   (nextendo_token.cpp) use TLS from several threads at once, and TLS 1.3 keeps its handshake and traffic keys in PSA's
   key store, which is only safe across threads with MBEDTLS_THREADING_C. POSIX threads where there are some; on
   Windows the SRW locks in hle_ssl.cpp (tls_ready, mbedtls_threading_set_alt), with the mutex type in
   threading_alt.h. */
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif
