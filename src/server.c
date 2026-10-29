/*
 * bxserver - BXChat server. Single threaded select() loop, IRC-like channels.
 *
 *   bxserver [-p port] [-b bindaddr] [-w password]
 *   (password may also come from the BXCHAT_PASSWORD environment variable)
 */
#include "common.h"

#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>

#define MAX_CLIENTS   200
#define MAX_NICK      16
#define MAX_CHAN      24
#define MAX_TOPIC     160
#define MAX_CHANNELS  128
#define MAX_TEXT      700
#define OUT_LIMIT     (256 * 1024)
#define HS_TIMEOUT    10
#define DEFAULT_CHAN  "#lobby"

enum { ST_FREE = 0, ST_HANDSHAKE, ST_HELLO, ST_ACTIVE };

typedef struct {
    sock_t   s;
    int      state;
    int      dead;
    time_t   since;
    uint8_t  pk[32], sk[32];
    bx_chan  ch;
    uint8_t  in[8192];
    size_t   inlen;
    uint8_t *out;
    size_t   outlen, outcap;
    char     nick[MAX_NICK + 1];
    char     chan[MAX_CHAN + 1];
    char     addr[64];
    char     reason[80];
} client;

typedef struct {
    int  used;
    char name[MAX_CHAN + 1];
    char topic[MAX_TOPIC + 1];
} channel;

static client  clients[MAX_CLIENTS];
static channel chans[MAX_CHANNELS];

static int     g_has_pw;
static uint8_t g_psk[32];
static uint8_t g_salt[BX_SALT_LEN];
static volatile sig_atomic_t g_stop;

/* ------------------------------------------------------------------ */

static void logf_(const char *fmt, ...) BX_PRINTF(1, 2);
static void logf_(const char *fmt, ...)
{
    char ts[16];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    va_list ap;

    strftime(ts, sizeof ts, "%H:%M:%S", tm);
    printf("[%s] ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* strip control characters (kills terminal escape injection and our \t separator) */
static void sanitize(char *s)
{
    char *w = s;
    for (; *s; s++)
        if ((unsigned char)*s >= 0x20 && (unsigned char)*s != 0x7f)
            *w++ = *s;
    *w = 0;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    char *p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

/* ------------------------------------------------------------------ */
/* output queue                                                        */
/* ------------------------------------------------------------------ */

static void queue_raw(client *c, const uint8_t *d, size_t n)
{
    if (c->dead) return;
    if (c->outlen + n > OUT_LIMIT) {
        snprintf(c->reason, sizeof c->reason, "send queue overflow");
        c->dead = 1;
        return;
    }
    if (c->outlen + n > c->outcap) {
        size_t nc = c->outcap ? c->outcap : 1024;
        while (nc < c->outlen + n) nc *= 2;
        uint8_t *p = (uint8_t *)realloc(c->out, nc);
        if (!p) { c->dead = 1; return; }
        c->out = p;
        c->outcap = nc;
    }
    memcpy(c->out + c->outlen, d, n);
    c->outlen += n;
}

static void send_text(client *c, char type, const char *text)
{
    uint8_t plain[BX_MAX_PLAIN];
    uint8_t frame[BX_MAX_FRAME];
    size_t n = strlen(text);

    if (n > BX_MAX_PLAIN - 1) n = BX_MAX_PLAIN - 1;
    plain[0] = (uint8_t)type;
    memcpy(plain + 1, text, n);
    size_t fl = bx_seal(&c->ch, plain, n + 1, frame);
    queue_raw(c, frame, fl);
}

static void send_msg(client *c, char type, const char *fmt, ...) BX_PRINTF(3, 4);
static void send_msg(client *c, char type, const char *fmt, ...)
{
    char buf[BX_MAX_PLAIN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    send_text(c, type, buf);
}

static void bcast(const char *chan, const client *except, char type, const char *fmt, ...) BX_PRINTF(4, 5);
static void bcast(const char *chan, const client *except, char type, const char *fmt, ...)
{
    char buf[BX_MAX_PLAIN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &clients[i];
        if (c->state == ST_ACTIVE && !c->dead && c != except &&
            c->chan[0] && strcasecmp(c->chan, chan) == 0)
            send_text(c, type, buf);
    }
}

static void flush_client(client *c)
{
    while (c->outlen) {
        int k = (int)send(c->s, (const char *)c->out, (int)c->outlen, BX_SEND_FLAGS);
        if (k < 0) {
            int e = SOCK_ERR();
            if (bx_would_block(e)) return;
            if (e == E_INTR) continue;
            c->dead = 1;
            return;
        }
        if (k == 0) return;
        memmove(c->out, c->out + k, c->outlen - (size_t)k);
        c->outlen -= (size_t)k;
    }
}

/* ------------------------------------------------------------------ */
/* channels / users                                                    */
/* ------------------------------------------------------------------ */

static int valid_nick(const char *n)
{
    size_t l = strlen(n);
    if (l < 1 || l > MAX_NICK) return 0;
    if (isdigit((unsigned char)n[0]) || n[0] == '-') return 0;
    for (; *n; n++)
        if (!(isalnum((unsigned char)*n) || strchr("_-[]\\`^{|}", *n)))
            return 0;
    return 1;
}

static int valid_chan(const char *n)
{
    size_t l = strlen(n);
    if (l < 2 || l > MAX_CHAN || n[0] != '#') return 0;
    for (n++; *n; n++)
        if (!(isalnum((unsigned char)*n) || strchr("_-.", *n)))
            return 0;
    return 1;
}

static client *find_nick(const char *nick)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].state == ST_ACTIVE && !clients[i].dead &&
            strcasecmp(clients[i].nick, nick) == 0)
            return &clients[i];
    return NULL;
}

static channel *chan_get(const char *name, int create)
{
    channel *freeslot = NULL;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (chans[i].used && strcasecmp(chans[i].name, name) == 0)
            return &chans[i];
        if (!chans[i].used && !freeslot)
            freeslot = &chans[i];
    }
    if (create && freeslot) {
        memset(freeslot, 0, sizeof *freeslot);
        freeslot->used = 1;
        snprintf(freeslot->name, sizeof freeslot->name, "%s", name);
        return freeslot;
    }
    return NULL;
}

