/*
 * bxclient - BXChat client with a BitchX-flavoured terminal UI.
 *
 *   bxclient [-p port] [-n nick] [-w password] host
 *   (password may also come from the BXCHAT_PASSWORD environment variable)
 *
 * Layout:  scrolling chat region / blue status bar / input line.
 * Needs an ANSI/VT terminal (any Unix terminal, Windows 10+ console).
 */
#include "common.h"

#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>

#ifdef _WIN32
  #include <conio.h>
  #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
  #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
  #endif
#else
  #include <termios.h>
  #include <sys/ioctl.h>
#endif

/* ================================================================== */
/* output buffer + raw terminal                                        */
/* ================================================================== */

static char  *ob;
static size_t oblen, obcap;
static int    g_w = 80, g_h = 24;
static int    g_term_ready;

static void ob_add(const char *s, size_t n)
{
    if (oblen + n + 1 > obcap) {
        obcap = (oblen + n + 1) * 2 + 4096;
        ob = (char *)realloc(ob, obcap);
        if (!ob) exit(1);
    }
    memcpy(ob + oblen, s, n);
    oblen += n;
}
static void ob_str(const char *s) { ob_add(s, strlen(s)); }
static void ob_printf(const char *fmt, ...) BX_PRINTF(1, 2);
static void ob_printf(const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
    ob_add(tmp, (size_t)n);
}

static void term_write(const char *p, size_t n)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD w;
    while (n) {
        if (!WriteFile(h, p, (DWORD)n, &w, NULL) || w == 0) return;
        p += w;
        n -= w;
    }
#else
    while (n) {
        ssize_t k = write(1, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += k;
        n -= (size_t)k;
    }
#endif
}

static void ob_flush(void)
{
    if (!oblen) return;
    term_write("\x1b[?25l", 6);
    term_write(ob, oblen);
    term_write("\x1b[?25h", 6);
    oblen = 0;
}

#ifdef _WIN32
static DWORD g_old_in, g_old_out;
static UINT  g_old_cp;
#else
static struct termios g_old_tio;
#endif

static void term_size(int *w, int *h)
{
    int ww = 80, hh = 24;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &i)) {
        ww = i.srWindow.Right - i.srWindow.Left + 1;
        hh = i.srWindow.Bottom - i.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        ww = ws.ws_col;
        hh = ws.ws_row;
    }
#endif
    if (ww < 20) ww = 20;
    if (hh < 5) hh = 5;
    *w = ww;
    *h = hh;
}

static void term_restore(void)
{
    char b[64];
    if (!g_term_ready) return;
    g_term_ready = 0;
    snprintf(b, sizeof b, "\x1b[r\x1b[0m\x1b[?25h\x1b[%d;1H\r\n", g_h);
    term_write(b, strlen(b));
#ifdef _WIN32
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_old_in);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_old_out);
    SetConsoleOutputCP(g_old_cp);
#else
    tcsetattr(0, TCSANOW, &g_old_tio);
#endif
}

