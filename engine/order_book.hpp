// Price-time priority limit order book.
// Hot path does no heap allocation: orders come from a fixed mmap pool, price levels
// are a flat array indexed by tick, order ids live in a preallocated open-addressing map
// (exchange ids are arbitrary 64-bit values, so a flat array by id doesn't work).
//
// Two ways to drive it:
//   add()          matching engine: cross against the other side, rest the remainder
//   add_resting()  / modify() / cancel(): mirror an exchange's L3 book (the exchange
//                  already matched; we just replay its order state changes)
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <sys/mman.h>

namespace lob {

enum class Side : uint8_t { Buy = 0, Sell = 1 };

struct Order {
    uint64_t id;
    uint64_t qty;     // integer lots
    uint32_t price;   // ticks, relative to the book's price base
    Side side;
    Order* prev;
    Order* next;
};

struct Level {
    Order* head = nullptr;
    Order* tail = nullptr;
    uint64_t total_qty = 0;
};

// Anonymous-mmap pool. If prefault=false, pages are touched lazily on first use ->
// first-touch page faults inside the hot path (a classic bug kslat should catch).
template <typename T>
class Pool {
public:
    Pool(size_t cap, bool prefault) : cap_(cap) {
        bytes_ = cap * sizeof(T);
        void* p = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | (prefault ? MAP_POPULATE : 0), -1, 0);
        if (p == MAP_FAILED) throw std::runtime_error("pool mmap failed");
        mem_ = static_cast<T*>(p);
        if (prefault) std::memset(p, 0, bytes_);
    }
    ~Pool() { munmap(mem_, bytes_); }
    Pool(const Pool&) = delete;
    T* alloc() {
        if (free_) { T* t = free_; free_ = *reinterpret_cast<T**>(t); return t; }
        if (next_ == cap_) return nullptr;
        return &mem_[next_++];
    }
    void release(T* t) { *reinterpret_cast<T**>(t) = free_; free_ = t; }
private:
    T* mem_;
    size_t cap_, bytes_, next_ = 0;
    T* free_ = nullptr;
};

// Open addressing, linear probing, backward-shift delete (no tombstones).
// Fixed power-of-two capacity, preallocated. Key 0 is reserved as "empty".
class IdMap {
public:
    explicit IdMap(size_t cap_pow2) : mask_(cap_pow2 - 1), keys_(cap_pow2, 0), vals_(cap_pow2, nullptr) {}
    Order* get(uint64_t k) const {
        for (size_t i = h(k);; i = (i + 1) & mask_) {
            if (keys_[i] == k) return vals_[i];
            if (keys_[i] == 0) return nullptr;
        }
    }
    bool put(uint64_t k, Order* v) {
        if (size_ * 4 >= (mask_ + 1) * 3) return false;          // keep load < 75%
        for (size_t i = h(k);; i = (i + 1) & mask_) {
            if (keys_[i] == 0) { keys_[i] = k; vals_[i] = v; size_++; return true; }
            if (keys_[i] == k) { vals_[i] = v; return true; }
        }
    }
    void erase(uint64_t k) {
        size_t i = h(k);
        for (;; i = (i + 1) & mask_) {
            if (keys_[i] == 0) return;
            if (keys_[i] == k) break;
        }
        size_--;
        for (size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            if (keys_[j] == 0) break;
            size_t home = h(keys_[j]);
            // move j back into hole i if its home slot is not in (i, j]
            if (((j - home) & mask_) >= ((j - i) & mask_)) {
                keys_[i] = keys_[j]; vals_[i] = vals_[j]; i = j;
            }
        }
        keys_[i] = 0; vals_[i] = nullptr;
    }
    size_t size() const { return size_; }
private:
    // Exchange order ids are mostly increasing, so keep neighbouring ids in neighbouring
    // slots (cache/TLB locality) and only fold the high bits in. A full avalanche hash
    // (splitmix) scatters them over the whole table: p50 went 60ns -> 200ns.
    size_t h(uint64_t k) const { return (k ^ (k >> 29) ^ (k >> 47)) & mask_; }
    size_t mask_, size_ = 0;
    std::vector<uint64_t> keys_;
    std::vector<Order*> vals_;
};

struct Stats {
    uint64_t adds = 0, cancels = 0, modifies = 0, trades = 0, traded_qty = 0;
    uint64_t rejected = 0;          // out of price range / pool full / unknown id
};

class OrderBook {
public:
    OrderBook(uint32_t max_ticks, size_t max_orders, bool prefault)
        : levels_{std::vector<Level>(max_ticks), std::vector<Level>(max_ticks)},
          ids_(pow2_at_least(max_orders * 2)), pool_(max_orders, prefault),
          max_ticks_(max_ticks), best_bid_(0), best_ask_(max_ticks) {}

    // Matching-engine add: cross first, then rest the remainder.
    void add(uint64_t id, Side side, uint32_t price, uint64_t qty) {
        stats_.adds++;
        if (price == 0 || price >= max_ticks_) { stats_.rejected++; return; }
        qty = match(side, price, qty);
        if (qty) rest(id, side, price, qty);
    }