static int chan_count(const char *name)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].state == ST_ACTIVE && !clients[i].dead &&
            clients[i].chan[0] && strcasecmp(clients[i].chan, name) == 0)
            n++;
    return n;
}

static void chan_gc(const char *name)
{
    channel *ch = chan_get(name, 0);
    if (ch && chan_count(name) == 0)
        ch->used = 0;
}

static void push_status(client *c)
{
    const char *topic = "";
    int cnt = 0;
    if (c->chan[0]) {
        channel *ch = chan_get(c->chan, 0);
        if (ch) topic = ch->topic;
        cnt = chan_count(c->chan);
    }
    send_msg(c, 'S', "%s\t%s\t%d\t%s", c->nick, c->chan, cnt, topic);
}

static void push_status_chan(const char *chan)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].state == ST_ACTIVE && !clients[i].dead &&
            clients[i].chan[0] && strcasecmp(clients[i].chan, chan) == 0)
            push_status(&clients[i]);
}

static void list_users(client *c, const char *chan)
{
    char line[BX_MAX_PLAIN];
    size_t len = 0;
    int cnt = 0;

    line[0] = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *o = &clients[i];
        if (o->state != ST_ACTIVE || o->dead || !o->chan[0] || strcasecmp(o->chan, chan) != 0)
            continue;
        cnt++;
        size_t nl = strlen(o->nick);
        if (len + nl + 2 > 400) {
            send_msg(c, 'I', "%s", line);
            line[0] = 0;
            len = 0;
        }
        len += (size_t)snprintf(line + len, sizeof line - len, "%s ", o->nick);
    }
    if (len) send_msg(c, 'I', "%s", line);
    send_msg(c, 'I', "%d user%s on %s", cnt, cnt == 1 ? "" : "s", chan);
}

static void do_part(client *c, const char *why)
{
    char old[MAX_CHAN + 1];
    if (!c->chan[0]) return;
    snprintf(old, sizeof old, "%s", c->chan);
    c->chan[0] = 0;
    bcast(old, c, 'L', "%s has left %s (%s)", c->nick, old, why);
    chan_gc(old);
    push_status_chan(old);
    push_status(c);
}