static int term_init(void)
{
#ifdef _WIN32
    HANDLE hi = GetStdHandle(STD_INPUT_HANDLE), ho = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(hi, &g_old_in) || !GetConsoleMode(ho, &g_old_out)) {
        fprintf(stderr, "bxclient needs a real console window\n");
        return -1;
    }
    SetConsoleMode(hi, g_old_in & ~(DWORD)(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    if (!SetConsoleMode(ho, g_old_out | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        SetConsoleMode(hi, g_old_in);
        fprintf(stderr, "this console has no ANSI support (need Windows 10+ / Windows Terminal)\n");
        return -1;
    }
    g_old_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#else
    if (!isatty(0) || !isatty(1)) {
        fprintf(stderr, "bxclient needs an interactive terminal\n");
        return -1;
    }
    tcgetattr(0, &g_old_tio);
    struct termios t = g_old_tio;
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | BRKINT | ISTRIP | INPCK);
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
#endif
    g_term_ready = 1;
    term_size(&g_w, &g_h);
    return 0;
}

/* ================================================================== */
/* keyboard                                                            */
/* ================================================================== */

#define K_NONE    (-1)
#define K_IGNORE  0x200000
enum { K_UP = 0x200001, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_PGUP, K_PGDN, K_ENTER, K_BS };

#ifdef _WIN32
static int term_next_key(void)
{
    if (!_kbhit()) return K_NONE;
    int c = _getwch();
    if (c == 0 || c == 0xE0) {
        switch (_getwch()) {
        case 72: return K_UP;    case 80: return K_DOWN;
        case 75: return K_LEFT;  case 77: return K_RIGHT;
        case 71: return K_HOME;  case 79: return K_END;
        case 83: return K_DEL;   case 73: return K_PGUP;
        case 81: return K_PGDN;  default: return K_IGNORE;
        }
    }
    if (c >= 0xD800 && c <= 0xDBFF) {                 /* surrogate pair */
        int lo = _getwch();
        return 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
    }
    if (c == 8 || c == 127) return K_BS;
    if (c == '\r' || c == '\n') return K_ENTER;
    return c;
}
static int term_fill(void) { return 0; }
#else
static unsigned char kb[512];
static size_t kbn, kbp;

static int term_fill(void)
{
    ssize_t n = read(0, kb, sizeof kb);
    if (n <= 0) return -1;
    kbn = (size_t)n;
    kbp = 0;
    return 0;
}

static int csi_key(int fin, int p0)
{
    switch (fin) {
    case 'A': return K_UP;    case 'B': return K_DOWN;
    case 'C': return K_RIGHT; case 'D': return K_LEFT;
    case 'H': return K_HOME;  case 'F': return K_END;
    case '~':
        switch (p0) {
        case 1: case 7: return K_HOME;
        case 4: case 8: return K_END;
        case 3: return K_DEL;
        case 5: return K_PGUP;
        case 6: return K_PGDN;
        }
    }
    return K_IGNORE;
}

static int term_next_key(void)
{
    if (kbp >= kbn) return K_NONE;
    unsigned c = kb[kbp++];

    if (c == 27) {
        if (kbp >= kbn) return K_IGNORE;
        unsigned d = kb[kbp++];
        if (d == 'O') {
            if (kbp >= kbn) return K_IGNORE;
            return csi_key(kb[kbp++], 0);
        }
        if (d == '[') {
            int p[2] = { 0, 0 }, pi = 0;
            while (kbp < kbn) {
                unsigned e = kb[kbp++];
                if (e >= '0' && e <= '9') p[pi] = p[pi] * 10 + (int)(e - '0');
                else if (e == ';') { if (pi < 1) pi++; }
                else if (e >= 0x40 && e <= 0x7e) return csi_key((int)e, p[0]);
            }
        }
        return K_IGNORE;
    }
    if (c >= 0xC0) {
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        int cp = (int)(c & (0xFFu >> (len + 1)));
        for (int i = 1; i < len; i++) {
            if (kbp >= kbn) return K_IGNORE;
            unsigned e = kb[kbp++];
            if ((e & 0xC0) != 0x80) return K_IGNORE;
            cp = (cp << 6) | (int)(e & 0x3F);
        }
        return cp;
    }
    if (c >= 0x80) return K_IGNORE;
    if (c == 127 || c == 8) return K_BS;
    if (c == '\r' || c == '\n') return K_ENTER;
    return (int)c;
}
#endif

/* ================================================================== */
/* UTF-8 helpers                                                       */
/* ================================================================== */

static int utf8_enc(int cp, char *o)
{
    if (cp < 0x80)          { o[0] = (char)cp; return 1; }
    if (cp < 0x800)         { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp < 0x10000)       { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63));
                              o[2] = (char)(0x80 | (cp & 63)); return 3; }
    if (cp <= 0x10FFFF)     { o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63));
                              o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4; }
    return 0;
}
static int u8_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static size_t u8_prev(const char *s, size_t i)
{
    if (i == 0) return 0;
    i--;
    while (i > 0 && u8_cont(s[i])) i--;
    return i;
}
static size_t u8_next(const char *s, size_t n, size_t i)
{
    if (i >= n) return n;
    i++;
    while (i < n && u8_cont(s[i])) i++;
    return i;
}
static size_t u8_cols(const char *s, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (!u8_cont(s[i])) c++;
    return c;
}
/* byte offset of the k-th character */
static size_t u8_off(const char *s, size_t n, size_t k)
{
    size_t i = 0;
    while (k-- && i < n) i = u8_next(s, n, i);
    return i;
}
/* bytes needed to show at most `cols` characters of a NUL-terminated string */
static size_t u8_take(const char *s, size_t cols)
{
    return u8_off(s, strlen(s), cols);
}

