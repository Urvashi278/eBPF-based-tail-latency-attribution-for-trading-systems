// Live market-data adapters. Each runs on the feed thread (never the hot thread):
// socket read -> JSON parse -> normalize to integer book ops -> SPSC push.
//
//   bitstamp  L3: every individual order (created / changed / deleted) via the public
//             live_orders_<pair> channel, seeded from the REST per-order snapshot.
//   binance   L2: <sym>@depth@100ms diff stream, seeded from the REST depth snapshot
//             with the documented U/u sequence sync; a gap triggers a resync.
#pragma once
#include "net.hpp"
#include "spsc.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <unordered_set>
#include <string>
#include <string_view>
#include <time.h>
#include <vector>

namespace feed {

enum Op : uint8_t { OP_ADD = 0, OP_CANCEL = 1, OP_AGGRESS = 2,   // synthetic matching flow
                    OP_REST = 3, OP_MODIFY = 4, OP_DELETE = 5 }; // exchange mirror ops

struct BookMsg {
    uint64_t id;
    uint64_t qty;       // lots
    uint64_t t_rx;      // CLOCK_MONOTONIC ns when the frame was fully read off the socket
    int64_t  wire_ns;   // CLOCK_REALTIME at rx - exchange timestamp (clock skew included)
    uint32_t price;     // ticks in the book's window
    uint8_t  op;
    uint8_t  side;      // 0 buy, 1 sell
    uint8_t  last;      // last op produced from this exchange message
    uint8_t  pad;
};

inline uint64_t mono_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ull + t.tv_nsec; }
inline int64_t real_ns() { timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec * 1000000000ll + t.tv_nsec; }

struct FeedStats {
    std::atomic<uint64_t> frames{0}, ops{0}, out_of_range{0}, queue_full{0};
    std::atomic<uint64_t> reconnects{0}, gaps{0}, snapshots{0};
    std::atomic<int64_t> last_wire_ns{0};
    std::atomic<bool> live{false};
    char last_error[160] = {0};
};

// ---------------------------------------------------------------- tiny JSON helpers
// Enough for these flat exchange messages; not a general JSON parser.
inline const char* skip_ws(const char* p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

// pointer to the value of "key" (first occurrence), or nullptr
inline const char* find_key(std::string_view js, std::string_view key) {
    std::string pat = "\"" + std::string(key) + "\"";
    size_t p = js.find(pat);
    if (p == std::string_view::npos) return nullptr;
    const char* c = skip_ws(js.data() + p + pat.size());
    if (*c != ':') return nullptr;
    return skip_ws(c + 1);
}

// read a string or number token at p; advances p past it
inline std::string token(const char*& p) {
    p = skip_ws(p);
    std::string s;
    if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; s += *p++; } if (*p) p++; }
    else { while (*p && *p != ',' && *p != ']' && *p != '}' && *p != ' ') s += *p++; }
    return s;
}

inline std::string get_str(std::string_view js, std::string_view key) {
    const char* p = find_key(js, key);
    return p ? token(p) : std::string();
}

// [[a,b,...],[a,b,...]] -> rows of tokens
inline std::vector<std::vector<std::string>> get_rows(std::string_view js, std::string_view key) {
    std::vector<std::vector<std::string>> rows;
    const char* p = find_key(js, key);
    if (!p || *p != '[') return rows;
    p = skip_ws(p + 1);
    while (*p == '[') {
        std::vector<std::string> row;
        p++;
        for (;;) {
            p = skip_ws(p);
            if (*p == ']') { p++; break; }
            row.push_back(token(p));
            p = skip_ws(p);
            if (*p == ',') p++;
        }
        rows.push_back(std::move(row));
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }
    return rows;
}

// ---------------------------------------------------------------- price/qty scaling
struct Scale {
    double tick = 0.01, lot = 1e-8;
    bool fixed = false;          // user passed --tick/--lot: don't auto-detect
    int64_t base = -1;           // exchange ticks mapped to the middle of the book window
    uint32_t window = 1 << 20;
    bool to_ticks(double px, uint32_t& out) {
        int64_t t = llround(px / tick);
        if (base < 0) base = t;                       // first price seen centres the window
        int64_t rel = t - base + window / 2;
        if (rel <= 0 || rel >= (int64_t)window) return false;
        out = (uint32_t)rel; return true;
    }
    uint64_t to_lots(double q) const { return (uint64_t)llround(q / lot); }
    double to_price(uint32_t ticks) const { return (double(ticks) - window / 2 + base) * tick; }
};

