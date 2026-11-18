/*
 * tiny ept - bootstrap for the ELKS Package Tool
 *
 * Copyright (c) 2026 Sam Warr
 * SPDX-License-Identifier: MIT
 *
 * Meant to be shipped as /bin/ept on ELKS floppy images, where the full ept
 * is too big. Whatever it is asked to do, it replaces itself with the full
 * ept from the first repository in /etc/ept.cfg, then runs that with the
 * same arguments. It understands nothing of the repository except:
 *
 *   index line:  name version md5 size mode dest   (at <repo>/index)
 *   file URL:    <repo>/pool/<name><dest>
 *   installed:   name version md5 dest channel     (in /etc/ept/inst)
 *
 * Written for size: no stdio, no malloc, and a looped MD5 that is slow
 * but a fraction of the size of the usual unrolled one.
 */
#include <stdint.h>
#include <stdio.h>             /* rename() only; no stdio is used */
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEST        "/bin/ept"     /* where ept lives, as the repository names it */
#ifndef SELF                    /* overridable for testing on a host */
#define SELF        DEST
#define TMP         DEST ".tmp"
#define CONFIG      "/etc/ept.cfg"
#define DIR         "/etc/ept"
#endif
#define DEFREPO     "http://ept.samwarr.dev"
#define DB          DIR "/inst"
#define STALL       10          /* seconds without data before a try is given up */
#define TRIES       3

extern char **environ;

static char buf[1024];
static char auth[64];           /* host[:port], for the Host header */
static char host[64];
static char base[96];           /* repository path, no trailing slash */
static unsigned short port;
static char line[160];          /* longer lines are cut, they are not ept's */
static int llen;
static void (*online)(char *);
static char ver[24], sum[33];   /* of the ept package, from the index */
static unsigned long size;
static char *env[24];

/* MD5 (RFC 1321), one step per loop pass. x86 is little-endian, so the
 * block and the digest are used in place without byte swapping. */
static uint32_t st[4];
static union { unsigned char c[64]; uint32_t w[16]; } mb;
static unsigned long mlen;

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const unsigned char S[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void md5_block(void)
{
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], f;
    int i, g, s;

    for (i = 0; i < 64; i++) {
        switch (i >> 4) {
        case 0:  f = (b & c) | (~b & d); g = i;              break;
        case 1:  f = (d & b) | (~d & c); g = (5 * i + 1) & 15; break;
        case 2:  f = b ^ c ^ d;          g = (3 * i + 5) & 15; break;
        default: f = c ^ (b | ~d);       g = (7 * i) & 15;     break;
        }
        f += a + K[i] + mb.w[g];
        s = S[(i >> 4) << 2 | (i & 3)];
        a = d;
        d = c;
        c = b;
        b += f << s | f >> (32 - s);
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

static void md5_init(void)
{
    st[0] = 0x67452301;
    st[1] = 0xefcdab89;
    st[2] = 0x98badcfe;
    st[3] = 0x10325476;
    mlen = 0;
}

static void md5_add(const char *p, int n)
{
    while (n--) {
        mb.c[(unsigned)mlen & 63] = *p++;
        if (!((unsigned)++mlen & 63))
            md5_block();
    }
}

/* finish, then compare with the hex digest in sum: 0 if they match */
static int md5_check(void)
{
    static const char hex[] = "0123456789abcdef";
    unsigned long n = mlen;
    unsigned char *d = (unsigned char *)st;
    int i;

    md5_add("\200", 1);
    while (((unsigned)mlen & 63) != 56)
        md5_add("", 1);
    mb.w[14] = n << 3;
    mb.w[15] = n >> 29;
    md5_block();
    for (i = 0; i < 16; i++)
        if (sum[2 * i] != hex[d[i] >> 4] || sum[2 * i + 1] != hex[d[i] & 15])
            return 1;
    return 0;
}

static void say(const char *s)
{
    write(STDERR_FILENO, s, strlen(s));
}

static void die(const char *s)
{
    say("ept: ");
    say(s);
    say("\n");
    exit(1);
}

static unsigned long num(const char *s)
{
    unsigned long n = 0;

    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

/* split a URL like http://host[:port][/path] into auth, host, port and base */
static void seturl(char *url)
{
    char *p, *e;

    if (strncmp(url, "http://", 7))
        die("repository must be an http:// URL");
    url += 7;
    for (p = url; *p && *p != '/'; p++)
        ;
    if (p == url || p - url >= sizeof(auth) || strlen(p) >= sizeof(base))
        die("bad repository URL");
    memcpy(auth, url, p - url);
    auth[p - url] = '\0';
    strcpy(base, p);
    for (e = base + strlen(base); e > base && e[-1] == '/'; )
        *--e = '\0';
    strcpy(host, auth);
    port = 80;
    if ((e = strchr(host, ':')) != NULL) {
        *e = '\0';
        port = num(e + 1);
    }
}

/* hand complete lines of the stream to online(), without \r or trailing blanks */
static void feed(const char *p, int n)
{
    char c;

    while (n--) {
        if ((c = *p++) == '\n') {
            while (llen && (line[llen - 1] == '\r' || line[llen - 1] == ' '))
                llen--;
            line[llen] = '\0';
            online(line);
            llen = 0;
        } else if (llen < sizeof(line) - 1)
            line[llen++] = c;
    }
}

static void cfgline(char *s)
{
    if (!host[0] && !strncmp(s, "REPO=", 5))
        seturl(s + 5);
}

static void idxline(char *s)
{
    char *f[6];
    int n = 0;

    while (n < 6) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        f[n++] = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        if (*s)
            *s++ = '\0';
    }
    if (n == 6 && !strcmp(f[0], "ept") && !strcmp(f[5], DEST)
            && strlen(f[1]) < sizeof(ver) && strlen(f[2]) == 32) {
        strcpy(ver, f[1]);
        strcpy(sum, f[2]);
        size = num(f[3]);
    }
}

/*
 * GET <base><path>. The body goes to fd (and into the md5) if fd >= 0,
 * else line by line to online(). 0 on a complete 200 reply.
 */
static int get(const char *path, int fd)
{
    int s, n, i, hdr = 1, nl = 0, slen = 0, rc = -1;
    unsigned long total = 0;
    char status[12];
    struct sockaddr_in a;
    fd_set r;
    struct timeval tv;

    llen = 0;                   /* drop a line cut short by an earlier try */
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    a.sin_family = AF_INET;
    a.sin_port = PORT_ANY;
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
        goto out;
    a.sin_port = htons(port);
    if (!(a.sin_addr.s_addr = in_gethostbyname(host))
            || in_connect(s, (struct sockaddr *)&a, sizeof(a), 10) < 0) {
        say("ept: cannot connect (is the network up?)\n");
        goto out;
    }
    /* fits: base 96 + path 20 + auth 64 + 70 < sizeof(buf) */
    strcpy(buf, "GET ");
    strcat(buf, base);
    strcat(buf, path);
    strcat(buf, " HTTP/1.0\r\nHost: ");
    strcat(buf, auth);
    strcat(buf, "\r\nUser-Agent: ept-tiny\r\nConnection: close\r\n\r\n");
    n = strlen(buf);
    if (write(s, buf, n) != n)
        goto out;
    for (;;) {
        FD_ZERO(&r);
        FD_SET(s, &r);
        tv.tv_sec = STALL;
        tv.tv_usec = 0;
        if (select(s + 1, &r, NULL, NULL, &tv) <= 0 || (n = read(s, buf, sizeof(buf))) < 0) {
            say("\nept: connection stalled\n");
            goto out;
        }
        if (n == 0)
            break;
        for (i = 0; i < n && hdr; i++) {
            if (slen < sizeof(status))
                status[slen++] = buf[i];
            if (buf[i] == '\n') {
                if (nl)
                    hdr = 0;            /* blank line ends the header */
                nl = 1;
            } else if (buf[i] != '\r')
                nl = 0;
        }
        if (hdr)
            continue;
        if (slen < sizeof(status) || strncmp(status, "HTTP/1.", 7) || strncmp(status + 9, "200", 3)) {
            say("ept: server error for ");
            say(path);
            say("\n");
            goto out;
        }
        n -= i;
        if (fd < 0)
            feed(buf + i, n);
        else {
            if (write(fd, buf + i, n) != n) {
                say("ept: write error\n");
                goto out;
            }
            md5_add(buf + i, n);
            if ((total ^ (total + n)) >> 12)
                say(".");               /* a dot per 4K: progress on slow links */
        }
        total += n;
    }
    if (hdr || (fd >= 0 && total != size))
        say("\nept: connection lost\n");
    else
        rc = 0;
out:
    close(s);
    return rc;
}

/* rename, or copy where that fails (FAT); the old file must already be gone */
static int put(const char *from, const char *to)
{
    int in, out, n;

    if (rename(from, to) == 0)
        return 0;
    if ((in = open(from, O_RDONLY)) < 0)
        return -1;
    if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755)) < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0 && write(out, buf, n) == n)
        ;
    close(in);
    if (close(out) < 0 || n != 0)
        return -1;
    unlink(from);
    return 0;
}

