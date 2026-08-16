/* mid360cap.c -- raw Livox Mid-360 UDP capture for the LidarScan field-test kit.
 *
 * A single-file, dependency-free C99 port of tools/remote-capture/capture_mid360.py.
 * It exists so the macOS kit can be handed to someone with NOTHING installed --
 * no Python, no Homebrew -- and still produce a capture the dev-side
 * verify_capture.py reads without changes.
 *
 * Build:  cc -O2 -std=c99 -Wall -Wextra -o mid360cap mid360cap.c
 *
 * Usage:
 *   mid360cap [--seconds N] [--out FILE] [--host-ip IP] [--ports 56100,56101,...]
 *
 * FILE FORMAT (".livoxdump") -- byte-identical to capture_mid360.py, whose
 * docstring is the source of truth:
 *
 *   Header:
 *     8 bytes   magic       "LX360CAP"
 *     u16 LE    version     1
 *     u16 LE    num_ports   N
 *     N x u32 LE            the UDP port bound for port_idx 0..N-1
 *   Records, back to back until EOF:
 *     u64 LE    t_ns        arrival time, ns since the Unix epoch
 *     u16 LE    port_idx    index into the port table
 *     u32 LE    len         payload length
 *     len bytes payload     the raw datagram, byte for byte
 *
 * This program only LISTENS. A Mid-360 does not discover its host: it is told
 * where to stream by an SDK2 config push. If the unit has never been pointed
 * at this Mac, run Livox Viewer 2 once, quit it (it holds these ports), then
 * run this.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_PORTS 32
#define MAX_DGRAM 65535

/* A healthy Mid-360 point stream is roughly 2000 datagrams/s
 * (~200k points/s at 96 points per packet). */
#define HEALTHY_DATAGRAMS_PER_SEC 1500.0

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void put_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}
static void put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}
static void put_u64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

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

static uint64_t now_epoch_ns(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

static int parse_ports(const char *s, int *out, int max) {
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p || v <= 0 || v > 65535) return -1;
        out[n++] = (int)v;
        p = end;
    }
    return n;
}