class Adapter {
public:
    Adapter(Spsc<BookMsg>& q, FeedStats& st, Scale& sc) : q_(q), st_(st), sc_(sc) {}
    virtual ~Adapter() = default;
    virtual void run(std::atomic<bool>& stop) = 0;

protected:
    void push(BookMsg m) {
        while (!q_.push(m)) { st_.queue_full++; __builtin_ia32_pause(); }
        st_.ops++;
    }
    void emit(uint8_t op, uint8_t side, double px, uint64_t id, double qty, uint64_t t_rx,
              int64_t wire, bool last) {
        BookMsg m{};
        if (op != OP_DELETE && op != OP_MODIFY) {
            if (!sc_.to_ticks(px, m.price)) { st_.out_of_range++; return; }
        }
        m.id = id; m.qty = sc_.to_lots(qty); m.t_rx = t_rx; m.wire_ns = wire;
        m.op = op; m.side = side; m.last = last;
        push(m);
    }
    void error(const std::string& e) {
        snprintf(st_.last_error, sizeof st_.last_error, "%s", e.c_str());
        fprintf(stderr, "feed: %s\n", e.c_str());
    }
    Spsc<BookMsg>& q_;
    FeedStats& st_;
    Scale& sc_;
};

// ---------------------------------------------------------------- Bitstamp (L3)
class Bitstamp : public Adapter {
public:
    Bitstamp(Spsc<BookMsg>& q, FeedStats& st, Scale& sc, std::string pair, std::string ws, std::string rest)
        : Adapter(q, st, sc), pair_(std::move(pair)),
          ws_url_(ws.empty() ? "wss://ws.bitstamp.net" : ws),
          rest_url_(rest.empty() ? "https://www.bitstamp.net" : rest) {}

    void run(std::atomic<bool>& stop) override {
        bool first = true;
        while (!stop) {
            try {
                if (!first) { st_.reconnects++; st_.live = false; }
                first = false;
                session(stop);
            } catch (std::exception& e) {
                error(std::string("bitstamp: ") + e.what());
                st_.live = false;
                for (int i = 0; i < 20 && !stop; i++) usleep(100000);
            }
        }
    }

private:
    // tick/lot from the venue: counter_decimals / base_decimals of the pair
    void detect_scale() {
        if (sc_.fixed || detected_) return;
        try {
            std::string info = net::http_get(rest_url_ + "/api/v2/trading-pairs-info/");
            size_t p = info.find("\"url_symbol\": \"" + pair_ + "\"");
            if (p == std::string::npos) p = info.find("\"url_symbol\":\"" + pair_ + "\"");
            if (p == std::string::npos) throw std::runtime_error("pair not listed");
            size_t a = info.rfind('{', p), b = info.find('}', p);
            std::string_view obj(info.data() + a, b - a + 1);
            int cd = atoi(get_str(obj, "counter_decimals").c_str());
            int bd = atoi(get_str(obj, "base_decimals").c_str());
            sc_.tick = pow(10.0, -cd); sc_.lot = pow(10.0, -bd);
            fprintf(stderr, "feed: bitstamp %s tick=%g lot=%g (from trading-pairs-info)\n", pair_.c_str(), sc_.tick, sc_.lot);
        } catch (std::exception& e) {
            fprintf(stderr, "feed: tick/lot auto-detect failed (%s), using tick=%g lot=%g\n", e.what(), sc_.tick, sc_.lot);
        }
        detected_ = true;
    }

