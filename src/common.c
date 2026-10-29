#include "common.h"

/* ------------------------------------------------------------------ */
/* crypto                                                              */
/* ------------------------------------------------------------------ */

int bx_derive_psk(uint8_t psk[32], const char *password, const uint8_t salt[BX_SALT_LEN])
{
    /* Argon2id. Raise the limits (e.g. *_MODERATE) if you want slower guessing. */
    return crypto_pwhash(psk, 32, password, strlen(password), salt,
                         crypto_pwhash_OPSLIMIT_INTERACTIVE,
                         crypto_pwhash_MEMLIMIT_INTERACTIVE,
                         crypto_pwhash_ALG_DEFAULT);
}

int bx_chan_setup(bx_chan *c, int is_server,
                  const uint8_t my_pk[32], const uint8_t my_sk[32],
                  const uint8_t peer_pk[32], const uint8_t *psk)
{
    uint8_t rx[32], tx[32];
    int r;

    if (is_server)
        r = crypto_kx_server_session_keys(rx, tx, my_pk, my_sk, peer_pk);
    else
        r = crypto_kx_client_session_keys(rx, tx, my_pk, my_sk, peer_pk);
    if (r != 0)
        return -1;

    if (psk) {                       /* bind the password into both directions */
        crypto_generichash(c->rx, 32, rx, 32, psk, 32);
        crypto_generichash(c->tx, 32, tx, 32, psk, 32);
    } else {
        memcpy(c->rx, rx, 32);
        memcpy(c->tx, tx, 32);
    }
    c->txn = c->rxn = 0;
    sodium_memzero(rx, sizeof rx);
    sodium_memzero(tx, sizeof tx);
    return 0;
}

static void make_nonce(uint8_t nonce[crypto_secretbox_NONCEBYTES], uint64_t n)
{
    memset(nonce, 0, crypto_secretbox_NONCEBYTES);
    for (int i = 0; i < 8; i++)
        nonce[i] = (uint8_t)(n >> (8 * i));
}

size_t bx_seal(bx_chan *c, const uint8_t *plain, size_t n, uint8_t *out)
{
    uint8_t nonce[crypto_secretbox_NONCEBYTES];
    size_t clen = n + crypto_secretbox_MACBYTES;

    make_nonce(nonce, c->txn++);
    out[0] = (uint8_t)(clen >> 8);
    out[1] = (uint8_t)(clen & 0xff);
    crypto_secretbox_easy(out + 2, plain, n, nonce, c->tx);
    return 2 + clen;
}

int bx_open(bx_chan *c, const uint8_t *cipher, size_t clen, uint8_t *out)
{
    uint8_t nonce[crypto_secretbox_NONCEBYTES];

    if (clen < crypto_secretbox_MACBYTES)
        return -1;
    make_nonce(nonce, c->rxn);
    if (crypto_secretbox_open_easy(out, cipher, clen, nonce, c->rx) != 0)
        return -1;
    c->rxn++;
    return (int)(clen - crypto_secretbox_MACBYTES);
}

void bx_fingerprint(char out[48], const uint8_t pk[32])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t h[16];
    int o = 0;

    crypto_generichash(h, sizeof h, pk, 32, NULL, 0);
    for (int i = 0; i < 16; i++) {
        out[o++] = hex[h[i] >> 4];
        out[o++] = hex[h[i] & 15];
        if (i % 2 == 1 && i != 15)
            out[o++] = ':';
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* sockets                                                             */
/* ------------------------------------------------------------------ */

int bx_net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;
#endif
    return 0;
}

int bx_set_nonblock(sock_t s)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

int bx_would_block(int err)
{
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

int bx_send_all(sock_t s, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        int k = (int)send(s, p, (int)n, BX_SEND_FLAGS);
        if (k < 0) {
            if (SOCK_ERR() == E_INTR)
                continue;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

int bx_recv_exact(sock_t s, void *buf, size_t n, int timeout_ms)
{
    char *p = (char *)buf;
    size_t got = 0;

    while (got < n) {
        fd_set rf;
        struct timeval tv;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int r = select((int)s + 1, &rf, NULL, NULL, &tv);
        if (r <= 0)
            return -1;
        int k = (int)recv(s, p + got, (int)(n - got), 0);
        if (k <= 0)
            return -1;
        got += (size_t)k;
    }
    return 0;
}
