// kslat engine: a pinned hot thread applies order-book updates and records a span
// per update on CLOCK_MONOTONIC - the same clock bpf_ktime_get_ns() uses, so spans
// join 1:1 with kernel events from the tracer.
//
//   --feed synthetic   generated flow, paced by busy-wait (default)
//   --feed bitstamp    live L3 order feed   (--symbol btcusd)
//   --feed binance     live L2 depth feed   (--symbol BTCUSDT)
//
// Threads:  feed (socket + JSON)  --SPSC-->  hot (book)  --SPSC-->  writer (disk, status)
//
// Span = {seq, t_rx, t0, t1, wire_ns, op, inject}
//   t1 - t0    service time (what the hot thread spent)
//   t1 - t_rx  tick-to-book latency, includes queueing behind earlier updates
//   wire_ns    exchange timestamp -> our receive, across clocks (NTP skew included)
//
// Known-cause injectors for validating attribution (ground truth kept per span):
//   --no-prefault  --fault-every N  --syscall-every N  --tlb-noise
#include "feeds.hpp"
#include "order_book.hpp"
#include "spsc.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <random>
#include <sched.h>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <unistd.h>
#include <vector>

using namespace lob;
using feed::BookMsg;

enum Inject : uint32_t { INJ_FAULT = 1, INJ_SYSCALL = 2 };

struct Span {            // 48 bytes, appended to spans.bin
    uint64_t seq;
    uint64_t t_rx;
    uint64_t t0;
    uint64_t t1;
    int64_t  wire_ns;
    uint32_t op;         // low byte: feed::Op, bit 8: last op of an exchange message
    uint32_t inject;
};
static_assert(sizeof(Span) == 48);

static inline uint64_t now_ns() { return feed::mono_ns(); }

struct Args {
    std::string feed = "synthetic", symbol, ws_url, rest_url, out = "run";
    uint64_t n = 2'000'000, rate = 500'000, seed = 42;
    double duration = 0, tick = 0, lot = 0;
    int cpu = 1, aux_cpu = 0;
    bool prefault = true, tlb_noise = false, wait_signal = false, tick_fixed = false;
    uint64_t fault_every = 0, syscall_every = 0;
};

static void usage() {
    fprintf(stderr,
        "engine [--feed synthetic|bitstamp|binance] [--symbol S] [--tick T] [--lot L]\n"
        "       [--ws-url URL] [--rest-url URL] [--duration SEC] [--out DIR] [--wait]\n"
        "       [--cpu C] [--aux-cpu C]            hot thread core / feed+writer+noise core\n"
        "       [--n N] [--rate R] [--seed S]      synthetic flow\n"
        "       [--no-prefault] [--fault-every N] [--syscall-every N] [--tlb-noise]\n");
    exit(1);
}

static Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&]() -> const char* { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (s == "--feed") a.feed = next();
        else if (s == "--symbol") a.symbol = next();
        else if (s == "--tick") a.tick = atof(next()), a.tick_fixed = true;
        else if (s == "--lot") a.lot = atof(next()), a.tick_fixed = true;
        else if (s == "--ws-url") a.ws_url = next();
        else if (s == "--rest-url") a.rest_url = next();
        else if (s == "--duration") a.duration = atof(next());
        else if (s == "--n") a.n = strtoull(next(), 0, 10);
        else if (s == "--rate") a.rate = strtoull(next(), 0, 10);
        else if (s == "--seed") a.seed = strtoull(next(), 0, 10);
        else if (s == "--cpu") a.cpu = atoi(next());
        else if (s == "--aux-cpu" || s == "--noise-cpu") a.aux_cpu = atoi(next());
        else if (s == "--out") a.out = next();
        else if (s == "--no-prefault") a.prefault = false;
        else if (s == "--fault-every") a.fault_every = strtoull(next(), 0, 10);
        else if (s == "--syscall-every") a.syscall_every = strtoull(next(), 0, 10);
        else if (s == "--tlb-noise") a.tlb_noise = true;
        else if (s == "--wait") a.wait_signal = true;
        else usage();
    }
    if (a.feed == "bitstamp") {
        if (a.symbol.empty()) a.symbol = "btcusd";
        if (!a.tick) a.tick = 1;          // fallback; auto-detected from trading-pairs-info
        if (!a.lot) a.lot = 1e-8;
    } else if (a.feed == "binance") {
        if (a.symbol.empty()) a.symbol = "BTCUSDT";
        if (!a.tick) a.tick = 0.01;
        if (!a.lot) a.lot = 1e-5;
    } else if (a.feed == "synthetic") {
        if (!a.tick) a.tick = 0.01;
        if (!a.lot) a.lot = 1;
    } else usage();
    return a;
}

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s) != 0) perror("sched_setaffinity");
}

