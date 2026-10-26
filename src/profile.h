// Opt-in timing for finding where logminify spends its time (build with
// -DLOGMINIFY_PROFILE=ON). PROF_SCOPE(slot) adds the wall time of the enclosing
// scope to a global per-slot counter, summed over all threads, so a slot's total
// divided by block1's total is that phase's share of the worker time.
// PROF_MARK(label) prints the main thread's wall time since the previous mark.
// Without LOGMINIFY_PROFILE both macros expand to nothing.
#ifndef LOGMINIFY_PROFILE_H
#define LOGMINIFY_PROFILE_H

#ifdef LOGMINIFY_PROFILE
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace prof {

enum Slot { block0, block1, parse, varext, tmplstore, index, numwrite, compress, num_slots };
inline const char *const slot_names[num_slots] = {
    "stage0 block (total)", "stage1 block (total)", "  replace_and_group",
    "  variable_extract",   "  store_template_with_ids", "    index get_or_insert",
    "  number files",       "  compressDirectory"};
inline std::atomic<uint64_t> ns[num_slots];
inline std::atomic<uint64_t> calls[num_slots];

inline uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Scope {
  Slot slot;
  uint64_t start = now_ns();
  explicit Scope(Slot s) : slot(s) {}
  ~Scope() {
    ns[slot].fetch_add(now_ns() - start, std::memory_order_relaxed);
    calls[slot].fetch_add(1, std::memory_order_relaxed);
  }
};

inline void mark(const char *label) {
  static uint64_t prev = now_ns();
  const uint64_t t = now_ns();
  std::fprintf(stderr, "[prof] main %-28s %10.3f s\n", label, (t - prev) / 1e9);
  prev = t;
}

inline void report() {
  const double total = ns[block1].load() / 1e9;
  std::fprintf(stderr, "[prof] %-30s %14s %12s %8s\n", "slot (thread time, all threads)", "seconds",
               "calls", "% stage1");
  for (int i = 0; i < num_slots; ++i) {
    const double s = ns[i].load() / 1e9;
    std::fprintf(stderr, "[prof] %-30s %14.3f %12llu %7.2f%%\n", slot_names[i], s,
                 (unsigned long long)calls[i].load(), total > 0 ? 100.0 * s / total : 0.0);
  }
}

}  // namespace prof

#define PROF_CAT2(a, b) a##b
#define PROF_CAT(a, b) PROF_CAT2(a, b)
#define PROF_SCOPE(slot) prof::Scope PROF_CAT(prof_scope_, __LINE__)(prof::slot)
#define PROF_MARK(label) prof::mark(label)
#define PROF_REPORT() prof::report()
#else
#define PROF_SCOPE(slot)
#define PROF_MARK(label)
#define PROF_REPORT()
#endif

#endif  // LOGMINIFY_PROFILE_H
