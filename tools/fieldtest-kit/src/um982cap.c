/* um982cap.c -- Unicore UM982 (and any NMEA receiver) serial capture for the
 * LidarScan field-test kit.
 *
 * Single-file, dependency-free C99. The macOS kit ships the compiled binary so
 * the runner needs nothing installed -- no Python, no pyserial, no Homebrew.
 *
 * Build:  cc -O2 -std=c99 -Wall -Wextra -o um982cap um982cap.c
 *
 * Usage:
 *   um982cap --list
 *   um982cap [--port /dev/cu.usbserial-XXX] [--baud 115200] [--seconds 90]
 *            [--out FILE] [--probe-seconds 2.5]
 *
 * With no --port it PROBES: every /dev/cu.* candidate at every candidate baud
 * rate, keeping whichever combination actually emits NMEA. That is deliberate
 * -- a UM982 eval board and a COIN-D6 lidar can both be CH340s, so a name
 * match proves nothing, but "does it emit $GxGGA" proves everything. A port
 * that answers with 0xAA 0x55 binary framing is the lidar and is skipped.
 *
 * Baud: 115200 is the UM982's documented default; 460800 is the common
 * alternative on eval boards. Both are tried, plus a few long shots.
 *
 * BENCH SEMANTICS: with no correction source (no NTRIP, no base station), the
 * best fix a UM982 can reach is SINGLE -- GGA quality 1. That is a PASS here.
 * RTK Fixed/Float is a later test.
 *
 * Everything received is written to --out verbatim, including Unicore's
 * proprietary '#UNIHEADINGA,...*<8 hex CRC32>' logs. Those carry a CRC32, not
 * the NMEA 2-hex XOR, so they are counted separately and never scored as
 * corrupt.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_CAND 64
#define LINEBUF 4096
#define READBUF 8192

static const int kBauds[] = {115200, 460800, 9600, 38400, 230400};
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

/* A UM982 at 1 Hz emits GGA/RMC/GSA/GST/VTG plus proprietary logs, so a
 * healthy stream is comfortably above this. */