static std::string g_out;
// Record our own threads so the analyzer can tell self-inflicted preemption apart.
static void register_thread(const char* name) {
    pthread_setname_np(pthread_self(), name);
    FILE* f = fopen((g_out + "/threads").c_str(), "a");
    if (f) { fprintf(f, "%ld %s\n", syscall(SYS_gettid), name); fclose(f); }
}

// Start a thread whose affinity is set before it first runs: a std::thread would
// inherit the hot CPU mask and could run there briefly before pinning itself.
template <typename F>
static pthread_t spawn_on(int cpu, F fn) {
    auto* heap = new F(std::move(fn));
    pthread_attr_t at; pthread_attr_init(&at);
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    pthread_attr_setaffinity_np(&at, sizeof(s), &s);
    pthread_t t;
    pthread_create(&t, &at, [](void* p) -> void* { auto* f = static_cast<F*>(p); (*f)(); delete f; return nullptr; }, heap);
    pthread_attr_destroy(&at);
    return t;
}

static std::atomic<bool> g_stop{false};
static void on_stop(int) { g_stop = true; }

// Hot thread -> writer: published with relaxed stores every update (cheap), read ~2x/s.
struct HotStats {
    alignas(64) std::atomic<uint64_t> msgs{0};
    std::atomic<uint32_t> best_bid{0}, best_ask{0};
    std::atomic<uint64_t> bid_qty{0}, ask_qty{0}, live{0}, rejected{0};
    std::atomic<uint64_t> span_drops{0};
    std::atomic<uint64_t> crossed{0};     // updates after which best bid >= best ask (should stay 0)
    std::atomic<double> microprice{0};
};

// ------------------------------------------------------------- synthetic flow
static std::vector<BookMsg> gen_flow(const Args& a, uint32_t ticks) {
    std::mt19937_64 rng(a.seed);
    std::uniform_real_distribution<double> U(0, 1);
    std::geometric_distribution<int> depth(0.3);
    std::vector<BookMsg> v; v.reserve(a.n);
    std::vector<uint64_t> live; live.reserve(1 << 20);
    double mid = ticks / 2.0;
    uint64_t next_id = 1;
    for (uint64_t i = 0; i < a.n; i++) {
        mid += (U(rng) - 0.5) * 0.2;
        BookMsg m{};
        m.last = 1;
        double r = U(rng);
        uint64_t qty = 1 + uint64_t(U(rng) * 100);
        if (r < 0.55 || live.empty()) {
            m.side = U(rng) < 0.5 ? 0 : 1;
            int d = 1 + depth(rng);
            m.price = m.side == 0 ? uint32_t(mid) - d : uint32_t(mid) + d;
            m.id = next_id; m.qty = qty; m.op = feed::OP_ADD;
            live.push_back(next_id++);
        } else if (r < 0.92) {
            size_t k = size_t(U(rng) * live.size());
            m.id = live[k]; m.op = feed::OP_CANCEL;
            live[k] = live.back(); live.pop_back();
        } else {
            m.side = U(rng) < 0.5 ? 0 : 1;
            m.price = m.side == 0 ? uint32_t(mid) + 5 : uint32_t(mid) - 5;
            m.id = next_id++; m.qty = qty * 3; m.op = feed::OP_AGGRESS;
        }
        v.push_back(m);
    }
    return v;
}

// ------------------------------------------------------------- TLB noise
// Sibling thread in the same mm: map, touch, unmap. Each munmap must flush TLBs on
// every CPU running this mm -> IPI into the hot thread's CPU.
static void tlb_noise_thread(int) {
    register_thread("tlb-noise");
    const size_t len = 16 * 4096;
    while (!g_stop.load(std::memory_order_relaxed)) {
        void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) continue;
        for (size_t o = 0; o < len; o += 4096) static_cast<volatile char*>(p)[o] = 1;
        munmap(p, len);
        usleep(200);
    }
}