    void session(std::atomic<bool>& stop) {
        detect_scale();
        net::WebSocket ws(ws_url_);
        ws.send_text("{\"event\":\"bts:subscribe\",\"data\":{\"channel\":\"live_orders_" + pair_ + "\"}}");

        // Buffer stream events while fetching the snapshot, then replay the newer ones.
        std::deque<std::pair<std::string, uint64_t>> buffered;
        std::string msg;
        // wait for subscription ack (and buffer anything that arrives)
        for (int i = 0; i < 50; i++) {
            if (!ws.recv(msg)) throw std::runtime_error("closed during subscribe");
            if (msg.find("subscription_succeeded") != std::string::npos) break;
            buffered.emplace_back(msg, mono_ns());
        }
        std::string snap = net::http_get(rest_url_ + "/api/v2/order_book/" + pair_ + "/?group=2");
        uint64_t snap_us = strtoull(get_str(snap, "microtimestamp").c_str(), nullptr, 10);
        uint64_t now = mono_ns();
        auto bids = get_rows(snap, "bids"), asks = get_rows(snap, "asks");
        if (bids.empty() && asks.empty()) throw std::runtime_error("empty snapshot: " + snap.substr(0, 120));
        // centre the price window on the snapshot mid before the first conversion
        if (sc_.base < 0 && !bids.empty() && !asks.empty())
            sc_.base = llround((atof(bids[0][0].c_str()) + atof(asks[0][0].c_str())) / 2 / sc_.tick);
        size_t n = 0, total = bids.size() + asks.size();
        for (auto* side : {&bids, &asks})
            for (auto& r : *side) {
                if (r.size() < 3) continue;
                emit(OP_REST, side == &bids ? 0 : 1, atof(r[0].c_str()), strtoull(r[2].c_str(), nullptr, 10),
                     atof(r[1].c_str()), now, 0, ++n == total);
            }
        st_.snapshots++;
        fprintf(stderr, "feed: bitstamp %s snapshot %zu bids / %zu asks\n", pair_.c_str(), bids.size(), asks.size());
        st_.live = true;

        for (auto& [m, t] : buffered) handle(m, t, snap_us);
        while (!stop) {
            if (!ws.recv(msg)) throw std::runtime_error("server closed");
            handle(msg, mono_ns(), snap_us);
        }
    }

    void handle(const std::string& m, uint64_t t_rx, uint64_t snap_us) {
        st_.frames++;
        std::string ev = get_str(m, "event");
        if (ev == "bts:request_reconnect") throw std::runtime_error("server requested reconnect");
        uint8_t op;
        if (ev == "order_created") op = OP_REST;
        else if (ev == "order_changed") op = OP_MODIFY;
        else if (ev == "order_deleted") op = OP_DELETE;
        else return;
        uint64_t us = strtoull(get_str(m, "microtimestamp").c_str(), nullptr, 10);
        if (us && us <= snap_us) return;                          // already in the snapshot
        int64_t wire = us ? real_ns() - (int64_t)us * 1000 : 0;
        st_.last_wire_ns = wire;
        uint64_t id = strtoull(get_str(m, "id").c_str(), nullptr, 10);
        uint8_t side = get_str(m, "order_type") == "1" ? 1 : 0;
        std::string amt = get_str(m, "amount_str");
        if (amt.empty()) amt = get_str(m, "amount");
        std::string px = get_str(m, "price_str");
        if (px.empty()) px = get_str(m, "price");
        emit(op, side, atof(px.c_str()), id, atof(amt.c_str()), t_rx, wire, true);
    }

    std::string pair_, ws_url_, rest_url_;
    bool detected_ = false;
};

// ---------------------------------------------------------------- Binance (L2)
// Each price level is mirrored as one synthetic order: id = side<<40 | tick.
class Binance : public Adapter {
public:
    Binance(Spsc<BookMsg>& q, FeedStats& st, Scale& sc, std::string sym, std::string ws, std::string rest)
        : Adapter(q, st, sc), sym_(std::move(sym)),
          ws_url_(ws.empty() ? "wss://stream.binance.com:9443" : ws),
          rest_url_(rest.empty() ? "https://api.binance.com" : rest) {
        for (auto& c : sym_) c = (char)tolower(c);
    }

    void run(std::atomic<bool>& stop) override {
        bool first = true;
        while (!stop) {
            try {
                if (!first) { st_.reconnects++; st_.live = false; }
                first = false;
                session(stop);
            } catch (std::exception& e) {
                error(std::string("binance: ") + e.what());
                st_.live = false;
                for (int i = 0; i < 20 && !stop; i++) usleep(100000);
            }
        }
    }

private:
    void clear_book(uint64_t t) {
        // resync: delete every level we mirrored
        size_t n = 0, total = levels_.size();
        for (uint64_t id : levels_) {
            BookMsg m{}; m.id = id; m.op = OP_DELETE; m.t_rx = t; m.last = ++n == total; push(m);
        }
        (void)total;
        levels_.clear();
    }

    void level(uint8_t side, const std::string& p, const std::string& q, uint64_t t, int64_t wire, bool last) {
        double px = atof(p.c_str()), qty = atof(q.c_str());
        uint32_t ticks;
        if (!sc_.to_ticks(px, ticks)) { st_.out_of_range++; return; }
        uint64_t id = (uint64_t(side) + 1) << 40 | ticks;
        BookMsg m{}; m.id = id; m.price = ticks; m.side = side; m.t_rx = t; m.wire_ns = wire; m.last = last;
        if (qty == 0) { m.op = OP_DELETE; levels_.erase(id); }
        else { m.op = OP_REST; m.qty = sc_.to_lots(qty); levels_.insert(id); }   // REST on existing id = set qty
        push(m);
    }