static void do_join(client *c, const char *name)
{
    if (!valid_chan(name)) {
        send_msg(c, 'E', "Invalid channel name. Use #name (letters, digits, _ - .), max %d chars.", MAX_CHAN);
        return;
    }
    if (c->chan[0] && strcasecmp(c->chan, name) == 0) {
        send_msg(c, 'E', "You are already on %s", c->chan);
        return;
    }
    channel *ch = chan_get(name, 1);
    if (!ch) {
        send_msg(c, 'E', "Server has too many channels");
        return;
    }
    if (c->chan[0]) do_part(c, "changed channel");
    snprintf(c->chan, sizeof c->chan, "%s", ch->name);
    bcast(c->chan, NULL, 'J', "%s (%s) has joined %s", c->nick, c->addr, c->chan);
    if (ch->topic[0]) send_msg(c, 'I', "Topic for %s: %s", ch->name, ch->topic);
    list_users(c, c->chan);
    push_status_chan(c->chan);
}

static void drop_client(client *c)
{
    if (c->state == ST_ACTIVE && c->chan[0]) {
        char old[MAX_CHAN + 1];
        snprintf(old, sizeof old, "%s", c->chan);
        c->chan[0] = 0;
        bcast(old, c, 'L', "%s has quit (%s)", c->nick, c->reason[0] ? c->reason : "connection closed");
        chan_gc(old);
        push_status_chan(old);
    }
    if (c->state == ST_ACTIVE)
        logf_("%s (%s) disconnected: %s", c->nick, c->addr, c->reason[0] ? c->reason : "connection closed");
    else
        logf_("%s dropped during login: %s", c->addr, c->reason[0] ? c->reason : "handshake failed");
    if (c->s != SOCK_BAD) sock_close(c->s);
    free(c->out);
    sodium_memzero(c, sizeof *c);
    c->s = SOCK_BAD;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_help(client *c)
{
    static const char *h[] = {
        "Commands:",
        "  /join #chan        join (or create) a channel",
        "  /part              leave current channel",
        "  /nick <name>       change nickname",
        "  /msg <nick> <text> private message  (alias /m, /query)",
        "  /me <action>       action message",
        "  /topic [text]      show / set channel topic",
        "  /who               users in this channel      (alias /names)",
        "  /list              all channels",
        "  /whois <nick>      info about a user",
        "  /quit [reason]     disconnect",
        "Client keys: PgUp/PgDn scroll, Up/Down history, Ctrl-L redraw, /clear",
    };
    for (size_t i = 0; i < sizeof h / sizeof *h; i++)
        send_msg(c, 'I', "%s", h[i]);
}

static void handle_command(client *c, char *line)
{
    char *cmd = line + 1;
    char *arg = cmd;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;
    trim(arg);

    if (!strcasecmp(cmd, "help") || !strcasecmp(cmd, "?")) {
        cmd_help(c);
    } else if (!strcasecmp(cmd, "nick")) {
        char *w = arg;
        while (*w && *w != ' ') w++;
        *w = 0;
        if (!valid_nick(arg)) {
            send_msg(c, 'E', "Invalid nick (1-%d chars, letters/digits/_-[]\\`^{|}, not starting with a digit)", MAX_NICK);
            return;
        }
        client *o = find_nick(arg);
        if (o && o != c) {
            send_msg(c, 'E', "Nick %s is already in use", arg);
            return;
        }
        char old[MAX_NICK + 1];
        snprintf(old, sizeof old, "%s", c->nick);
        snprintf(c->nick, sizeof c->nick, "%s", arg);
        if (c->chan[0])
            bcast(c->chan, c, 'I', "%s is now known as %s", old, c->nick);
        send_msg(c, 'I', "You are now known as %s", c->nick);
        logf_("%s is now %s", old, c->nick);
        if (c->chan[0]) push_status_chan(c->chan); else push_status(c);
    } else if (!strcasecmp(cmd, "join") || !strcasecmp(cmd, "j")) {
        char *w = arg;
        while (*w && *w != ' ') w++;
        *w = 0;
        if (!*arg) { send_msg(c, 'E', "Usage: /join #channel"); return; }
        if (arg[0] != '#') {            /* be forgiving: /join foo -> #foo */
            char tmp[MAX_CHAN + 2];
            snprintf(tmp, sizeof tmp, "#%s", arg);
            do_join(c, tmp);
        } else {
            do_join(c, arg);
        }
    } else if (!strcasecmp(cmd, "part") || !strcasecmp(cmd, "leave")) {
        if (!c->chan[0]) { send_msg(c, 'E', "You are not in a channel"); return; }
        do_part(c, *arg ? arg : "leaving");
    } else if (!strcasecmp(cmd, "msg") || !strcasecmp(cmd, "m") || !strcasecmp(cmd, "query")) {
        char *text = arg;
        while (*text && *text != ' ') text++;
        if (*text) *text++ = 0;
        trim(text);
        if (!*arg || !*text) { send_msg(c, 'E', "Usage: /msg <nick> <text>"); return; }
        client *o = find_nick(arg);
        if (!o) { send_msg(c, 'E', "No such nick: %s", arg); return; }
        send_msg(o, 'P', "%s\t%s", c->nick, text);
        send_msg(c, 'Q', "%s\t%s", o->nick, text);
    } else if (!strcasecmp(cmd, "me")) {
        if (!c->chan[0]) { send_msg(c, 'E', "You are not in a channel"); return; }
        if (!*arg) { send_msg(c, 'E', "Usage: /me <action>"); return; }
        bcast(c->chan, NULL, 'A', "%s\t%s", c->nick, arg);
    } else if (!strcasecmp(cmd, "topic")) {
        if (!c->chan[0]) { send_msg(c, 'E', "You are not in a channel"); return; }
        channel *ch = chan_get(c->chan, 0);
        if (!ch) return;
        if (!*arg) {
            send_msg(c, 'I', "Topic for %s: %s", ch->name, ch->topic[0] ? ch->topic : "(none)");
        } else {
            snprintf(ch->topic, sizeof ch->topic, "%s", arg);
            bcast(c->chan, NULL, 'I', "%s changed the topic to: %s", c->nick, ch->topic);
            push_status_chan(c->chan);
        }
    } else if (!strcasecmp(cmd, "who") || !strcasecmp(cmd, "names")) {
        if (!c->chan[0]) { send_msg(c, 'E', "You are not in a channel"); return; }
        list_users(c, c->chan);
    } else if (!strcasecmp(cmd, "list")) {
        int n = 0;
        for (int i = 0; i < MAX_CHANNELS; i++) {
            if (!chans[i].used) continue;
            n++;
            send_msg(c, 'I', "%-*s %3d  %s", MAX_CHAN, chans[i].name, chan_count(chans[i].name), chans[i].topic);
        }
        send_msg(c, 'I', "%d channel%s", n, n == 1 ? "" : "s");
    } else if (!strcasecmp(cmd, "whois")) {
        client *o = find_nick(arg);
        if (!o) { send_msg(c, 'E', "No such nick: %s", arg); return; }
        send_msg(c, 'I', "%s is %s on %s, idle-connected for %ds", o->nick, o->addr,
                 o->chan[0] ? o->chan : "(no channel)", (int)(time(NULL) - o->since));
    } else if (!strcasecmp(cmd, "quit") || !strcasecmp(cmd, "exit") || !strcasecmp(cmd, "bye")) {
        snprintf(c->reason, sizeof c->reason, "%.70s", *arg ? arg : "quit");
        c->dead = 1;
    } else {
        send_msg(c, 'E', "Unknown command /%s  (try /help)", cmd);
    }
}

static void handle_plain(client *c, char *line)
{
    sanitize(line);
    trim(line);
    if (strlen(line) > MAX_TEXT) line[MAX_TEXT] = 0;

    if (c->state == ST_HELLO) {
        char base[MAX_NICK + 1] = "guest";
        char cand[MAX_NICK + 8];
        if (strncmp(line, "HELLO ", 6) == 0) {
            char *n = line + 6;
            char *w = n;
            while (*w && *w != ' ') w++;
            *w = 0;
            if (valid_nick(n)) snprintf(base, sizeof base, "%s", n);
        } else {
            snprintf(c->reason, sizeof c->reason, "protocol error");
            c->dead = 1;
            return;
        }
        snprintf(cand, sizeof cand, "%s", base);
        for (int i = 1; find_nick(cand) && i < 1000; i++)
            snprintf(cand, sizeof cand, "%.12s%d", base, i);
        snprintf(c->nick, sizeof c->nick, "%s", cand);
        c->state = ST_ACTIVE;
        logf_("%s joined as %s", c->addr, c->nick);

        send_msg(c, 'I', "Welcome to BXChat, %s", c->nick);
        send_msg(c, 'I', "Connection encrypted (X25519 + XSalsa20-Poly1305)%s",
                 g_has_pw ? ", password authenticated" : "; server has no password");
        send_msg(c, 'I', "Type /help for commands");
        do_join(c, DEFAULT_CHAN);
        return;
    }

    if (!*line) return;
    if (line[0] == '/')
        handle_command(c, line);
    else if (!c->chan[0])
        send_msg(c, 'E', "You are not in a channel. Use /join #channel");
    else
        bcast(c->chan, NULL, 'M', "%s\t%s", c->nick, line);
}

/* ------------------------------------------------------------------ */
/* network I/O                                                         */
/* ------------------------------------------------------------------ */

static void consume(client *c, size_t n)
{
    memmove(c->in, c->in + n, c->inlen - n);
    c->inlen -= n;
}

static void process_input(client *c)
{
    for (;;) {
        if (c->state == ST_HANDSHAKE) {
            if (c->inlen < BX_HS_CLIENT_LEN) return;
            uint8_t cpk[BX_PK_LEN];
            memcpy(cpk, c->in, BX_PK_LEN);
            consume(c, BX_HS_CLIENT_LEN);
            if (bx_chan_setup(&c->ch, 1, c->pk, c->sk, cpk, g_has_pw ? g_psk : NULL) != 0) {
                snprintf(c->reason, sizeof c->reason, "bad key exchange");
                c->dead = 1;
                return;
            }
            sodium_memzero(c->sk, sizeof c->sk);
            c->state = ST_HELLO;
            continue;
        }
        if (c->inlen < 2) return;
        size_t clen = ((size_t)c->in[0] << 8) | c->in[1];
        if (clen < crypto_secretbox_MACBYTES + 1 || clen > BX_MAX_CIPHER) {
            snprintf(c->reason, sizeof c->reason, "bad frame length");
            c->dead = 1;
            return;
        }
        if (c->inlen < 2 + clen) return;

        uint8_t plain[BX_MAX_PLAIN + 1];
        int n = bx_open(&c->ch, c->in + 2, clen, plain);
        if (n < 0) {
            snprintf(c->reason, sizeof c->reason,
                     c->state == ST_HELLO ? "authentication failed (wrong password?)"
                                          : "decryption failed");
            c->dead = 1;
            return;
        }
        consume(c, 2 + clen);
        plain[n] = 0;
        handle_plain(c, (char *)plain);
        if (c->dead) return;
    }
}

static void do_accept(sock_t ls)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    sock_t ns = accept(ls, (struct sockaddr *)&ss, &sl);
    if (ns == SOCK_BAD) return;

    client *c = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].state == ST_FREE) { c = &clients[i]; break; }
