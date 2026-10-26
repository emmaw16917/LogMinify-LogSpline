// RSSPD with concurrent inserts (prototype): a map from strings to IDs that
// get_or_insert() and lookup() use from any number of threads.
//
// Structure (as RSSPD, rsspd.hpp): each node gathers the bytes at its keys' 8
// smallest adjacent-pair first-difference positions into a 64-bit value, which a
// radix spline maps to a narrow range of the node's sorted keys; values the spline
// cannot place go to children through an open-addressing table. The keys and
// child tables of a node never change once built.
//
// Inserts: a key is inserted at the node its lookup ends at, into that node's
// buffer: an open-addressing hash set of immutable records whose slots only ever
// go from empty to a record (published with release), so readers probe it without
// locks or version checks. The buffer is allocated on the first insert and holds
// up to max(buffer_min, keys of the subtree / 2^buffer_shift) records, so the
// rebuilds it triggers copy O(2^buffer_shift) keys per insert, amortized. The
// inserter holds the node's mutex while it probes again and allocates the ID, so
// concurrent inserts of one key agree on its ID. Routing depends only on the key
// and the (immutable) nodes it passes, so a key always reaches the node holding
// it, even if its bytes at the chosen positions do not order it among that node's
// keys.
//
// Rebuilds: when a buffer is half full, the background thread rebuilds the
// subtree under that node from all its keys (arrays and buffers) in a new
// segment; meanwhile, an inserter that finds the buffer full puts one twice as
// large in front of it (readers probe the chain), so inserts never wait for a
// rebuild (without the background thread, it rebuilds itself). One rebuild
// runs at a time, in two phases. (1) Snapshot: the subtree's keys are read
// without locks (inserts go on) and the new subtree is built, unpublished.
// (2) Catch-up: the old subtree is frozen (each node's mutex taken, `frozen`
// set), so it no longer changes; the keys buffered since the snapshot are put
// into the new subtree's buffers (nobody else sees it yet), and the parent's
// child pointer is switched with one atomic store. Inserters wait only during
// the catch-up, which handles a few keys. Readers are never blocked: one that
// entered the old subtree sees its content, one that enters later the new
// subtree, which has every key the old one had when frozen. Inserters that find
// a frozen node restart from the root until they reach the new subtree. The
// parent does not change, so routing to the subtree stays the same.
//
// Memory: a rebuild retires the replaced subtree (its nodes, buffers and
// records; a segment once all its nodes are retired) and the background thread
// frees it once no thread can still be in it, by epochs: every call stores the
// global epoch in the calling thread's slot (an acquire load and a relaxed
// store; a thread's slot only grows, so a store still in flight shows an older,
// safer value), a rebuild publishes the new subtree and then increments the
// epoch, and the subtree retired at epoch e is freed once every online slot is
// above e. A thread claims its slot on its first call (and comes back from
// thread_offline()) with a seq_cst store and fence, the one step that needs
// them. A thread that stopped calling the index holds reclamation back until it
// calls again, calls thread_offline() or exits. The background thread tries to
// reclaim after each rebuild, every 10 ms while idle and when stopped.
// retired_bytes() / freed_bytes() report the amounts.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "rs/builder.h"
#include "rs/radix_spline.h"

struct RSSPDCConfig {  // see RSSPDC
  size_t max_error = 1;         // spline error bound
  size_t radix_bits = 6;        // radix table bits of non-root splines
  size_t root_radix_bits = 10;  // ... of a root spline (rebuilt often: keep its table small)
  uint32_t buffer_min = 64;     // a node buffers at least this many keys
  uint32_t buffer_shift = 2;    // ... or its subtree's keys >> buffer_shift
  bool background = true;       // false: rebuild only when a buffer is full
#ifdef RSSPDC_BSTREE
  size_t bs_t = 4;              // a same-v group larger than this gets a child
#endif
};

class RSSPDC {
 public:
  using AllocId = uint64_t (*)();
  using Config = RSSPDCConfig;

  // keys: sorted and distinct; each gets an ID from alloc_id, in order
  explicit RSSPDC(AllocId alloc_id, const std::vector<std::string> &keys = {}, Config cfg = Config())
      : alloc_id_(alloc_id), cfg_(cfg) {
#ifdef RSSPDC_BSTREE
    if (const char *e = std::getenv("RSSPDC_BS_T")) {
      const unsigned long t = std::strtoul(e, nullptr, 10);
      if (t > 0) cfg_.bs_t = t;
    }
#endif
    if (const char *e = std::getenv("RSSPDC_MAX_ERROR")) {
      const unsigned long v = std::strtoul(e, nullptr, 10);
      if (v > 0) cfg_.max_error = v;
    }
    std::vector<Entry> entries;
    entries.reserve(keys.size());
    for (const auto &k : keys) entries.push_back({k, alloc_id_()});
    root_.store(build_segment(entries, &root_), std::memory_order_release);
    if (cfg_.background) bg_ = std::thread([this] { background_loop(); });
  }

