/*
 * BXChat - common definitions (platform layer + encrypted channel API)
 */
#ifndef BXCHAT_COMMON_H
#define BXCHAT_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sodium.h>

#ifdef _WIN32
  #ifndef FD_SETSIZE
  #define FD_SETSIZE 256
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_BAD      INVALID_SOCKET
  #define sock_close    closesocket
  #define SOCK_ERR()    WSAGetLastError()
  #define E_INTR        WSAEINTR
  #define strcasecmp    _stricmp
  #define strncasecmp   _strnicmp
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <strings.h>
  typedef int sock_t;
  #define SOCK_BAD      (-1)
  #define sock_close    close
  #define SOCK_ERR()    errno
  #define E_INTR        EINTR
#endif

#ifdef MSG_NOSIGNAL
  #define BX_SEND_FLAGS MSG_NOSIGNAL
#else
  #define BX_SEND_FLAGS 0
#endif

#ifdef __GNUC__
  #define BX_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
  #define BX_PRINTF(a, b)
#endif

/* ---- protocol ---------------------------------------------------------
 *
 *  server -> client (clear):  "BXC1" | flags(1) | salt(16) | server_pk(32)
 *  client -> server (clear):  client_pk(32)
 *  afterwards, every message is a frame:
 *        len(2, big endian) | secretbox(plaintext)      len = n + MACBYTES
 *
 *  Session keys come from an ephemeral X25519 exchange (crypto_kx). If the
 *  server has a password, Argon2id(password, salt) is mixed into both session
 *  keys, so only someone who knows the password can talk to the server (and
 *  nobody without it can impersonate the server).
 */
#define BX_DEFAULT_PORT   "7000"
#define BX_MAGIC          "BXC1"
#define BX_FLAG_PASSWORD  0x01
#define BX_SALT_LEN       16
#define BX_PK_LEN         32
#define BX_HS_SERVER_LEN  (4 + 1 + BX_SALT_LEN + BX_PK_LEN)
#define BX_HS_CLIENT_LEN  BX_PK_LEN
#define BX_MAX_PLAIN      1024
#define BX_MAX_CIPHER     (BX_MAX_PLAIN + crypto_secretbox_MACBYTES)
#define BX_MAX_FRAME      (2 + BX_MAX_CIPHER)

typedef struct {
    uint8_t  tx[32];
    uint8_t  rx[32];
    uint64_t txn;      /* nonce counters: replay / reorder / drop => MAC fail */
    uint64_t rxn;
} bx_chan;

int    bx_derive_psk(uint8_t psk[32], const char *password, const uint8_t salt[BX_SALT_LEN]);
int    bx_chan_setup(bx_chan *c, int is_server,
                     const uint8_t my_pk[32], const uint8_t my_sk[32],
                     const uint8_t peer_pk[32], const uint8_t *psk /* or NULL */);
/* returns total frame size written to out (>= 2 + n + MAC), out must hold BX_MAX_FRAME */
size_t bx_seal(bx_chan *c, const uint8_t *plain, size_t n, uint8_t *out);
/* returns plaintext length, or -1 on authentication failure */
int    bx_open(bx_chan *c, const uint8_t *cipher, size_t clen, uint8_t *out);
void   bx_fingerprint(char out[48], const uint8_t pk[32]);

/* ---- networking helpers ---------------------------------------------- */
int  bx_net_init(void);
int  bx_set_nonblock(sock_t s);
int  bx_would_block(int err);
int  bx_send_all(sock_t s, const void *buf, size_t n);
int  bx_recv_exact(sock_t s, void *buf, size_t n, int timeout_ms);

#endif