static void strip_ctrl(char *s)
{
    char *w = s;
    for (; *s; s++)
        if ((unsigned char)*s >= 0x20 && (unsigned char)*s != 0x7f)
            *w++ = *s;
    *w = 0;
}

/* ================================================================== */
/* scrollback + UI                                                     */
/* ================================================================== */

#define LOGMAX 2000
static char *logl[LOGMAX];
static int   log_head, log_n;
static int   scroll_off;

static char g_nick[32], g_chan[32], g_topic[256];
static int  g_count;
static int  g_connected;
static int  g_quit;

static int need_full = 1, need_status, need_input;

static char  inbuf[512];
static size_t inlen, incur;
static char  hist[64][512], hsave[512];
static int   hn, hpos;

static void log_add(const char *line)
{
    free(logl[log_head]);
    logl[log_head] = strdup(line);
    log_head = (log_head + 1) % LOGMAX;
    if (log_n < LOGMAX) log_n++;
}
static const char *log_get(int back)
{
    int i = ((log_head - 1 - back) % LOGMAX + LOGMAX) % LOGMAX;
    return logl[i] ? logl[i] : "";
}
static int max_scroll(void)
{
    int rows = g_h - 2;
    return log_n > rows ? log_n - rows : 0;
}

static void ui_status_draw(void)
{
    char left[600], right[96];
    char where[96];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    if (g_chan[0]) snprintf(where, sizeof where, "%s:%d", g_chan, g_count);
    else snprintf(where, sizeof where, "no channel");
    snprintf(left, sizeof left, " [%02d:%02d] [%s] [%s]%s%s", tm->tm_hour, tm->tm_min,
             g_nick[0] ? g_nick : "?", where, g_topic[0] ? " " : "", g_topic);
    snprintf(right, sizeof right, "%s%s ", scroll_off ? "(scrollback) " : "",
             g_connected ? "BXChat [encrypted]" : "BXChat [DISCONNECTED]");

    size_t rc = u8_cols(right, strlen(right));
    if ((int)rc > g_w) {
        right[u8_take(right, (size_t)g_w)] = 0;
        rc = u8_cols(right, strlen(right));
    }
    size_t maxl = (size_t)g_w - rc;
    size_t lb = u8_take(left, maxl);
    size_t lc = u8_cols(left, lb);

    ob_printf("\x1b[%d;1H\x1b[44;97m", g_h - 1);
    ob_add(left, lb);
    for (size_t i = lc; i < maxl; i++) ob_add(" ", 1);
    ob_str(right);
    ob_str("\x1b[0m");
}

static void ui_input_draw(void)
{
    char prompt[64];
    snprintf(prompt, sizeof prompt, "[%s] ", g_nick[0] ? g_nick : "?");
    int pc = (int)u8_cols(prompt, strlen(prompt));
    int avail = g_w - pc - 1;
    if (avail < 1) avail = 1;

    size_t total = u8_cols(inbuf, inlen), curc = u8_cols(inbuf, incur);
    size_t off = 0;
    if (curc >= (size_t)avail) off = curc - (size_t)avail + 1;
    size_t end = off + (size_t)avail < total ? off + (size_t)avail : total;
    size_t b0 = u8_off(inbuf, inlen, off), b1 = u8_off(inbuf, inlen, end);

    ob_printf("\x1b[%d;1H\x1b[2K\x1b[94m[\x1b[97m%s\x1b[94m]\x1b[0m ", g_h, g_nick[0] ? g_nick : "?");
    ob_add(inbuf + b0, b1 - b0);
    ob_printf("\x1b[%d;%dH", g_h, pc + (int)(curc - off) + 1);
}

static void ui_full(void)
{
    int rows = g_h - 2;
    int avail = log_n - scroll_off;
    int k = avail < rows ? avail : rows;

    ob_printf("\x1b[0m\x1b[2J\x1b[1;%dr", rows);
    for (int i = k - 1; i >= 0; i--) {
        ob_printf("\x1b[%d;1H\n", rows);
        ob_str(log_get(scroll_off + i));
        ob_str("\x1b[0m");
    }
    ui_status_draw();
    ui_input_draw();
}