#define HEALTHY_SENTENCES_PER_SEC 3.0
#define HEALTHY_CHECKSUM_PCT 99.0

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_monotonic(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* ------------------------------------------------------------------ ports */

typedef struct { char path[256]; } Cand;

static int is_candidate(const char *name) {
    /* macOS callout devices. cu.* rather than tty.* -- cu does not block
     * waiting for carrier detect. */
    static const char *pfx[] = {
        "cu.usbserial", "cu.wchusbserial", "cu.SLAB_USBtoUART", "cu.usbmodem",
        "cu.UC",        "cu.CP210",        "cu.usbserial-",     NULL};
    for (int i = 0; pfx[i]; i++)
        if (!strncmp(name, pfx[i], strlen(pfx[i]))) return 1;
    return 0;
}

static int list_candidates(Cand *out, int max) {
    DIR *d = opendir("/dev");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!is_candidate(e->d_name)) continue;
        snprintf(out[n].path, sizeof(out[n].path), "/dev/%s", e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

static int open_serial(const char *path, int baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { close(fd); return -1; }
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= (tcflag_t)~CSTOPB;   /* 1 stop bit */
    tio.c_cflag &= (tcflag_t)~PARENB;   /* no parity  */
    tio.c_cflag &= (tcflag_t)~CSIZE;
    tio.c_cflag |= CS8;                 /* 8 data bits */
    tio.c_cflag &= (tcflag_t)~CRTSCTS;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (cfsetspeed(&tio, (speed_t)baud) != 0) { close(fd); return -1; }
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* --------------------------------------------------------------- NMEA math */

/* NMEA 0183: '$' body '*' 2-hex XOR. Returns 1 ok, 0 bad, -1 not checkable. */
static int nmea_checksum_ok(const char *s, size_t len) {
    if (len < 4 || (s[0] != '$' && s[0] != '!')) return -1;
    long star = -1;
    for (long i = (long)len - 1; i >= 0; i--) {
        if (s[i] == '*') { star = i; break; }
    }
    if (star < 1 || (size_t)(star + 3) > len) return -1;
    int claimed = 0;
    for (int k = 1; k <= 2; k++) {
        char c = s[star + k];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        claimed = claimed * 16 + v;
    }
    int computed = 0;
    for (long i = 1; i < star; i++) computed ^= (unsigned char)s[i];
    return computed == claimed ? 1 : 0;
}

typedef struct {
    long sentences, cs_ok, cs_bad, unicore, heading;
    long fix_hist[10];      /* GGA quality digit 0..9 */
    int  best_fix;          /* -1 = no GGA seen yet */
    int  last_fix;
    int  max_sats;
    int  last_sats;
} Stats;

static void stats_init(Stats *s) {
    memset(s, 0, sizeof(*s));
    s->best_fix = -1;
    s->last_fix = -1;
    s->last_sats = -1;
}

static const char *fix_name(int q) {
    switch (q) {
        case -1: return "none reported";
        case 0:  return "NO FIX (needs sky view)";
        case 1:  return "SINGLE (normal GPS)";
        case 2:  return "DGPS";
        case 3:  return "PPS";
        case 4:  return "RTK FIXED (centimetre)";
        case 5:  return "RTK FLOAT (decimetre)";
        case 6:  return "dead reckoning";
        case 7:  return "manual";
        case 8:  return "simulated";
        default: return "unknown";
    }
}

/* Splits a body on commas and returns field n, or NULL. Writes into dst. */
static int nmea_field(const char *body, size_t len, int n, char *dst, size_t dstsz) {
    int field = 0;
    size_t start = 0, i = 0;
    for (i = 0; i <= len; i++) {
        if (i == len || body[i] == ',') {
            if (field == n) {
                size_t l = i - start;
                if (l >= dstsz) l = dstsz - 1;
                memcpy(dst, body + start, l);
                dst[l] = 0;
                return 1;
            }
            field++;
            start = i + 1;
        }
    }
    return 0;
}

static void stats_add_line(Stats *st, const char *line, size_t len) {
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                       line[len - 1] == ' ')) len--;
    while (len > 0 && (*line == ' ' || *line == '\t')) { line++; len--; }
    if (len < 3) return;

    if (line[0] == '#') {
        /* Unicore proprietary ASCII log: 8-hex CRC32 tail, not an NMEA XOR. */
        st->unicore++;
        if (memmem(line, len, "HEADING", 7) || memmem(line, len, "HDT", 3))
            st->heading++;
        return;
    }
    if (line[0] != '$') return;

    st->sentences++;
    int ck = nmea_checksum_ok(line, len);
    if (ck == 1) st->cs_ok++;
    else if (ck == 0) st->cs_bad++;

    size_t body_len = len;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '*') { body_len = i; break; }
    }

    char sid[16];
    if (!nmea_field(line, body_len, 0, sid, sizeof(sid))) return;
    /* sid still carries the leading '$' */
    const char *id = sid + 1;
    size_t idl = strlen(id);
    if (idl >= 3) {
        const char *tail = id + idl - 3;
        if (!strcmp(tail, "HDT") || !strcmp(tail, "HDG") ||
            !strcmp(tail, "HPR") || !strcmp(tail, "THS"))
            st->heading++;
        if (!strcmp(tail, "GGA")) {
            char q[16], sats[16];
            if (nmea_field(line, body_len, 6, q, sizeof(q))) {
                int qi = (q[0] >= '0' && q[0] <= '9') ? q[0] - '0' : 0;
                st->fix_hist[qi]++;
                st->last_fix = qi;
                if (qi > st->best_fix) st->best_fix = qi;
            }
            if (nmea_field(line, body_len, 7, sats, sizeof(sats))) {
                int si = atoi(sats);
                st->last_sats = si;
                if (si > st->max_sats) st->max_sats = si;
            }
        }
    }
}