static void write_meta(const Args& a, pid_t tid, uint64_t t_start, uint64_t t_end, const OrderBook* book) {
    std::string p = a.out + "/meta.json", tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f,
        "{\"span_version\": 2, \"mode\": \"%s\", \"symbol\": \"%s\", \"tick\": %g, \"lot\": %g,\n"
        " \"pid\": %d, \"tid\": %d, \"cpu\": %d, \"aux_cpu\": %d, \"n\": %lu, \"rate\": %lu,\n"
        " \"prefault\": %s, \"fault_every\": %lu, \"syscall_every\": %lu, \"tlb_noise\": %s,\n"
        " \"t_start\": %lu, \"t_end\": %lu",
        a.feed.c_str(), a.symbol.c_str(), a.tick, a.lot, getpid(), tid, a.cpu, a.aux_cpu,
        (unsigned long)a.n, (unsigned long)a.rate, a.prefault ? "true" : "false",
        (unsigned long)a.fault_every, (unsigned long)a.syscall_every, a.tlb_noise ? "true" : "false",
        (unsigned long)t_start, (unsigned long)t_end);
    if (book)
        fprintf(f, ",\n \"trades\": %lu, \"adds\": %lu, \"cancels\": %lu, \"modifies\": %lu, \"rejected\": %lu",
                (unsigned long)book->stats().trades, (unsigned long)book->stats().adds,
                (unsigned long)book->stats().cancels, (unsigned long)book->stats().modifies,
                (unsigned long)book->stats().rejected);
    fprintf(f, "}\n");
    fclose(f);
    rename(tmp.c_str(), p.c_str());
}