    // Mirror mode: the exchange says this order now rests on the book.
    void add_resting(uint64_t id, Side side, uint32_t price, uint64_t qty) {
        stats_.adds++;
        if (price == 0 || price >= max_ticks_ || qty == 0) { stats_.rejected++; return; }
        if (ids_.get(id)) { modify(id, qty); return; }
        rest(id, side, price, qty);
    }

    // New remaining quantity for an existing order (partial fill or size change).
    // Reductions keep queue priority; increases lose it (moved to the back), as on most venues.
    void modify(uint64_t id, uint64_t new_qty) {
        Order* o = ids_.get(id);
        if (!o) { stats_.rejected++; return; }
        stats_.modifies++;
        if (new_qty == 0) { remove(o); return; }
        Level& lv = levels_[idx(o->side)][o->price];
        if (new_qty <= o->qty) {
            lv.total_qty -= o->qty - new_qty;
            o->qty = new_qty;
        } else {
            Side s = o->side; uint32_t p = o->price;
            remove(o);
            rest(id, s, p, new_qty);
        }
    }

    void cancel(uint64_t id) {
        Order* o = ids_.get(id);
        if (!o) { stats_.rejected++; return; }
        stats_.cancels++;
        remove(o);
    }

    uint32_t best_bid() const { return best_bid_; }
    uint32_t best_ask() const { return best_ask_; }
    uint32_t max_ticks() const { return max_ticks_; }
    bool has_bid() const { return best_bid_ > 0 && levels_[0][best_bid_].head; }
    bool has_ask() const { return best_ask_ < max_ticks_ && levels_[1][best_ask_].head; }
    uint64_t bid_qty() const { return has_bid() ? levels_[0][best_bid_].total_qty : 0; }
    uint64_t ask_qty() const { return has_ask() ? levels_[1][best_ask_].total_qty : 0; }
    size_t live_orders() const { return ids_.size(); }
    const Stats& stats() const { return stats_; }

private:
    static size_t pow2_at_least(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }
    static int idx(Side s) { return s == Side::Buy ? 0 : 1; }

    void rest(uint64_t id, Side side, uint32_t price, uint64_t qty) {
        Order* o = pool_.alloc();
        if (!o) { stats_.rejected++; return; }
        if (!ids_.put(id, o)) { pool_.release(o); stats_.rejected++; return; }
        *o = Order{id, qty, price, side, nullptr, nullptr};
        Level& lv = levels_[idx(side)][price];
        o->prev = lv.tail;
        if (lv.tail) lv.tail->next = o; else lv.head = o;
        lv.tail = o;
        lv.total_qty += qty;
        if (side == Side::Buy) { if (price > best_bid_ || !has_bid()) best_bid_ = price; }
        else { if (price < best_ask_ || !has_ask()) best_ask_ = price; }
    }

    uint64_t match(Side side, uint32_t price, uint64_t qty) {
        if (side == Side::Buy) {
            while (qty && has_ask() && best_ask_ <= price)
                qty = take_level(levels_[1][best_ask_], qty), advance_ask();
        } else {
            while (qty && has_bid() && best_bid_ >= price)
                qty = take_level(levels_[0][best_bid_], qty), advance_bid();
        }
        return qty;
    }

    uint64_t take_level(Level& lv, uint64_t qty) {
        while (qty && lv.head) {
            Order* o = lv.head;
            uint64_t fill = o->qty < qty ? o->qty : qty;
            o->qty -= fill; qty -= fill; lv.total_qty -= fill;
            stats_.trades++; stats_.traded_qty += fill;
            if (o->qty == 0) {
                lv.head = o->next;
                if (lv.head) lv.head->prev = nullptr; else lv.tail = nullptr;
                ids_.erase(o->id);
                pool_.release(o);
            }
        }
        return qty;
    }

    void remove(Order* o) {
        Level& lv = levels_[idx(o->side)][o->price];
        if (o->prev) o->prev->next = o->next; else lv.head = o->next;
        if (o->next) o->next->prev = o->prev; else lv.tail = o->prev;
        lv.total_qty -= o->qty;
        if (!lv.head) {
            if (o->side == Side::Buy && o->price == best_bid_) advance_bid();
            if (o->side == Side::Sell && o->price == best_ask_) advance_ask();
        }
        ids_.erase(o->id);
        pool_.release(o);
    }

    // Linear scan to the next non-empty level. Fine for dense books near the touch;
    // on a sparse book this is itself a tail-latency source kslat will show as "unexplained".
    void advance_bid() { while (best_bid_ > 0 && !levels_[0][best_bid_].head) --best_bid_; }
    void advance_ask() { while (best_ask_ < max_ticks_ && !levels_[1][best_ask_].head) ++best_ask_; }

    std::vector<Level> levels_[2];
    IdMap ids_;
    Pool<Order> pool_;
    uint32_t max_ticks_;
    uint32_t best_bid_, best_ask_;
    Stats stats_;
};

} // namespace lob