  RSSPDC(const RSSPDC &) = delete;
  RSSPDC &operator=(const RSSPDC &) = delete;

  ~RSSPDC() {
    stop_background();
    for (auto &seg : segments_)  // live nodes and retired ones not yet freed
      for (auto &node : seg->nodes)
        if (node) free_buffers(node.get());
  }

  // Finish queued rebuilds, stop the background thread and free what is safe to
  // free; later rebuilds happen only in inserters whose buffer is full.
  void stop_background() {
    {
      std::lock_guard<std::mutex> lk(queue_mu_);
      stopping_ = true;
    }
    queue_cv_.notify_all();
    if (bg_.joinable()) bg_.join();
    std::lock_guard<std::mutex> rl(rebuild_mu_);
    reclaim_locked();
  }

  // The calling thread will not use the index until its next call, so it no
  // longer holds reclamation back.
  void thread_offline() const {
    ThreadSlot &t = thread_slot();
    if (t.owner != instance_ || t.slot == nullptr) return;
    t.slot->epoch.store(kOffline, std::memory_order_release);
    t.offline = true;
  }

  // ID of key, 0 if absent
  uint64_t lookup(std::string_view key) const {
    enter();
    uint64_t v;
    const Node *n = descend(key, v);
    if (const uint64_t id = search(n, v, key)) return id;
    return probe(n->buffer.load(std::memory_order_acquire), key);
  }

  // ID of key, inserting it with a new ID if absent
  uint64_t get_or_insert(std::string_view key) {
    enter();
    for (;;) {
      uint64_t v;
      Node *n = descend(key, v);
      if (const uint64_t id = search(n, v, key)) return id;
      if (const uint64_t id = probe(n->buffer.load(std::memory_order_acquire), key)) return id;

      std::unique_lock<std::mutex> lk(n->mu);
      if (n->frozen) {  // being replaced: retry from the root
        lk.unlock();
        frozen_retries_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
        continue;
      }
      Buffer *b = n->buffer.load(std::memory_order_relaxed);
      if (b == nullptr) {
        b = new Buffer(buffer_limit(n));
        n->buffer.store(b, std::memory_order_release);
      }
      // no insert into b can run now: this probe is final
      if (const uint64_t id = probe(b, key)) return id;
      if (b->count == b->limit) {
        if (!cfg_.background) {  // full: rebuild it here, then retry
          lk.unlock();
          full_rebuilds_.fetch_add(1, std::memory_order_relaxed);
          rebuild(n);
          continue;
        }
        // full: put a buffer twice as large in front, so inserts never wait for
        // the rebuild (already requested at half full)
        Buffer *nb = new Buffer(b->limit * 2);
        nb->next = b;
        n->buffer.store(nb, std::memory_order_release);
        b = nb;
        buffer_grows_.fetch_add(1, std::memory_order_relaxed);
      }
      Record *r = Record::make(key, alloc_id_());
      b->put(r);
      const bool request = cfg_.background && b->count * 2 >= b->limit && !n->rebuild_requested;
      if (request) n->rebuild_requested = true;
      lk.unlock();
      if (request) enqueue(n);
      return r->id;
    }
  }

  // All keys and IDs in key order. Call only while no thread inserts.
  std::vector<std::pair<std::string, uint64_t>> scan() const {
    std::vector<const Node *> live;
    collect(root_.load(std::memory_order_acquire), live);
    std::vector<Entry> all;
    for (const Node *n : live) append_entries(n, all);
    sort_unique(all);
    std::vector<std::pair<std::string, uint64_t>> out;
    out.reserve(all.size());
    for (const Entry &e : all) out.emplace_back(std::string(e.key), e.id);
    return out;
  }

  size_t rebuilds() const { return rebuilds_.load(); }
  size_t rebuilt_keys() const { return rebuilt_keys_.load(); }
  size_t retired_bytes() const { return retired_bytes_.load(); }  // replaced structures, total
  size_t freed_bytes() const { return freed_bytes_.load(); }      // ... of which freed
  size_t pending_bytes() const { return retired_bytes_.load() - freed_bytes_.load(); }
  size_t peak_pending_bytes() const { return peak_pending_bytes_.load(); }
  size_t frozen_retries() const { return frozen_retries_.load(); }
  size_t full_rebuilds() const { return full_rebuilds_.load(); }
  size_t buffer_grows() const { return buffer_grows_.load(); }
  double rebuild_s() const { return rebuild_ns_.load() / 1e9; }
  size_t late_keys() const { return late_keys_.load(); }  // keys added in rebuilds' catch-up phase