static void ui_print_line(const char *line)
{
    log_add(line);
    if (scroll_off > 0) {                 /* keep the scrolled view anchored */
        if (scroll_off < max_scroll()) scroll_off++;
        need_status = 1;
        return;
    }
    ob_printf("\x1b[%d;1H\n", g_h - 2);
    ob_str(line);
    ob_str("\x1b[0m");
    need_input = 1;
}

static void say(const char *fmt, ...) BX_PRINTF(1, 2);
static void say(const char *fmt, ...)
{
    char body[1400], line[1600];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "\x1b[94m[\x1b[97m%02d:%02d\x1b[94m]\x1b[0m %s",
             tm->tm_hour, tm->tm_min, body);
    ui_print_line(line);
}

static int nick_color(const char *n)
{
    static const int pal[] = { 91, 92, 93, 94, 95, 96 };
    unsigned h = 5381;
    for (; *n; n++) h = h * 33 + (unsigned)tolower((unsigned char)*n);
    return pal[h % 6];
}

/* ================================================================== */
/* network                                                             */
/* ================================================================== */

static sock_t  g_sock = SOCK_BAD;
static bx_chan g_ch;
static uint8_t g_rbuf[8192];
static size_t  g_rlen;

static int net_send_line(const char *s, size_t n)
{
    uint8_t frame[BX_MAX_FRAME];
    if (n == 0 || n > BX_MAX_PLAIN) return 0;
    size_t fl = bx_seal(&g_ch, (const uint8_t *)s, n, frame);
    return bx_send_all(g_sock, frame, fl);
}

static void disconnect(const char *why)
{
    if (!g_connected) return;
    g_connected = 0;
    sock_close(g_sock);
    g_sock = SOCK_BAD;
    say("\x1b[91m***\x1b[0m Disconnected: %s. Type /quit (or Ctrl-C) to exit.", why);
    need_status = 1;
}

static void on_server_msg(const uint8_t *p, size_t n)
{
    char text[BX_MAX_PLAIN + 1];
    char *f[4] = { NULL, NULL, NULL, NULL };
    int nf = 1;
    char type;

    if (n < 1) return;
    type = (char)p[0];
    memcpy(text, p + 1, n - 1);
    text[n - 1] = 0;

    f[0] = text;
    if (strchr("MAPQS", type)) {
        for (char *q = text; *q && nf < 4; q++)
            if (*q == '\t') { *q = 0; f[nf++] = q + 1; }
    }
    for (int i = 0; i < 4; i++) {
        if (!f[i]) f[i] = text + n - 1;          /* writable empty string (text's NUL) */
        strip_ctrl(f[i]);
    }

    switch (type) {
    case 'M': say("\x1b[36m<\x1b[%dm%s\x1b[36m>\x1b[0m %s", nick_color(f[0]), f[0], f[1]); break;
    case 'A': say("\x1b[95m* %s %s\x1b[0m", f[0], f[1]); break;
    case 'P': say("\x1b[91m*\x1b[93m%s\x1b[91m*\x1b[0m %s", f[0], f[1]); ob_str("\a"); need_input = 1; break;
    case 'Q': say("\x1b[92m->\x1b[0m *\x1b[93m%s\x1b[0m* %s", f[0], f[1]); break;
    case 'I': say("\x1b[96m***\x1b[0m %s", f[0]); break;
    case 'J': say("\x1b[92m***\x1b[0m %s", f[0]); break;
    case 'L': say("\x1b[93m***\x1b[0m %s", f[0]); break;
    case 'E': say("\x1b[91m***\x1b[0m %s", f[0]); break;
    case 'S':
        snprintf(g_nick, sizeof g_nick, "%s", f[0]);
        snprintf(g_chan, sizeof g_chan, "%s", f[1]);
        g_count = atoi(f[2]);
        snprintf(g_topic, sizeof g_topic, "%s", f[3]);
        need_status = 1;
        break;
    default: break;
    }
}

