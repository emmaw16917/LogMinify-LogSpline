// RSSPDC as LogMinify's global template dictionary: a radix string spline on
// discriminative bytes with native concurrent inserts (third_party/rsspdc, see its
// README). It maps each template's key to the template's ID.
//
// - A template with NUL or 0x01 bytes is escaped (escape_nul in dict_dump_format.h);
//   keys keep the templates' byte order. RSSPDC reads past a key's end as 0, so a
//   key must not contain NUL for its bytes at the chosen positions to order it.
// - RSSPDC::get_or_insert() allocates a new template's ID under the lock of the
//   node the key reaches, so concurrent calls agree on one ID and IDs stay dense.
// - The dictionary is dumped front coded (dict_dump_format.h) from RSSPDC::scan().
// - Rebuilds run on a background thread; dump() stops it first.
#ifndef LOGMINIFY_RSSPDC_TEMPLATE_INDEX_H
#define LOGMINIFY_RSSPDC_TEMPLATE_INDEX_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dict_dump_format.h"
#include "rsspdc.hpp"

class RsspdcTemplateIndex {
 public:
  using AllocId = uint64_t (*)();

  // An empty index.
  explicit RsspdcTemplateIndex(AllocId alloc_id) : RsspdcTemplateIndex(alloc_id, {}) {}

  // The index of `sorted`: distinct templates in byte order, which get IDs from
  // alloc_id() in that order, as the other indexes give them when the templates
  // are inserted one by one.
  RsspdcTemplateIndex(AllocId alloc_id, const std::vector<std::string> &sorted)
      : index_(alloc_id, keys_of(sorted)) {}
  RsspdcTemplateIndex(const RsspdcTemplateIndex &) = delete;
  RsspdcTemplateIndex &operator=(const RsspdcTemplateIndex &) = delete;

  // ID of `tmpl`, inserting it with a new ID if absent. Concurrent calls with the
  // same template agree on one ID.
  uint64_t get_or_insert(const std::string &tmpl) {
    if (!needs_nul_escape(tmpl)) {
      if (const uint64_t id = index_.lookup(tmpl)) return id;
      check_template(tmpl);
      return index_.get_or_insert(tmpl);
    }
    const std::string key = escape_nul(tmpl);
    if (const uint64_t id = index_.lookup(key)) return id;
    check_template(tmpl);
    return index_.get_or_insert(key);
  }

  // The calling thread leaves the index until its next call (it no longer holds
  // back the freeing of replaced subtrees); see RSSPDC::thread_offline().
  void thread_offline() const { index_.thread_offline(); }

  // Write the dictionary: the templates in byte order, front coded
  // (dict_dump_format.h), and their IDs in the same order in ids_path. Call only
  // while no thread modifies the index; stops the background rebuilds.
  bool dump(const std::string &text_path, const std::string &ids_path) {
    if (ids_path.empty()) return false;
    index_.stop_background();
    std::vector<std::string> templates;  // in byte order (escaping keeps it)
    std::vector<uint64_t> ids;           // in the same order
    for (auto &kv : index_.scan()) {
      std::string tmpl;
      if (!unescape_nul(kv.first, tmpl)) return false;
      templates.push_back(std::move(tmpl));
      ids.push_back(kv.second);
    }
    std::ofstream out(text_path, std::ios::binary);
    if (!out) return false;
    std::string_view prev;
    for (const auto &t : templates) {
      write_front_coded(out, prev, t);
      prev = t;
    }
    if (!out) return false;
    std::ofstream ids_out(ids_path, std::ios::binary);
    write_delta_ids(ids_out, ids);
    return static_cast<bool>(ids_out);
  }

 private:
  static std::vector<std::string> keys_of(const std::vector<std::string> &sorted) {
    std::vector<std::string> keys;
    keys.reserve(sorted.size());
    for (const auto &t : sorted) {
      check_template(t);
      keys.push_back(needs_nul_escape(t) ? escape_nul(t) : t);
    }
    return keys;
  }

  // The dump is line-based, so a template must not contain '\n' (templates are log
  // lines without their newline anyway).
  static void check_template(const std::string &tmpl) {
    if (std::memchr(tmpl.data(), '\n', tmpl.size()) != nullptr) {
      std::cerr << "RsspdcTemplateIndex: template contains a newline byte" << std::endl;
      std::abort();
    }
  }

  RSSPDC index_;
};

#endif  // LOGMINIFY_RSSPDC_TEMPLATE_INDEX_H