int main(int argc, char **argv)
{
    int fd, n, try;

    (void)argc;
    if (getenv("EPTSELF"))      /* the repository gave us tiny ept again */
        die("the downloaded ept is not the full one");

    online = cfgline;
    if ((fd = open(CONFIG, O_RDONLY)) >= 0) {
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            feed(buf, n);
        feed("\n", 1);
        close(fd);
    }
    if (!host[0])
        seturl(strcpy(line, DEFREPO));

    say("ept: getting the full ept from http://");
    say(auth);
    say(base);
    say("\n");
    online = idxline;
    for (try = 1; get("/index", -1) != 0; try++)
        if (try == TRIES)
            die("cannot fetch the package list");
    if (!sum[0])
        die("the package list has no ept");

    for (try = 1; ; try++) {
        if ((fd = open(TMP, O_WRONLY | O_CREAT | O_TRUNC, 0755)) < 0)
            die("cannot write " TMP);
        md5_init();
        n = get("/pool/ept" DEST, fd);
        if (close(fd) < 0)
            n = -1;
        if (n == 0) {
            if (md5_check() == 0)
                break;
            say("ept: checksum mismatch\n");
        }
        if (try == TRIES) {
            unlink(TMP);
            die("cannot download ept");
        }
    }
    say("\n");

    /* a running program can be unlinked but not overwritten */
    unlink(SELF);
    if (put(TMP, SELF) != 0)
        die("cannot install " SELF ", the new one is in " TMP);
    chmod(SELF, 0755);

    /* on a fresh system, record ept as installed; never touch an existing list */
    mkdir(DIR, 0755);
    if ((fd = open(DB, O_WRONLY | O_CREAT | O_EXCL, 0644)) >= 0) {
        strcpy(line, "ept ");
        strcat(line, ver);
        strcat(line, " ");
        strcat(line, sum);
        strcat(line, " " DEST " s\n");
        write(fd, line, strlen(line));
        close(fd);
    }

    say("ept: installed ept ");
    say(ver);
    say("\n");

    /* run it with our environment plus EPTSELF, which also tells the full
     * ept that it is already up to date */
    for (n = 0; environ[n] && n < 22; n++)
        env[n] = environ[n];
    env[n] = "EPTSELF=1";
    execve(SELF, argv, env);
    die("cannot run " SELF);
    return 1;
}
