// kslat userspace: load BPF, filter to one process, stream events to a binary file.
#include <argp.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include "kslat.h"
#include "kslat.skel.h"

static volatile sig_atomic_t stop;
static FILE *out;
static unsigned long long counts[16];

static void on_sig(int s) { (void)s; stop = 1; }

static int on_event(void *ctx, void *data, size_t sz)
{
    (void)ctx;
    const struct event *e = data;
    if (sz < sizeof(*e)) return 0;
    fwrite(e, sizeof(*e), 1, out);
    if (e->type < 16) counts[e->type]++;
    return 0;
}

static int libbpf_log(enum libbpf_print_level lvl, const char *fmt, va_list args)
{
    if (lvl == LIBBPF_DEBUG) return 0;
    return vfprintf(stderr, fmt, args);
}

int main(int argc, char **argv)
{
    int pid = 0, tid = 0, opt;
    const char *path = "events.bin";
    double duration = 0;
    while ((opt = getopt(argc, argv, "p:t:o:d:")) != -1) {
        switch (opt) {
        case 'p': pid = atoi(optarg); break;
        case 't': tid = atoi(optarg); break;
        case 'o': path = optarg; break;
        case 'd': duration = atof(optarg); break;
        default:
            fprintf(stderr, "usage: kslat -p PID [-t TID] [-o events.bin] [-d seconds]\n");
            return 1;
        }
    }
    if (!pid) { fprintf(stderr, "kslat: -p PID required\n"); return 1; }

    libbpf_set_print(libbpf_log);
    struct kslat_bpf *skel = kslat_bpf__open();
    if (!skel) { fprintf(stderr, "open failed\n"); return 1; }
    skel->rodata->target_tgid = pid;
    skel->rodata->target_tid = tid;
    if (kslat_bpf__load(skel)) { fprintf(stderr, "load failed\n"); return 1; }
    if (kslat_bpf__attach(skel)) { fprintf(stderr, "attach failed\n"); return 1; }

    out = fopen(path, "wb");
    if (!out) { perror(path); return 1; }
    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), on_event, NULL, NULL);
    if (!rb) { fprintf(stderr, "ringbuf failed\n"); return 1; }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    fprintf(stderr, "kslat: tracing pid %d -> %s\n", pid, path);
    fflush(stderr);

    while (!stop) {
        /* producers never wake us (BPF_RB_NO_WAKEUP), so drain on a 10ms timer */
        int r = ring_buffer__consume(rb);
        if (r < 0 && r != -EINTR) break;
        if (r > 0) fflush(out);          /* live dashboard tails this file */
        struct timespec nap = {0, 10 * 1000 * 1000};
        nanosleep(&nap, NULL);
        if (duration > 0) {
            struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
            if ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 > duration) break;
        }
        if (kill(pid, 0) != 0 && errno == ESRCH) break;   /* target exited */
    }
    ring_buffer__consume(rb);

    static const char *names[] = {"", "offcpu", "hardirq", "softirq", "vector", "fault",
                                  "tlb", "syscall", "reclaim", "compaction", "migrate"};
    fprintf(stderr, "kslat: done, dropped=%llu\n", (unsigned long long)skel->bss->dropped);
    for (int i = 1; i <= 10; i++)
        if (counts[i]) fprintf(stderr, "  %-11s %llu\n", names[i], counts[i]);

    fclose(out);
    ring_buffer__free(rb);
    kslat_bpf__destroy(skel);
    return 0;
}