 private:
  static constexpr uint32_t kNone = 0xffffffffu;  // unused position: gathers 0

  // Epochs (see "Memory" above)
  static constexpr size_t kEpochSlots = 1024;  // threads using the index at once
  static constexpr uint64_t kOffline = ~0ULL;
  struct alignas(64) EpochSlot {
    std::atomic<uint64_t> epoch{kOffline};
    std::atomic<bool> used{false};
  };
  struct EpochSlots {
    EpochSlot slot[kEpochSlots];
    std::atomic<size_t> high{0};  // slots [0, high) have been claimed
  };
  // A thread's slot in the index it used last; shares the slots so that a thread
  // exiting after the index is destroyed releases its slot safely.
  struct ThreadSlot {
    std::shared_ptr<EpochSlots> slots;
    uint64_t owner = 0;  // instance_ of that index
    EpochSlot *slot = nullptr;
    bool offline = false;
    ~ThreadSlot() { release(); }
    void release() {
      if (slot != nullptr) {
        slot->epoch.store(kOffline, std::memory_order_release);
        slot->used.store(false, std::memory_order_release);
        slot = nullptr;
      }
      slots.reset();
      owner = 0;
    }
  };
  static ThreadSlot &thread_slot() {
    thread_local ThreadSlot t;
    return t;
  }
  static uint64_t next_instance() {
    static std::atomic<uint64_t> n{0};
    return ++n;
  }

  void enter() const {
    ThreadSlot &t = thread_slot();
    if (t.owner != instance_ || t.offline) return come_online(t);
    t.slot->epoch.store(epoch_.load(std::memory_order_acquire), std::memory_order_relaxed);
  }