#ifndef _WIN32
    if (ns >= FD_SETSIZE) c = NULL;
#endif
    if (!c) {
        sock_close(ns);
        logf_("rejected connection: server full");
        return;
    }
    memset(c, 0, sizeof *c);
    c->s = ns;
    c->state = ST_HANDSHAKE;
    c->since = time(NULL);
    bx_set_nonblock(ns);
    {
        int one = 1;
        setsockopt(ns, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    }
    if (getnameinfo((struct sockaddr *)&ss, sl, c->addr, sizeof c->addr, NULL, 0, NI_NUMERICHOST) != 0)
        snprintf(c->addr, sizeof c->addr, "?");
    if (strncmp(c->addr, "::ffff:", 7) == 0)
        memmove(c->addr, c->addr + 7, strlen(c->addr + 7) + 1);

    crypto_kx_keypair(c->pk, c->sk);

    uint8_t hs[BX_HS_SERVER_LEN];
    memcpy(hs, BX_MAGIC, 4);
    hs[4] = g_has_pw ? BX_FLAG_PASSWORD : 0;
    memcpy(hs + 5, g_salt, BX_SALT_LEN);
    memcpy(hs + 5 + BX_SALT_LEN, c->pk, BX_PK_LEN);
    queue_raw(c, hs, sizeof hs);
    logf_("connection from %s", c->addr);
}