static void net_read(void)
{
    int k = (int)recv(g_sock, (char *)g_rbuf + g_rlen, (int)(sizeof g_rbuf - g_rlen), 0);
    if (k == 0) { disconnect("connection closed by server"); return; }
    if (k < 0) {
        int e = SOCK_ERR();
        if (bx_would_block(e) || e == E_INTR) return;
        disconnect("network error");
        return;
    }
    g_rlen += (size_t)k;

    while (g_rlen >= 2) {
        size_t clen = ((size_t)g_rbuf[0] << 8) | g_rbuf[1];
        if (clen < crypto_secretbox_MACBYTES + 1 || clen > BX_MAX_CIPHER) {
            disconnect("protocol error");
            return;
        }
        if (g_rlen < 2 + clen) break;
        uint8_t plain[BX_MAX_PLAIN + 1];
        int n = bx_open(&g_ch, g_rbuf + 2, clen, plain);
        if (n < 0) {
            disconnect("decryption failed (data tampered with?)");
            return;
        }
        memmove(g_rbuf, g_rbuf + 2 + clen, g_rlen - 2 - clen);
        g_rlen -= 2 + clen;
        on_server_msg(plain, (size_t)n);
    }
}

static sock_t net_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res, *rp;
    sock_t s = SOCK_BAD;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "cannot resolve %s: %s\n", host, gai_strerror(rc));
        return SOCK_BAD;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == SOCK_BAD) continue;
        if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        sock_close(s);
        s = SOCK_BAD;
    }
    freeaddrinfo(res);
    if (s == SOCK_BAD) fprintf(stderr, "cannot connect to %s:%s\n", host, port);
    return s;
}

static uint8_t g_first[BX_MAX_PLAIN + 1];
static int     g_first_len;
static char    g_fp[48];