  // Claim a slot (or reuse this thread's) and announce the thread; the fence pairs
  // with min_epoch()'s: a reclaimer that misses this store frees only what this
  // thread can no longer reach.
  void come_online(ThreadSlot &t) const {
    if (t.owner != instance_) {
      t.release();
      for (size_t i = 0;; ++i) {
        if (i == kEpochSlots) {
          std::fprintf(stderr, "RSSPDC: more than %zu threads\n", kEpochSlots);
          std::abort();
        }
        EpochSlot &s = slots_->slot[i];
        bool free_slot = false;
        if (!s.used.load(std::memory_order_relaxed) && s.used.compare_exchange_strong(free_slot, true)) {
          t.slot = &s;
          size_t h = slots_->high.load();
          while (h < i + 1 && !slots_->high.compare_exchange_weak(h, i + 1)) {
          }
          break;
        }
      }
      t.slots = slots_;
      t.owner = instance_;
    }
    t.offline = false;
    t.slot->epoch.store(epoch_.load(), std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  // smallest epoch of an online thread
  uint64_t min_epoch() const {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    uint64_t m = kOffline;
    const size_t h = slots_->high.load();
    for (size_t i = 0; i < h; ++i) m = std::min(m, slots_->slot[i].epoch.load(std::memory_order_acquire));
    return m;
  }

  struct Record {  // a buffered key; immutable once published
    uint64_t id;
    uint32_t len;
    char bytes[1];
    static Record *make(std::string_view k, uint64_t id) {
      Record *r = static_cast<Record *>(std::malloc(sizeof(Record) + k.size()));
      if (r == nullptr) throw std::bad_alloc();
      r->id = id;
      r->len = static_cast<uint32_t>(k.size());
      std::memcpy(r->bytes, k.data(), k.size());
      return r;
    }
    std::string_view key() const { return std::string_view(bytes, len); }
  };

  static uint64_t hash_key(std::string_view k) { return std::hash<std::string_view>()(k); }

  // Insert-only hash set of records: a slot goes from nullptr to a record once.
  struct Buffer {
    uint32_t limit;  // records it may hold (at most half the slots)
    uint32_t mask;
    uint32_t count = 0;  // written under the node's mutex only
    std::unique_ptr<std::atomic<Record *>[]> slots;
    const Buffer *next = nullptr;  // the full buffer this one was put in front of
    explicit Buffer(uint32_t lim) : limit(lim) {
      uint32_t cap = 4;
      while (cap < 2 * lim) cap <<= 1;
      mask = cap - 1;
      slots.reset(new std::atomic<Record *>[cap]);
      for (uint32_t i = 0; i < cap; ++i) slots[i].store(nullptr, std::memory_order_relaxed);
    }
    void put(Record *r) {
      for (uint64_t i = hash_key(r->key()) & mask;; i = (i + 1) & mask)
        if (slots[i].load(std::memory_order_relaxed) == nullptr) {
          slots[i].store(r, std::memory_order_release);
          ++count;
          return;
        }
    }
  };

  struct Entry {
    std::string_view key;
    uint64_t id;
  };

  struct Segment;

  struct Node {
    // immutable after the build
    const Segment *seg = nullptr;
    uint32_t min = 0, n = 0;   // keys [min, min + n) of seg
    uint32_t subtree_keys = 0;  // = n: a node's range covers its children's
    uint32_t pos[8] = {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
#if defined(RSSPDC_BSTREE)
    std::vector<uint64_t> chunks;  // open addressing: value -> child
    std::unique_ptr<std::atomic<Node *>[]> children;  // nullptr = empty slot; switched by rebuilds
    uint64_t mask = 0;
#elif defined(RSSPDC_HASH_V)
    uint32_t hmask = 0;
    std::vector<uint64_t> hkeys;     // open addressing: gathered value -> key range
    std::vector<uint32_t> hlo, hhi;  // [hlo, hhi) in seg
#else
    bool has_model = false;
    rs::RadixSpline<uint64_t> model;
    std::vector<uint64_t> chunks;  // open addressing: value -> child
    std::unique_ptr<std::atomic<Node *>[]> children;  // nullptr = empty slot; switched by rebuilds
    uint64_t mask = 0;
#endif
    std::atomic<Node *> *slot = nullptr;  // the pointer to this node (parent's child slot or root_)
    // inserts
    std::atomic<Buffer *> buffer{nullptr};  // set once, under mu
    std::mutex mu;                   // guards inserts and the flags below
    bool frozen = false;             // being or been replaced by a rebuild
    bool rebuild_requested = false;  // queued for the background thread
    bool retired = false;            // replaced
    // background thread only
    uint32_t idx = 0;     // in seg->nodes
    bool popped = false;  // its rebuild request left the queue: no pointer to it remains there

    uint64_t gather(const char *s, size_t len) const {
      uint64_t v = 0;
      for (int i = 0; i < 8; ++i) v = (v << 8) | (pos[i] < len ? static_cast<uint8_t>(s[pos[i]]) : 0);
      return v;
    }
#ifndef RSSPDC_HASH_V
    Node *find_child(uint64_t value) const {
      if (!children) return nullptr;
      for (uint64_t i = hash(value) & mask;; i = (i + 1) & mask) {
        Node *c = children[i].load(std::memory_order_acquire);
        if (c == nullptr) return nullptr;
        if (chunks[i] == value) return c;
      }
    }
    std::atomic<Node *> *child_slot(uint64_t value) {
      for (uint64_t i = hash(value) & mask;; i = (i + 1) & mask)
        if (children[i].load(std::memory_order_relaxed) == nullptr) {
          chunks[i] = value;
          return &children[i];
        }
    }
#endif
    static uint64_t hash(uint64_t x) {
      x ^= x >> 33;
      x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33;
      return x;
    }
  };

  struct Segment {  // the keys and nodes of one (re)build
    std::string bytes;
    std::vector<size_t> offsets;
    std::vector<uint64_t> ids;
    std::vector<std::unique_ptr<Node>> nodes;  // nullptr once freed
    uint32_t unretired = 0, unfreed = 0;      // nodes not yet retired / freed (under rebuild_mu_)
    std::string_view key(size_t i) const { return std::string_view(bytes.data() + offsets[i], offsets[i + 1] - offsets[i]); }
  };

  uint32_t buffer_limit(const Node *n) const {
    return std::max<uint32_t>(cfg_.buffer_min, n->subtree_keys >> cfg_.buffer_shift);
  }

  Node *descend(std::string_view key, uint64_t &v) const {
    return descend_from(root_.load(std::memory_order_acquire), key, v);
  }

  static Node *descend_from(Node *n, std::string_view key, uint64_t &v) {
#ifdef RSSPDC_HASH_V
    v = n->gather(key.data(), key.size());
    return n;
#else
    for (;;) {
      v = n->gather(key.data(), key.size());
      Node *c = n->find_child(v);
      if (c == nullptr) return n;
      n = c;
    }
#endif
  }

  static uint64_t search(const Node *n, uint64_t v, std::string_view key) {
#if defined(RSSPDC_BSTREE)
    if (n->n == 0) return 0;
    // lower_bound of v in [min, min+n)
    uint32_t l = n->min, h = n->min + n->n;
    while (l < h) {
      const uint32_t mid = l + (h - l) / 2;
      const std::string_view mk = n->seg->key(mid);
      const uint64_t mv = n->gather(mk.data(), mk.size());
      if (mv < v) l = mid + 1;
      else h = mid;
    }
    uint32_t lo = l;
    // upper_bound of v
    h = n->min + n->n;
    while (l < h) {
      const uint32_t mid = l + (h - l) / 2;
      const std::string_view mk = n->seg->key(mid);
      const uint64_t mv = n->gather(mk.data(), mk.size());
      if (mv <= v) l = mid + 1;
      else h = mid;
    }
    uint32_t hi = l;
    // the group has at most bs_t keys; compare full keys
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      const int c = n->seg->key(mid).compare(key);
      if (c == 0) return n->seg->ids[mid];
      if (c < 0) lo = mid + 1;
      else hi = mid;
    }
    return 0;
#elif defined(RSSPDC_HASH_V)
    if (n->hmask == 0) return 0;
    for (uint64_t i = Node::hash(v) & n->hmask;; i = (i + 1) & n->hmask) {
      if (n->hlo[i] == 0xffffffffu) return 0;
      if (n->hkeys[i] == v) {
        uint32_t lo = n->hlo[i], hi = n->hhi[i];
        while (lo < hi) {
          const uint32_t mid = lo + (hi - lo) / 2;
          const int c = n->seg->key(mid).compare(key);
          if (c == 0) return n->seg->ids[mid];
          if (c < 0) lo = mid + 1;
          else hi = mid;
        }
        return 0;
      }
    }
#else
    if (!n->has_model) return 0;
    const rs::SearchBound b = n->model.GetSearchBound(v);
    // the spline counts one key more than the node has, so clamp the end
    size_t lo = n->min + b.begin, hi = std::min<size_t>(n->min + b.end, n->min + n->n);
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      const int c = n->seg->key(mid).compare(key);
      if (c == 0) return n->seg->ids[mid];
      if (c < 0) lo = mid + 1;
      else hi = mid;
    }
    return 0;
#endif
  }

  static uint64_t probe(const Buffer *b, std::string_view key) {
    if (b == nullptr) return 0;
    const uint64_t h = hash_key(key);
    for (; b != nullptr; b = b->next)
      for (uint64_t i = h & b->mask;; i = (i + 1) & b->mask) {
        const Record *r = b->slots[i].load(std::memory_order_acquire);
        if (r == nullptr) break;
        if (r->len == key.size() && std::memcmp(r->bytes, key.data(), key.size()) == 0) return r->id;
      }
    return 0;
  }

  // nodes of the live subtree under n
  static void collect(const Node *n, std::vector<const Node *> &out) {
    out.push_back(n);
#ifndef RSSPDC_HASH_V
    if (!n->children) return;
    for (uint64_t i = 0; i <= n->mask; ++i)
      if (const Node *c = n->children[i].load(std::memory_order_acquire)) collect(c, out);
#endif
  }

  // the keys of n's array and buffers (views into segments and records, which
  // live until n is freed)
  static void append_entries(const Node *n, std::vector<Entry> &out) {
    for (uint32_t i = n->min; i < n->min + n->n; ++i) out.push_back({n->seg->key(i), n->seg->ids[i]});
    append_buffered(n, out);
  }

  static void append_buffered(const Node *n, std::vector<Entry> &out) {
    for (const Buffer *b = n->buffer.load(std::memory_order_acquire); b != nullptr; b = b->next)
      for (uint32_t i = 0; i <= b->mask; ++i)
        if (const Record *r = b->slots[i].load(std::memory_order_acquire)) out.push_back({r->key(), r->id});
  }

  static void sort_unique(std::vector<Entry> &v) {
    std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) { return a.key < b.key; });
    v.erase(std::unique(v.begin(), v.end(), [](const Entry &a, const Entry &b) { return a.key == b.key; }), v.end());
  }

  // Build a subtree of entries (sorted by key, distinct) in a new segment; its
  // root will be stored in *slot by the caller.
  Node *build_segment(const std::vector<Entry> &entries, std::atomic<Node *> *slot) {
    auto seg = std::make_unique<Segment>();
    seg->offsets.reserve(entries.size() + 1);
    for (const Entry &e : entries) {
      seg->offsets.push_back(seg->bytes.size());
      seg->bytes.append(e.key);
      seg->ids.push_back(e.id);
    }
    seg->offsets.push_back(seg->bytes.size());
    std::vector<uint32_t> diff(entries.empty() ? 0 : entries.size() - 1);
    for (size_t i = 0; i + 1 < entries.size(); ++i) {
      const std::string_view a = seg->key(i), b = seg->key(i + 1);
      uint32_t d = 0;
      while (d < a.size() && d < b.size() && a[d] == b[d]) ++d;
      diff[i] = d;
    }
    Node *root = build_node(*seg, diff, 0, (int)entries.size() - 1, 0, slot);
    seg->unretired = seg->unfreed = static_cast<uint32_t>(seg->nodes.size());
    std::lock_guard<std::mutex> lk(segments_mu_);
    segments_.push_back(std::move(seg));
    return root;
  }