static sock_t make_listener(const char *bindaddr, const char *port)
{
    struct addrinfo hints, *res = NULL, *rp;
    sock_t s = SOCK_BAD;

    for (int pass = 0; pass < 2 && s == SOCK_BAD; pass++) {
        memset(&hints, 0, sizeof hints);
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        /* no bind address: try dual-stack IPv6 first, then plain IPv4 */
        hints.ai_family = bindaddr ? AF_UNSPEC : (pass == 0 ? AF_INET6 : AF_INET);
        if (getaddrinfo(bindaddr, port, &hints, &res) != 0)
            continue;
        for (rp = res; rp; rp = rp->ai_next) {
            s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (s == SOCK_BAD) continue;
#ifndef _WIN32
            { int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one); }
#endif
#ifdef IPV6_V6ONLY
            if (rp->ai_family == AF_INET6) {
                int zero = 0;
                setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
            }
#endif
            if (bind(s, rp->ai_addr, (int)rp->ai_addrlen) == 0 && listen(s, 32) == 0)
                break;
            sock_close(s);
            s = SOCK_BAD;
        }
        freeaddrinfo(res);
        if (bindaddr) break;
    }
    return s;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "BXChat server\n"
            "usage: %s [-p port] [-b bindaddr] [-w password]\n"
            "  -p port      TCP port (default %s)\n"
            "  -b addr      bind address (default: all interfaces)\n"
            "  -w password  require this password (or set BXCHAT_PASSWORD)\n",
            prog, BX_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    const char *port = BX_DEFAULT_PORT, *bindaddr = NULL, *password = getenv("BXCHAT_PASSWORD");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) bindaddr = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) password = argv[++i];
        else { usage(argv[0]); return 1; }
    }
    if (password && !*password) password = NULL;

    if (sodium_init() < 0) { fprintf(stderr, "libsodium init failed\n"); return 1; }
    if (bx_net_init() != 0) { fprintf(stderr, "network init failed\n"); return 1; }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].s = SOCK_BAD;

    if (password) {
        randombytes_buf(g_salt, sizeof g_salt);
        fprintf(stderr, "deriving key from password... ");
        if (bx_derive_psk(g_psk, password, g_salt) != 0) {
            fprintf(stderr, "failed (out of memory?)\n");
            return 1;
        }
        fprintf(stderr, "ok\n");
        g_has_pw = 1;
    }

    sock_t ls = make_listener(bindaddr, port);
    if (ls == SOCK_BAD) {
        fprintf(stderr, "cannot listen on port %s\n", port);
        return 1;
    }
    bx_set_nonblock(ls);
    logf_("BXChat server listening on port %s (%s)", port,
          g_has_pw ? "password protected" : "NO password");

    while (!g_stop) {
        fd_set rf, wf;
        int maxfd = (int)ls;
        struct timeval tv = { 1, 0 };

        FD_ZERO(&rf);
        FD_ZERO(&wf);
        FD_SET(ls, &rf);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client *c = &clients[i];
            if (c->state == ST_FREE) continue;
            FD_SET(c->s, &rf);
            if (c->outlen) FD_SET(c->s, &wf);
            if ((int)c->s > maxfd) maxfd = (int)c->s;
        }

        int r = select(maxfd + 1, &rf, &wf, NULL, &tv);
        if (r < 0) {
            if (SOCK_ERR() == E_INTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(ls, &rf)) do_accept(ls);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            client *c = &clients[i];
            if (c->state == ST_FREE) continue;
            if (FD_ISSET(c->s, &rf) && !c->dead) {
                int k = (int)recv(c->s, (char *)c->in + c->inlen, (int)(sizeof c->in - c->inlen), 0);
                if (k == 0) {
                    c->dead = 1;
                } else if (k < 0) {
                    int e = SOCK_ERR();
                    if (!bx_would_block(e) && e != E_INTR) c->dead = 1;
                } else {
                    c->inlen += (size_t)k;
                    process_input(c);
                }
            }
        }

        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client *c = &clients[i];
            if (c->state == ST_FREE) continue;
            if (c->state != ST_ACTIVE && now - c->since > HS_TIMEOUT) {
                snprintf(c->reason, sizeof c->reason, "login timeout");
                c->dead = 1;
            }
            if (c->outlen) flush_client(c);
            if (c->dead) {
                flush_client(c);            /* best effort: deliver final messages */
                drop_client(c);
            }
        }
    }

    logf_("shutting down");
    return 0;
}