    // tick/lot from exchangeInfo: PRICE_FILTER.tickSize / LOT_SIZE.stepSize
    void detect_scale(const std::string& upper) {
        if (sc_.fixed || detected_) return;
        try {
            std::string info = net::http_get(rest_url_ + "/api/v3/exchangeInfo?symbol=" + upper);
            double t = atof(get_str(info, "tickSize").c_str()), l = atof(get_str(info, "stepSize").c_str());
            if (t <= 0 || l <= 0) throw std::runtime_error("no tickSize/stepSize");
            sc_.tick = t; sc_.lot = l;
            fprintf(stderr, "feed: binance %s tick=%g lot=%g (from exchangeInfo)\n", upper.c_str(), t, l);
        } catch (std::exception& e) {
            fprintf(stderr, "feed: tick/lot auto-detect failed (%s), using tick=%g lot=%g\n", e.what(), sc_.tick, sc_.lot);
        }
        detected_ = true;
    }

    void session(std::atomic<bool>& stop) {
        std::string upper = sym_; for (auto& c : upper) c = (char)toupper(c);
        detect_scale(upper);
        net::WebSocket ws(ws_url_ + "/ws/" + sym_ + "@depth@100ms");
        std::deque<std::pair<std::string, uint64_t>> buffered;
        std::string msg;
        // collect at least one diff before the snapshot so the sync rule can be checked
        if (!ws.recv(msg)) throw std::runtime_error("closed");
        buffered.emplace_back(msg, mono_ns());
        std::string snap = net::http_get(rest_url_ + "/api/v3/depth?symbol=" + upper + "&limit=5000");
        uint64_t last_id = strtoull(get_str(snap, "lastUpdateId").c_str(), nullptr, 10);
        if (!last_id) throw std::runtime_error("bad snapshot: " + snap.substr(0, 120));
        auto bids = get_rows(snap, "bids"), asks = get_rows(snap, "asks");
        if (sc_.base < 0 && !bids.empty() && !asks.empty())
            sc_.base = llround((atof(bids[0][0].c_str()) + atof(asks[0][0].c_str())) / 2 / sc_.tick);
        uint64_t now = mono_ns();
        clear_book(now);
        size_t n = 0, total = bids.size() + asks.size();
        for (auto& r : bids) if (r.size() >= 2) level(0, r[0], r[1], now, 0, ++n == total);
        for (auto& r : asks) if (r.size() >= 2) level(1, r[0], r[1], now, 0, ++n == total);
        st_.snapshots++;
        fprintf(stderr, "feed: binance %s snapshot %zu bids / %zu asks (lastUpdateId %lu)\n",
                upper.c_str(), bids.size(), asks.size(), (unsigned long)last_id);
        st_.live = true;

        uint64_t prev_u = 0;
        auto apply = [&](const std::string& m, uint64_t t) -> bool {
            st_.frames++;
            if (get_str(m, "e") != "depthUpdate") return true;
            uint64_t U = strtoull(get_str(m, "U").c_str(), nullptr, 10);
            uint64_t u = strtoull(get_str(m, "u").c_str(), nullptr, 10);
            if (u <= last_id) return true;                               // older than snapshot
            if (prev_u == 0) { if (U > last_id + 1) return false; }      // must straddle snapshot
            else if (U != prev_u + 1) return false;                      // gap
            prev_u = u;
            int64_t E = atoll(get_str(m, "E").c_str());
            int64_t wire = E ? real_ns() - E * 1000000 : 0;
            st_.last_wire_ns = wire;
            auto b = get_rows(m, "b"), a = get_rows(m, "a");
            size_t k = 0, tot = b.size() + a.size();
            for (auto& r : b) if (r.size() >= 2) level(0, r[0], r[1], t, wire, ++k == tot);
            for (auto& r : a) if (r.size() >= 2) level(1, r[0], r[1], t, wire, ++k == tot);
            return true;
        };
        for (auto& [m, t] : buffered)
            if (!apply(m, t)) { st_.gaps++; throw std::runtime_error("sequence gap during sync"); }
        while (!stop) {
            if (!ws.recv(msg)) throw std::runtime_error("server closed");
            if (!apply(msg, mono_ns())) { st_.gaps++; throw std::runtime_error("sequence gap, resyncing"); }
        }
    }

    std::string sym_, ws_url_, rest_url_;
    std::unordered_set<uint64_t> levels_;   // mirrored level ids, for resync
    bool detected_ = false;
};

} // namespace feed