#ifdef RSSPDC_HASH_V
  // Open addressing over the distinct gathered values of this node: v -> [lo,hi).
  static void build_hash(Node *node, const std::vector<uint64_t> &v) {
    size_t distinct = 0, maxgroup = 0;
    for (size_t i = 0; i < v.size();) {
      ++distinct;
      size_t j = i + 1;
      while (j < v.size() && v[j] == v[i]) ++j;
      if (j - i > maxgroup) maxgroup = j - i;
      i = j;
    }
    uint32_t cap = 4;
    while (cap < 2 * distinct) cap <<= 1;
    node->hmask = cap - 1;
    node->hkeys.assign(cap, 0);
    node->hlo.assign(cap, 0xffffffffu);
    node->hhi.assign(cap, 0);
    for (size_t i = 0; i < v.size();) {
      size_t j = i + 1;
      while (j < v.size() && v[j] == v[i]) ++j;
      uint64_t h = Node::hash(v[i]) & node->hmask;
      while (node->hlo[h] != 0xffffffffu) h = (h + 1) & node->hmask;
      node->hkeys[h] = v[i];
      node->hlo[h] = node->min + static_cast<uint32_t>(i);
      node->hhi[h] = node->min + static_cast<uint32_t>(j);
      i = j;
    }
  }