int main(int argc, char **argv) {
    double seconds = 45.0;
    const char *out_path = "mid360.livoxdump";
    const char *host_ip = "0.0.0.0";
    int ports[MAX_PORTS];
    int n_ports = parse_ports("56100,56101,56200,56201,56300,56301,56400,56401,56500,56501",
                              ports, MAX_PORTS);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(a, "--out") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!strcmp(a, "--host-ip") && i + 1 < argc) {
            host_ip = argv[++i];
        } else if (!strcmp(a, "--ports") && i + 1 < argc) {
            n_ports = parse_ports(argv[++i], ports, MAX_PORTS);
            if (n_ports <= 0) { fprintf(stderr, "bad --ports list\n"); return 2; }
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("usage: mid360cap [--seconds N] [--out FILE] [--host-ip IP] [--ports P,P,...]\n");
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct in_addr bind_addr;
    if (!inet_aton(host_ip, &bind_addr)) {
        fprintf(stderr, "bad --host-ip: %s\n", host_ip);
        return 2;
    }

    int fds[MAX_PORTS];
    long long pkts[MAX_PORTS], bytes_[MAX_PORTS];
    for (int i = 0; i < n_ports; i++) { fds[i] = -1; pkts[i] = 0; bytes_[i] = 0; }

    int maxfd = -1;
    for (int i = 0; i < n_ports; i++) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { perror("socket"); return 3; }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        int rcvbuf = 8 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr = bind_addr;
        sa.sin_port = htons((uint16_t)ports[i]);
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            fprintf(stderr,
                    "\nERROR: could not open network port %d (%s).\n"
                    "Another program already owns it -- almost always Livox Viewer.\n"
                    "Quit Livox Viewer completely and run this test again.\n",
                    ports[i], strerror(errno));
            for (int j = 0; j < i; j++) close(fds[j]);
            return 3;
        }
        fds[i] = fd;
        if (fd > maxfd) maxfd = fd;
    }

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", out_path, strerror(errno));
        for (int i = 0; i < n_ports; i++) close(fds[i]);
        return 4;
    }

    unsigned char hdr[12];
    memcpy(hdr, "LX360CAP", 8);
    put_u16(hdr + 8, 1);
    put_u16(hdr + 10, (uint16_t)n_ports);
    fwrite(hdr, 1, 12, f);
    for (int i = 0; i < n_ports; i++) {
        unsigned char pb[4];
        put_u32(pb, (uint32_t)ports[i]);
        fwrite(pb, 1, 4, f);
    }

    printf("  Listening on %d network ports for %.0f seconds...\n\n", n_ports, seconds);
    fflush(stdout);

    unsigned char *buf = malloc(MAX_DGRAM);
    if (!buf) { fclose(f); return 5; }

    double t0 = now_monotonic();
    int last_shown = -1;
    long long total = 0;

    while (!g_stop) {
        double elapsed = now_monotonic() - t0;
        if (elapsed >= seconds) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        for (int i = 0; i < n_ports; i++) FD_SET(fds[i], &rfds);
        struct timeval tv;
        double remaining = seconds - elapsed;
        double wait = remaining < 0.2 ? remaining : 0.2;
        if (wait < 0) wait = 0;
        tv.tv_sec = (time_t)wait;
        tv.tv_usec = (suseconds_t)((wait - (double)tv.tv_sec) * 1e6);

        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            for (int i = 0; i < n_ports; i++) {
                if (!FD_ISSET(fds[i], &rfds)) continue;
                for (;;) {
                    ssize_t n = recv(fds[i], buf, MAX_DGRAM, MSG_DONTWAIT);
                    if (n <= 0) break;
                    unsigned char rec[14];
                    put_u64(rec, now_epoch_ns());
                    put_u16(rec + 8, (uint16_t)i);
                    put_u32(rec + 10, (uint32_t)n);
                    fwrite(rec, 1, 14, f);
                    fwrite(buf, 1, (size_t)n, f);
                    pkts[i]++;
                    bytes_[i] += n;
                    total++;
                }
            }
        }

        int sec = (int)(now_monotonic() - t0);
        if (sec != last_shown) {
            last_shown = sec;
            int best = 0;
            for (int i = 1; i < n_ports; i++) if (pkts[i] > pkts[best]) best = i;
            double el = now_monotonic() - t0;
            double rate = el > 0 ? (double)pkts[best] / el : 0.0;
            printf("\r  %3d / %3.0f s   %10lld packets   busiest port %d (%.0f/s)      ",
                   sec, seconds, total, ports[best], rate);
            fflush(stdout);
        }
    }
    printf("\n");

    double elapsed = now_monotonic() - t0;
    if (elapsed <= 0) elapsed = seconds;
    fflush(f);
    fclose(f);
    for (int i = 0; i < n_ports; i++) close(fds[i]);
    free(buf);

    int best = 0;
    long long total_bytes = 0;
    for (int i = 0; i < n_ports; i++) {
        total_bytes += bytes_[i];
        if (pkts[i] > pkts[best]) best = i;
    }
    double best_rate = (double)pkts[best] / elapsed;

    printf("\n");
    printf("KEY duration_s %.1f\n", elapsed);
    printf("KEY total_datagrams %lld\n", total);
    printf("KEY total_bytes %lld\n", total_bytes);
    for (int i = 0; i < n_ports; i++) {
        if (pkts[i] > 0)
            printf("KEY port_%d %lld pkts, %lld bytes, %.0f/s\n",
                   ports[i], pkts[i], bytes_[i], (double)pkts[i] / elapsed);
    }
    printf("KEY busiest_port %d\n", ports[best]);
    printf("KEY busiest_rate_per_s %.0f\n", best_rate);
    printf("KEY healthy_threshold_per_s %.0f\n", HEALTHY_DATAGRAMS_PER_SEC);
    printf("KEY file %s\n", out_path);

    if (best_rate >= HEALTHY_DATAGRAMS_PER_SEC) {
        printf("VERDICT PASS\n");
        return 0;
    } else if (total > 0) {
        printf("VERDICT WARN\n");
        return 0;
    }
    printf("VERDICT FAIL\n");
    return 1;
}