/* ------------------------------------------------------------------ probe */

/* Reads up to ms milliseconds from fd, scoring the sample. */
static void score_sample(int fd, double secs, int *n_nmea, int *n_uni, int *n_d6,
                         long *n_bytes) {
    unsigned char buf[READBUF];
    double t0 = now_monotonic();
    *n_nmea = *n_uni = *n_d6 = 0;
    *n_bytes = 0;
    unsigned char prev = 0;
    int have_prev = 0;
    while (now_monotonic() - t0 < secs) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        struct timeval tv = {0, 100000};
        if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) continue;
        *n_bytes += n;
        for (ssize_t i = 0; i < n; i++) {
            if (have_prev && prev == 0xAA && buf[i] == 0x55) (*n_d6)++;
            prev = buf[i];
            have_prev = 1;
            if (buf[i] == '$') (*n_nmea)++;
            else if (buf[i] == '#') (*n_uni)++;
        }
    }
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *port = NULL;
    const char *out_path = "um982.nmea";
    int baud = 115200;
    double seconds = 90.0;
    double probe_secs = 2.5;
    int do_list = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--port") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(a, "--baud") && i + 1 < argc) baud = atoi(argv[++i]);
        else if (!strcmp(a, "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(a, "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(a, "--probe-seconds") && i + 1 < argc) probe_secs = atof(argv[++i]);
        else if (!strcmp(a, "--list")) do_list = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("usage: um982cap [--list] [--port DEV] [--baud N] [--seconds N] [--out FILE]\n");
            return 0;
        } else { fprintf(stderr, "unknown option: %s\n", a); return 2; }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Cand cands[MAX_CAND];
    int n_cand = list_candidates(cands, MAX_CAND);

    if (do_list) {
        if (n_cand == 0) printf("no USB serial devices found in /dev\n");
        for (int i = 0; i < n_cand; i++) printf("%s\n", cands[i].path);
        return n_cand > 0 ? 0 : 1;
    }

    char chosen[256] = {0};
    int chosen_baud = baud;

    if (port) {
        snprintf(chosen, sizeof(chosen), "%s", port);
    } else {
        if (n_cand == 0) {
            printf("KEY error no_serial_devices\n");
            printf("VERDICT FAIL\n");
            return 1;
        }
        printf("  Found %d device(s). Checking each one:\n", n_cand);
        int bauds[8];
        int nb = 0;
        bauds[nb++] = baud;
        for (int i = 0; i < kNumBauds && nb < 8; i++)
            if (kBauds[i] != baud) bauds[nb++] = kBauds[i];

        for (int c = 0; c < n_cand && !chosen[0]; c++) {
            for (int b = 0; b < nb; b++) {
                printf("    %s at %d ... ", cands[c].path, bauds[b]);
                fflush(stdout);
                int fd = open_serial(cands[c].path, bauds[b]);
                if (fd < 0) {
                    printf("cannot open (%s)\n", strerror(errno));
                    break;
                }
                int nn, nu, nd;
                long nbytes;
                score_sample(fd, probe_secs, &nn, &nu, &nd, &nbytes);
                close(fd);
                if (nn >= 2 || nu >= 2) {
                    printf("THIS IS THE GPS\n");
                    snprintf(chosen, sizeof(chosen), "%s", cands[c].path);
                    chosen_baud = bauds[b];
                    break;
                }
                if (nd >= 5) { printf("this is the spinning lidar, skipping\n"); break; }
                if (nbytes == 0) printf("silent\n");
                else printf("noise (%ld bytes)\n", nbytes);
            }
        }
        if (!chosen[0]) {
            printf("KEY error no_gps_found\n");
            printf("VERDICT FAIL\n");
            return 1;
        }
    }

    int fd = open_serial(chosen, chosen_baud);
    if (fd < 0) {
        printf("KEY error cannot_open %s (%s)\n", chosen, strerror(errno));
        printf("VERDICT FAIL\n");
        return 1;
    }
    printf("\n  Using %s at %d baud 8N1\n\n", chosen, chosen_baud);

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        printf("KEY error cannot_write %s\n", out_path);
        printf("VERDICT FAIL\n");
        close(fd);
        return 1;
    }

    Stats st;
    stats_init(&st);
    char line[LINEBUF];
    size_t line_len = 0;
    unsigned char buf[READBUF];
    long total_bytes = 0;
    double t0 = now_monotonic();
    int last_shown = -1;

    while (!g_stop) {
        double elapsed = now_monotonic() - t0;
        if (elapsed >= seconds) break;

        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        struct timeval tv = {0, 200000};
        int sr = select(fd + 1, &r, NULL, NULL, &tv);
        if (sr > 0) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                fwrite(buf, 1, (size_t)n, f);
                total_bytes += n;
                for (ssize_t i = 0; i < n; i++) {
                    char ch = (char)buf[i];
                    if (ch == '\n' || ch == '\r') {
                        if (line_len > 0) { stats_add_line(&st, line, line_len); line_len = 0; }
                    } else if (line_len < LINEBUF - 1) {
                        line[line_len++] = ch;
                    } else {
                        line_len = 0;  /* absurdly long line: drop it */
                    }
                }
            }
        }

        int sec = (int)(now_monotonic() - t0);
        if (sec != last_shown) {
            last_shown = sec;
            char sats[16];
            if (st.last_sats < 0) snprintf(sats, sizeof(sats), "  -");
            else snprintf(sats, sizeof(sats), "%3d", st.last_sats);
            printf("\r  %3d/%.0fs  sentences %6ld  satellites %s  fix: %-24s",
                   sec, seconds, st.sentences, sats,
                   st.last_fix < 0 ? "waiting..." : fix_name(st.last_fix));
            fflush(stdout);
        }
    }
    printf("\n");

    double elapsed = now_monotonic() - t0;
    if (elapsed <= 0) elapsed = seconds;
    fflush(f);
    fclose(f);
    close(fd);

    long cs_total = st.cs_ok + st.cs_bad;
    double cs_pct = cs_total > 0 ? 100.0 * (double)st.cs_ok / (double)cs_total : 100.0;
    double rate = (double)st.sentences / elapsed;

    printf("\n");
    printf("KEY port %s\n", chosen);
    printf("KEY baud %d\n", chosen_baud);
    printf("KEY duration_s %.1f\n", elapsed);
    printf("KEY bytes %ld\n", total_bytes);
    printf("KEY sentences %ld\n", st.sentences);
    printf("KEY sentences_per_s %.2f\n", rate);
    printf("KEY checksum_ok %ld\n", st.cs_ok);
    printf("KEY checksum_bad %ld\n", st.cs_bad);
    printf("KEY checksum_pct %.2f\n", cs_pct);
    printf("KEY unicore_lines %ld\n", st.unicore);
    printf("KEY heading_sentences %ld\n", st.heading);
    printf("KEY max_satellites %d\n", st.max_sats);
    printf("KEY best_fix %d\n", st.best_fix);
    printf("KEY best_fix_name %s\n", fix_name(st.best_fix));
    for (int q = 0; q < 10; q++)
        if (st.fix_hist[q] > 0)
            printf("KEY fix_hist_%d %ld (%s)\n", q, st.fix_hist[q], fix_name(q));
    printf("KEY file %s\n", out_path);
    printf("KEY note no corrections used - SINGLE is the expected best result\n");

    if (st.sentences == 0 && st.unicore == 0) { printf("VERDICT FAIL\n"); return 1; }
    int stream_ok = rate >= HEALTHY_SENTENCES_PER_SEC;
    int cs_ok_flag = (cs_total == 0) || (cs_pct >= HEALTHY_CHECKSUM_PCT);
    if (stream_ok && cs_ok_flag && st.best_fix >= 1) { printf("VERDICT PASS\n"); return 0; }
    printf("VERDICT WARN\n");
    return 0;
}