#endif

  Node *build_node(Segment &seg, const std::vector<uint32_t> &diff, int min, int max, int level,
                   std::atomic<Node *> *slot) {
    seg.nodes.push_back(std::make_unique<Node>());
    Node *node = seg.nodes.back().get();
    node->idx = static_cast<uint32_t>(seg.nodes.size() - 1);
    node->seg = &seg;
    node->slot = slot;
    if (max < min) return node;  // no keys: buffer only
    node->min = min;
    node->n = node->subtree_keys = max - min + 1;
    std::vector<uint32_t> ds(diff.begin() + min, diff.begin() + max);
    std::sort(ds.begin(), ds.end());
    ds.erase(std::unique(ds.begin(), ds.end()), ds.end());
    for (size_t i = 0; i < 8 && i < ds.size(); ++i) node->pos[i] = ds[i];

    std::vector<uint64_t> v(node->n);
    for (int i = min; i <= max; ++i) v[i - min] = node->gather(seg.key(i).data(), seg.key(i).size());
#if defined(RSSPDC_BSTREE)
    std::vector<std::pair<int, int>> groups;  // same-value groups larger than bs_t, [s, e]
    for (int i = 0; i < (int)v.size();) {
      int e = i;
      while (e + 1 < (int)v.size() && v[e + 1] == v[i]) ++e;
      if ((size_t)(e - i + 1) > cfg_.bs_t) groups.push_back({i, e});
      i = e + 1;
    }
    if (!groups.empty()) {
      size_t cap = 4;
      while (cap < groups.size() * 2) cap <<= 1;
      node->mask = cap - 1;
      node->chunks.assign(cap, 0);
      node->children.reset(new std::atomic<Node *>[cap]);
      for (size_t i = 0; i < cap; ++i) node->children[i].store(nullptr, std::memory_order_relaxed);
      for (const auto &g : groups) {
        std::atomic<Node *> *s = node->child_slot(v[g.first]);
        // mark the slot used before building (child_slot finds empty slots by nullptr)
        s->store(reinterpret_cast<Node *>(1), std::memory_order_relaxed);
        Node *child = build_node(seg, diff, min + g.first, min + g.second, level + 1, s);
        s->store(child, std::memory_order_relaxed);
      }
    }
    return node;
#elif defined(RSSPDC_HASH_V)
    build_hash(node, v);
    return node;
#else
    const uint64_t top = v.back() == ~0ULL ? v.back() : v.back() + 1;
    rs::Builder<uint64_t> rsb(v.front(), top, level == 0 ? cfg_.root_radix_bits : cfg_.radix_bits, cfg_.max_error);
    for (uint64_t c : v) rsb.AddKey(c);
    node->model = rsb.Finalize();
    node->has_model = true;

    std::vector<std::pair<int, int>> groups;  // equal values the spline cannot place, [s, e] relative to min
    for (int i = 0; i < (int)v.size();) {
      int e = i;
      while (e + 1 < (int)v.size() && v[e + 1] == v[i]) ++e;
      if (e > i) {
        bool naughty = false;
        for (int j = i; j <= e && !naughty; ++j) {
          const rs::SearchBound bd = node->model.GetSearchBound(v[j]);
          naughty = !(j >= (int)bd.begin && j < (int)bd.end);
        }
        if (naughty) groups.push_back({i, e});
      }
      i = e + 1;
    }
    if (!groups.empty()) {
      size_t cap = 4;
      while (cap < groups.size() * 2) cap <<= 1;
      node->mask = cap - 1;
      node->chunks.assign(cap, 0);
      node->children.reset(new std::atomic<Node *>[cap]);
      for (size_t i = 0; i < cap; ++i) node->children[i].store(nullptr, std::memory_order_relaxed);
      for (const auto &g : groups) {
        std::atomic<Node *> *s = node->child_slot(v[g.first]);
        // mark the slot used before building (child_slot finds empty slots by nullptr)
        s->store(reinterpret_cast<Node *>(1), std::memory_order_relaxed);
        Node *child = build_node(seg, diff, min + g.first, min + g.second, level + 1, s);
        s->store(child, std::memory_order_relaxed);
      }
    }
    return node;
#endif
  }

  // Replace the subtree under n (if still live) by one rebuilt from all its keys.
  void rebuild(Node *n) {
    std::lock_guard<std::mutex> rl(rebuild_mu_);
    const auto t0 = std::chrono::steady_clock::now();
    struct Timer {
      std::atomic<size_t> &ns;
      std::chrono::steady_clock::time_point t0;
      ~Timer() { ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); }
    } timer{rebuild_ns_, t0};
    {
      std::lock_guard<std::mutex> lk(n->mu);
      if (n->retired) return;  // replaced meanwhile (with an ancestor)
    }
    // only rebuilds change child pointers, so the subtree's nodes stay these
    std::vector<const Node *> sub;
    collect(n, sub);
    // 1. snapshot (inserts go on) and build the new subtree, unpublished
    std::vector<Entry> entries;
    for (const Node *m : sub) append_entries(m, entries);
    sort_unique(entries);
    Node *fresh = build_segment(entries, n->slot);
    // 2. freeze, add the keys buffered since the snapshot, publish
    for (const Node *m : sub) {
      Node *mm = const_cast<Node *>(m);
      std::lock_guard<std::mutex> lk(mm->mu);
      mm->frozen = true;
    }
    std::vector<Entry> late;
    for (const Node *m : sub) append_buffered(m, late);  // arrays do not change
    const auto by_key = [](const Entry &a, const Entry &b) { return a.key < b.key; };
    late.erase(std::remove_if(late.begin(), late.end(),
                              [&](const Entry &e) { return std::binary_search(entries.begin(), entries.end(), e, by_key); }),
               late.end());
    std::vector<std::pair<Node *, size_t>> target;  // node of the new subtree a late key reaches
    for (size_t i = 0; i < late.size(); ++i) {
      uint64_t v;
      target.push_back({descend_from(fresh, late[i].key, v), i});
    }
    std::sort(target.begin(), target.end());
    for (size_t i = 0; i < target.size();) {
      size_t e = i;
      while (e < target.size() && target[e].first == target[i].first) ++e;
      Node *t = target[i].first;
      Buffer *b = new Buffer(std::max<uint32_t>(buffer_limit(t), static_cast<uint32_t>(e - i)));
      for (size_t j = i; j < e; ++j) b->put(Record::make(late[target[j].second].key, late[target[j].second].id));
      t->buffer.store(b, std::memory_order_relaxed);  // published with the subtree below
      i = e;
    }
    late_keys_.fetch_add(late.size());
    n->slot->store(fresh, std::memory_order_release);
    // 3. retire the old subtree: threads that loaded the new epoch see fresh
    Retired r{epoch_.fetch_add(1, std::memory_order_seq_cst), {}};
    size_t bytes = 0;
    for (const Node *m : sub) {
      Node *mm = const_cast<Node *>(m);
      {
        std::lock_guard<std::mutex> lk(mm->mu);
        mm->retired = true;
      }
      r.nodes.push_back(mm);
      bytes += node_bytes(mm);
      Segment *s = const_cast<Segment *>(mm->seg);
      if (--s->unretired == 0) bytes += segment_bytes(*s);
    }
    retired_.push_back(std::move(r));
    const size_t pending = retired_bytes_.fetch_add(bytes) + bytes - freed_bytes_.load();
    if (pending > peak_pending_bytes_.load()) peak_pending_bytes_.store(pending);
    rebuilt_keys_.fetch_add(entries.size());
    rebuilds_.fetch_add(1);
    reclaim_locked();
  }

  struct Retired {  // a replaced subtree
    uint64_t epoch;  // freed once every online thread is past it
    std::vector<Node *> nodes;
  };

  static void free_buffers(Node *n) {
    for (const Buffer *b = n->buffer.load(std::memory_order_relaxed); b != nullptr;) {
      for (uint32_t i = 0; i <= b->mask; ++i) std::free(b->slots[i].load(std::memory_order_relaxed));
      const Buffer *next = b->next;
      delete b;
      b = next;
    }
    n->buffer.store(nullptr, std::memory_order_relaxed);
  }

  static size_t node_bytes(const Node *n) {
#if defined(RSSPDC_BSTREE)
    size_t bytes = sizeof(Node) + n->chunks.capacity() * sizeof(uint64_t) + (n->children ? (n->mask + 1) * sizeof(void *) : 0);
#elif defined(RSSPDC_HASH_V)
    size_t bytes = sizeof(Node) + n->hkeys.capacity() * sizeof(uint64_t) +
                   (n->hlo.capacity() + n->hhi.capacity()) * sizeof(uint32_t);
#else
    size_t bytes = sizeof(Node) + n->chunks.capacity() * sizeof(uint64_t) + (n->children ? (n->mask + 1) * sizeof(void *) : 0);
    if (n->has_model) bytes += n->model.GetSize() - sizeof(n->model);
#endif
    for (const Buffer *b = n->buffer.load(std::memory_order_relaxed); b != nullptr; b = b->next) {
      bytes += sizeof(Buffer) + (b->mask + 1) * sizeof(void *);
      for (uint32_t i = 0; i <= b->mask; ++i)
        if (const Record *r = b->slots[i].load(std::memory_order_relaxed)) bytes += sizeof(Record) + r->len;
    }
    return bytes;
  }

  static size_t segment_bytes(const Segment &s) {
    return sizeof(Segment) + s.bytes.capacity() + s.offsets.capacity() * sizeof(size_t) +
           s.ids.capacity() * sizeof(uint64_t) + s.nodes.capacity() * sizeof(void *);
  }

  // Free the retired subtrees no thread can be in. Holds rebuild_mu_.
  void reclaim_locked() {
    if (retired_.empty()) return;
    const uint64_t safe = min_epoch();
    for (auto it = retired_.begin(); it != retired_.end();) {
      bool ok = it->epoch < safe;
      for (size_t i = 0; ok && i < it->nodes.size(); ++i)  // still queued: rebuild() will look at it
        ok = !it->nodes[i]->rebuild_requested || it->nodes[i]->popped;
      if (!ok) {
        ++it;
        continue;
      }
      size_t bytes = 0;
      for (Node *m : it->nodes) {
        bytes += node_bytes(m);
        free_buffers(m);
        Segment *s = const_cast<Segment *>(m->seg);
        s->nodes[m->idx].reset();
        if (--s->unfreed == 0) {
          bytes += segment_bytes(*s);
          std::lock_guard<std::mutex> lk(segments_mu_);
          for (auto &p : segments_)
            if (p.get() == s) {
              p = std::move(segments_.back());
              segments_.pop_back();
              break;
            }
        }
      }
      freed_bytes_.fetch_add(bytes);
      it = retired_.erase(it);
    }
  }

  void enqueue(Node *n) {
    {
      std::lock_guard<std::mutex> lk(queue_mu_);
      queue_.push_back(n);
    }
    queue_cv_.notify_one();
  }

  void background_loop() {
    std::unique_lock<std::mutex> lk(queue_mu_);
    for (;;) {
      if (!queue_cv_.wait_for(lk, std::chrono::milliseconds(10), [this] { return stopping_ || !queue_.empty(); })) {
        lk.unlock();  // idle: free what has become safe since
        {
          std::lock_guard<std::mutex> rl(rebuild_mu_);
          reclaim_locked();
        }
        lk.lock();
        continue;
      }
      if (queue_.empty()) return;  // stopping, nothing left
      Node *n = queue_.front();
      queue_.pop_front();
      lk.unlock();
      rebuild(n);
      {
        std::lock_guard<std::mutex> rl(rebuild_mu_);
        n->popped = true;
      }
      lk.lock();
    }
  }

  AllocId alloc_id_;
  Config cfg_;
  std::atomic<Node *> root_{nullptr};
  std::mutex segments_mu_;
  std::vector<std::unique_ptr<Segment>> segments_;  // segments with nodes not yet freed
  std::mutex rebuild_mu_;                            // one rebuild at a time; guards retired_
  std::list<Retired> retired_;
  const uint64_t instance_ = next_instance();
  std::shared_ptr<EpochSlots> slots_ = std::make_shared<EpochSlots>();
  std::atomic<uint64_t> epoch_{1};
  std::atomic<size_t> freed_bytes_{0}, peak_pending_bytes_{0};
  std::mutex queue_mu_;
  std::condition_variable queue_cv_;
  std::deque<Node *> queue_;
  bool stopping_ = false;
  std::thread bg_;
  std::atomic<size_t> rebuilds_{0}, rebuilt_keys_{0}, retired_bytes_{0}, late_keys_{0}, frozen_retries_{0}, full_rebuilds_{0}, rebuild_ns_{0}, buffer_grows_{0};
};