static int do_handshake(const char *password, const char *nick)
{
    uint8_t hs[BX_HS_SERVER_LEN], pk[32], sk[32], psk[32];
    const uint8_t *pskp = NULL;

    if (bx_recv_exact(g_sock, hs, sizeof hs, 10000) != 0) {
        fprintf(stderr, "no handshake from server (is it a BXChat server?)\n");
        return -1;
    }
    if (memcmp(hs, BX_MAGIC, 4) != 0) {
        fprintf(stderr, "not a BXChat server (or incompatible version)\n");
        return -1;
    }
    int need_pw = (hs[4] & BX_FLAG_PASSWORD) != 0;
    const uint8_t *salt = hs + 5, *spk = hs + 5 + BX_SALT_LEN;

    if (need_pw && !password) {
        fprintf(stderr, "this server requires a password (use -w or BXCHAT_PASSWORD)\n");
        return -1;
    }
    if (!need_pw && password)
        fprintf(stderr, "note: server has no password; yours is NOT being sent\n");
    if (need_pw) {
        fprintf(stderr, "deriving key... ");
        if (bx_derive_psk(psk, password, salt) != 0) {
            fprintf(stderr, "failed (out of memory?)\n");
            return -1;
        }
        fprintf(stderr, "ok\n");
        pskp = psk;
    }

    crypto_kx_keypair(pk, sk);
    if (bx_send_all(g_sock, pk, sizeof pk) != 0) return -1;
    if (bx_chan_setup(&g_ch, 0, pk, sk, spk, pskp) != 0) {
        fprintf(stderr, "key exchange failed\n");
        return -1;
    }
    sodium_memzero(sk, sizeof sk);
    sodium_memzero(psk, sizeof psk);
    bx_fingerprint(g_fp, spk);

    char hello[64];
    int hl = snprintf(hello, sizeof hello, "HELLO %s", nick);
    if (net_send_line(hello, (size_t)hl) != 0) return -1;

    /* the first frame back proves the keys match (wrong password => server hangs up) */
    uint8_t hdr[2], cbuf[BX_MAX_CIPHER];
    if (bx_recv_exact(g_sock, hdr, 2, 15000) != 0) {
        fprintf(stderr, "server closed the connection during login%s\n",
                need_pw ? " - wrong password?" : "");
        return -1;
    }
    size_t clen = ((size_t)hdr[0] << 8) | hdr[1];
    if (clen < crypto_secretbox_MACBYTES + 1 || clen > BX_MAX_CIPHER ||
        bx_recv_exact(g_sock, cbuf, clen, 15000) != 0) {
        fprintf(stderr, "login failed (bad reply from server)\n");
        return -1;
    }
    g_first_len = bx_open(&g_ch, cbuf, clen, g_first);
    if (g_first_len < 0) {
        fprintf(stderr, "login failed: could not authenticate server%s\n",
                need_pw ? " (wrong password?)" : "");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/* line editor                                                         */
/* ================================================================== */

static void ed_set(const char *s)
{
    snprintf(inbuf, sizeof inbuf, "%s", s);
    inlen = incur = strlen(inbuf);
}

static void ed_insert(int cp)
{
    char t[4];
    int l = utf8_enc(cp, t);
    if (!l || inlen + (size_t)l >= sizeof inbuf - 1) return;
    memmove(inbuf + incur + l, inbuf + incur, inlen - incur);
    memcpy(inbuf + incur, t, (size_t)l);
    inlen += (size_t)l;
    incur += (size_t)l;
}

static void ed_delete_range(size_t a, size_t b)   /* delete bytes [a,b) */
{
    memmove(inbuf + a, inbuf + b, inlen - b);
    inlen -= b - a;
    if (incur > b) incur -= b - a;
    else if (incur > a) incur = a;
}

static void submit(void)
{
    char line[512];
    if (scroll_off) { scroll_off = 0; need_full = 1; }
    if (inlen == 0) return;
    memcpy(line, inbuf, inlen);
    line[inlen] = 0;
    size_t len = inlen;

    if (hn == 0 || strcmp(hist[hn - 1], line) != 0) {
        if (hn == 64) { memmove(hist[0], hist[1], 63 * sizeof hist[0]); hn--; }
        snprintf(hist[hn++], sizeof hist[0], "%s", line);
    }
    hpos = hn;
    inlen = incur = 0;
    inbuf[0] = 0;
    need_input = 1;

    if (!strcmp(line, "/clear") || !strcmp(line, "/cls")) {
        for (int i = 0; i < LOGMAX; i++) { free(logl[i]); logl[i] = NULL; }
        log_n = log_head = 0;
        need_full = 1;
        return;
    }
    int is_quit = !strncasecmp(line, "/quit", 5) || !strncasecmp(line, "/exit", 5) ||
                  !strncasecmp(line, "/bye", 4);
    if (!g_connected) {
        if (is_quit) g_quit = 1;
        else say("\x1b[91m***\x1b[0m Not connected.");
        return;
    }
    if (net_send_line(line, len) != 0) {
        disconnect("send failed");
        return;
    }
    if (is_quit) g_quit = 1;
}

static void handle_key(int k)
{
    int rows = g_h - 2;
    need_input = 1;
    switch (k) {
    case K_IGNORE: break;
    case K_ENTER: submit(); break;
    case K_BS:
        if (incur > 0) ed_delete_range(u8_prev(inbuf, incur), incur);
        break;
    case K_DEL:
        if (incur < inlen) ed_delete_range(incur, u8_next(inbuf, inlen, incur));
        break;
    case K_LEFT:  incur = u8_prev(inbuf, incur); break;
    case K_RIGHT: incur = u8_next(inbuf, inlen, incur); break;
    case K_HOME: case 1:  incur = 0; break;
    case K_END:  case 5:  incur = inlen; break;
    case 11: ed_delete_range(incur, inlen); break;               /* ^K */
    case 21: ed_delete_range(0, incur); break;                   /* ^U */
    case 23: {                                                   /* ^W */
        size_t i = incur;
        while (i > 0 && inbuf[i - 1] == ' ') i--;
        while (i > 0 && inbuf[i - 1] != ' ') i--;
        ed_delete_range(i, incur);
        break;
    }
    case 12: need_full = 1; break;                               /* ^L */
    case 3:  g_quit = 1; break;                                  /* ^C */
    case 4:  if (inlen == 0) g_quit = 1; break;                  /* ^D */
    case K_UP:
        if (hpos > 0) {
            if (hpos == hn) snprintf(hsave, sizeof hsave, "%.*s", (int)inlen, inbuf);
            ed_set(hist[--hpos]);
        }
        break;
    case K_DOWN:
        if (hpos < hn) {
            hpos++;
            ed_set(hpos == hn ? hsave : hist[hpos]);
        }
        break;
    case K_PGUP:
        scroll_off += rows / 2 ? rows / 2 : 1;
        if (scroll_off > max_scroll()) scroll_off = max_scroll();
        need_full = 1;
        break;
    case K_PGDN:
        scroll_off -= rows / 2 ? rows / 2 : 1;
        if (scroll_off < 0) scroll_off = 0;
        need_full = 1;
        break;
    default:
        if (k >= 32 && k < 0x110000) ed_insert(k);
        break;
    }
}

/* ================================================================== */

static volatile sig_atomic_t g_sig;
static void on_signal(int s) { (void)s; g_sig = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
            "BXChat client\n"
            "usage: %s [-p port] [-n nick] [-w password] host\n"
            "  -p port      server port (default %s)\n"
            "  -n nick      nickname (default: your user name)\n"
            "  -w password  server password (or set BXCHAT_PASSWORD)\n",
            prog, BX_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    const char *host = NULL, *port = BX_DEFAULT_PORT;
    const char *nick = getenv("USER");
    const char *password = getenv("BXCHAT_PASSWORD");

    if (!nick) nick = getenv("USERNAME");
    if (!nick || !*nick) nick = "guest";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nick = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) password = argv[++i];
        else if (argv[i][0] != '-' && !host) host = argv[i];
        else { usage(argv[0]); return 1; }
    }
    if (!host) { usage(argv[0]); return 1; }
    if (password && !*password) password = NULL;

    if (sodium_init() < 0) { fprintf(stderr, "libsodium init failed\n"); return 1; }
    if (bx_net_init() != 0) { fprintf(stderr, "network init failed\n"); return 1; }
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGHUP
    signal(SIGHUP, on_signal);
#endif

    fprintf(stderr, "connecting to %s:%s...\n", host, port);
    g_sock = net_connect(host, port);
    if (g_sock == SOCK_BAD) return 1;
    {
        int one = 1;
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    }
    if (do_handshake(password, nick) != 0) return 1;
    g_connected = 1;

    if (term_init() != 0) return 1;
    atexit(term_restore);

    snprintf(g_nick, sizeof g_nick, "%s", nick);
    ob_printf("\x1b[0m\x1b[2J");
    ui_print_line("\x1b[96m BXChat \x1b[0m- encrypted chat");
    say("\x1b[96m***\x1b[0m Connected to %s:%s", host, port);
    say("\x1b[96m***\x1b[0m Server key fingerprint: \x1b[93m%s\x1b[0m%s", g_fp,
        password ? " (password-authenticated)" : "");
    say("\x1b[96m***\x1b[0m PgUp/PgDn scroll, Up/Down history, Ctrl-L redraw, /help for commands");
    on_server_msg(g_first, (size_t)g_first_len);

    time_t last_min = 0;
    while (!g_quit && !g_sig) {
        fd_set rf;
        struct timeval tv = { 0, 250000 };
        int r = 0, maxfd = -1;

        FD_ZERO(&rf);
        if (g_connected) { FD_SET(g_sock, &rf); maxfd = (int)g_sock; }
#ifdef _WIN32
        tv.tv_usec = 30000;                  /* console input can't be select()ed: poll it */
        if (g_connected) r = select(0, &rf, NULL, NULL, &tv);
        else Sleep(30);
        if (r < 0) break;
#else
        FD_SET(0, &rf);
        r = select(maxfd + 1 > 1 ? maxfd + 1 : 1, &rf, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
#endif
        if (r > 0 && g_connected && FD_ISSET(g_sock, &rf))
            net_read();

#ifndef _WIN32
        if (r > 0 && FD_ISSET(0, &rf) && term_fill() != 0)
            break;
#endif
        int k;
        while ((k = term_next_key()) != K_NONE)
            handle_key(k);

        /* housekeeping: resize, clock tick, redraw */
        int w, h;
        term_size(&w, &h);
        if (w != g_w || h != g_h) {
            g_w = w;
            g_h = h;
            if (scroll_off > max_scroll()) scroll_off = max_scroll();
            need_full = 1;
        }
        time_t m = time(NULL) / 60;
        if (m != last_min) { last_min = m; need_status = 1; }

        if (need_full) {
            ui_full();
        } else {
            if (need_status) { ui_status_draw(); need_input = 1; }
            if (need_input) ui_input_draw();
        }
        need_full = need_status = need_input = 0;
        ob_flush();
    }

    if (g_connected) {                       /* tell the server we're leaving */
        net_send_line("/quit", 5);
        sock_close(g_sock);
    }
    return 0;
}