// ------------------------------------------------------------- writer thread
static void writer_thread(const Args& a, Spsc<Span>& spans, HotStats& hs, feed::FeedStats& fs,
                          feed::Scale& sc, uint64_t t_start) {
    register_thread("kslat-writer");
    std::string path = a.out + "/spans.bin";
    FILE* f = fopen(path.c_str(), "wb");
    std::vector<Span> batch; batch.reserve(8192);
    uint64_t last_flush = now_ns(), last_status = 0;
    auto flush = [&]() {
        if (!batch.empty()) { fwrite(batch.data(), sizeof(Span), batch.size(), f); batch.clear(); }
        fflush(f);
    };
    for (;;) {
        Span s;
        bool got = false;
        while (batch.size() < 8192 && spans.pop(s)) { batch.push_back(s); got = true; }
        if (batch.size() >= 8192) { fwrite(batch.data(), sizeof(Span), batch.size(), f); batch.clear(); }
        uint64_t now = now_ns();
        if (now - last_flush > 200'000'000) { flush(); last_flush = now; }
        if (now - last_status > 500'000'000) {
            last_status = now;
            std::string p = a.out + "/status.json", tmp = p + ".tmp";
            if (FILE* st = fopen(tmp.c_str(), "w")) {
                uint64_t n = hs.msgs.load(std::memory_order_acquire);
                bool hb = hs.bid_qty.load() > 0, ha = hs.ask_qty.load() > 0;
                fprintf(st,
                    "{\"t\": %lu, \"uptime_s\": %.1f, \"feed\": \"%s\", \"symbol\": \"%s\", \"live\": %s, \"tick\": %g,\n"
                    " \"updates\": %lu, \"best_bid\": %s, \"best_ask\": %s, \"bid_qty\": %.8g, \"ask_qty\": %.8g,\n"
                    " \"microprice\": %s, \"live_orders\": %lu, \"rejected\": %lu, \"span_drops\": %lu, \"crossed\": %lu,\n"
                    " \"frames\": %lu, \"feed_ops\": %lu, \"out_of_range\": %lu, \"queue_full\": %lu,\n"
                    " \"reconnects\": %lu, \"gaps\": %lu, \"snapshots\": %lu, \"last_wire_us\": %.1f,\n"
                    " \"last_error\": \"%s\"}\n",
                    (unsigned long)now, (now - t_start) / 1e9, a.feed.c_str(), a.symbol.c_str(),
                    (a.feed == "synthetic" || fs.live) ? "true" : "false", sc.tick,
                    (unsigned long)n,
                    hb ? std::to_string(sc.to_price(hs.best_bid.load())).c_str() : "null",
                    ha ? std::to_string(sc.to_price(hs.best_ask.load())).c_str() : "null",
                    hs.bid_qty.load() * sc.lot, hs.ask_qty.load() * sc.lot,
                    (hb && ha) ? std::to_string(sc.base >= 0 ? (hs.microprice.load() - sc.window / 2 + sc.base) * sc.tick
                                                             : hs.microprice.load()).c_str() : "null",
                    (unsigned long)hs.live.load(), (unsigned long)hs.rejected.load(),
                    (unsigned long)hs.span_drops.load(), (unsigned long)hs.crossed.load(),
                    (unsigned long)fs.frames.load(),
                    (unsigned long)fs.ops.load(), (unsigned long)fs.out_of_range.load(),
                    (unsigned long)fs.queue_full.load(), (unsigned long)fs.reconnects.load(),
                    (unsigned long)fs.gaps.load(), (unsigned long)fs.snapshots.load(),
                    fs.last_wire_ns.load() / 1e3, fs.last_error);
                fclose(st);
                rename(tmp.c_str(), p.c_str());
            }
        }
        if (!got) {
            if (g_stop.load() && !spans.pop(s)) break;
            usleep(1000);
        }
    }
    Span s;
    while (spans.pop(s)) batch.push_back(s);
    flush();
    fclose(f);
}

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    const uint32_t TICKS = a.feed == "synthetic" ? (1u << 16) : (1u << 20);

    // Block SIGUSR1 (start signal) in every thread; SIGINT/SIGTERM stop the run.
    sigset_t ss; sigemptyset(&ss); sigaddset(&ss, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &ss, nullptr);
    struct sigaction sa{}; sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, nullptr); sigaction(SIGTERM, &sa, nullptr);

    g_out = a.out;
    std::string cmd = "mkdir -p '" + a.out + "'";
    if (system(cmd.c_str()) != 0) return 1;

    std::vector<BookMsg> flow;
    if (a.feed == "synthetic") {
        fprintf(stderr, "generating %lu msgs...\n", (unsigned long)a.n);
        flow = gen_flow(a, TICKS);
    }
    OrderBook book(TICKS, a.feed == "synthetic" ? (1 << 21) : (1 << 22), a.prefault);

    const size_t fault_pages = 1 << 16;
    char* fault_region = nullptr;
    if (a.fault_every) {
        fault_region = (char*)mmap(nullptr, fault_pages * 4096, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        madvise(fault_region, fault_pages * 4096, MADV_NOHUGEPAGE);
    }
    int logfd = a.syscall_every ? open("/dev/null", O_WRONLY) : -1;

    auto spans = std::make_unique<Spsc<Span>>(1 << 20);
    auto inq = std::make_unique<Spsc<BookMsg>>(1 << 18);
    HotStats hs;
    feed::FeedStats fs;
    feed::Scale sc; sc.tick = a.tick; sc.lot = a.lot; sc.window = TICKS;
    sc.fixed = a.tick_fixed;
    if (a.feed == "synthetic") sc.base = TICKS / 2;       // synthetic prices are already ticks

    unlink((a.out + "/threads").c_str());
    pin(a.cpu);
    pid_t tid = (pid_t)syscall(SYS_gettid);
    register_thread("kslat-hot");
    { FILE* f = fopen((a.out + "/ready").c_str(), "w"); fprintf(f, "%d %d\n", getpid(), tid); fclose(f); }
    if (a.wait_signal) {
        fprintf(stderr, "ready pid=%d tid=%d, waiting for SIGUSR1\n", getpid(), tid);
        int sig; sigwait(&ss, &sig);
    }

    uint64_t start = now_ns();
    write_meta(a, tid, start, 0, nullptr);

    pthread_t noise = 0, writer, feeder = 0;
    std::unique_ptr<feed::Adapter> adapter;
    if (a.tlb_noise) noise = spawn_on(a.aux_cpu, [&]() { tlb_noise_thread(a.aux_cpu); });
    writer = spawn_on(a.aux_cpu, [&]() { writer_thread(a, *spans, hs, fs, sc, start); });
    if (a.feed == "bitstamp")
        adapter = std::make_unique<feed::Bitstamp>(*inq, fs, sc, a.symbol, a.ws_url, a.rest_url);
    else if (a.feed == "binance")
        adapter = std::make_unique<feed::Binance>(*inq, fs, sc, a.symbol, a.ws_url, a.rest_url);
    if (adapter)
        feeder = spawn_on(a.aux_cpu, [&]() { register_thread("kslat-feed"); adapter->run(g_stop); });

    const uint64_t interval = a.rate ? 1000000000ull / a.rate : 0;
    const uint64_t deadline = a.duration > 0 ? start + uint64_t(a.duration * 1e9) : UINT64_MAX;
    uint64_t seq = 0, fault_idx = 0;
    char logline[8] = "trade\n";
    const bool synthetic = a.feed == "synthetic";

    // ------------------------------------------------------------- hot loop
    for (;;) {
        BookMsg m;
        if (synthetic) {
            if (seq == flow.size() || g_stop.load(std::memory_order_relaxed)) break;
            m = flow[seq];
            m.t_rx = start + seq * interval;                    // when it was "due"
            if (interval) while (now_ns() < m.t_rx) __builtin_ia32_pause();
            else m.t_rx = now_ns();
        } else {
            if (!inq->pop(m)) {
                if ((seq & 0xff) == 0 && (g_stop.load(std::memory_order_relaxed) || now_ns() > deadline)) break;
                __builtin_ia32_pause();
                continue;
            }
        }
        uint32_t inj = 0;
        uint64_t t0 = now_ns();

        switch (m.op) {
            case feed::OP_ADD:
            case feed::OP_AGGRESS: book.add(m.id, Side(m.side), m.price, m.qty); break;
            case feed::OP_CANCEL:
            case feed::OP_DELETE:  book.cancel(m.id); break;
            case feed::OP_REST:    book.add_resting(m.id, Side(m.side), m.price, m.qty); break;
            case feed::OP_MODIFY:  book.modify(m.id, m.qty); break;
        }
        // top-of-book signal: size-weighted microprice (in ticks)
        uint64_t bq = book.bid_qty(), aq = book.ask_qty();
        double micro = (bq + aq) ? (double(book.best_bid()) * aq + double(book.best_ask()) * bq) / double(bq + aq) : 0;

        if (a.fault_every && seq % a.fault_every == a.fault_every - 1) {
            size_t pg = fault_idx++ % fault_pages;
            fault_region[pg * 4096] = 1;
            if (pg == fault_pages - 1) madvise(fault_region, fault_pages * 4096, MADV_DONTNEED);
            inj |= INJ_FAULT;
        }
        if (a.syscall_every && seq % a.syscall_every == a.syscall_every - 1) {
            if (write(logfd, logline, 6) < 0) {}
            inj |= INJ_SYSCALL;
        }
        uint64_t t1 = now_ns();

        if (!spans->push(Span{seq, m.t_rx, t0, t1, m.wire_ns, uint32_t(m.op) | (uint32_t(m.last) << 8), inj}))
            hs.span_drops.fetch_add(1, std::memory_order_relaxed);
        hs.best_bid.store(book.best_bid(), std::memory_order_relaxed);
        hs.best_ask.store(book.best_ask(), std::memory_order_relaxed);
        hs.bid_qty.store(bq, std::memory_order_relaxed);
        hs.ask_qty.store(aq, std::memory_order_relaxed);
        hs.microprice.store(micro, std::memory_order_relaxed);
        hs.live.store(book.live_orders(), std::memory_order_relaxed);
        hs.rejected.store(book.stats().rejected, std::memory_order_relaxed);
        if (!synthetic && m.last && bq && aq && book.best_bid() >= book.best_ask())
            hs.crossed.fetch_add(1, std::memory_order_relaxed);
        hs.msgs.store(++seq, std::memory_order_release);
        if (!synthetic && now_ns() > deadline) break;
    }
    uint64_t end = now_ns();
    g_stop = true;
    (void)feeder;                                // may be blocked in a socket read; _exit below
    if (noise) pthread_join(noise, nullptr);
    pthread_join(writer, nullptr);
    write_meta(a, tid, start, end, &book);
    fprintf(stderr, "done: %lu updates in %.3fs, trades=%lu rejected=%lu span_drops=%lu\n",
            (unsigned long)seq, (end - start) / 1e9, (unsigned long)book.stats().trades,
            (unsigned long)book.stats().rejected, (unsigned long)hs.span_drops.load());
    _exit(0);                                    // don't wait on a detached feed thread
}
