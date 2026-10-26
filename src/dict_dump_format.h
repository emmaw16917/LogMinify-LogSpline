// The template dictionary is stored as two files (front-coded templates + IDs):
//   <name>_fc.txt   the templates in byte order, front coded: a line
//                   "<n>\t<rest>" is the first n bytes of the previous template,
//                   then <rest>; a template or <rest> runs to the end of its line,
//                   so it may contain tabs or parentheses (no newline).
//   <name>_ids.bin  the IDs, in the same order, each as the zigzag varint of its
//                   difference from the previous ID.
// The escaping helpers keep index keys (NUL and 0x01) intact and order-preserving.
#ifndef LOGMINIFY_DICT_DUMP_FORMAT_H
#define LOGMINIFY_DICT_DUMP_FORMAT_H

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


inline bool needs_nul_escape(std::string_view s) {
  return s.find('\0') != std::string_view::npos || s.find('\x01') != std::string_view::npos;
}

inline std::string escape_nul(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\0' || c == '\x01') {
      out.push_back('\x01');
      out.push_back(c == '\0' ? '\x01' : '\x02');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Inverse of escape_nul(); false if `s` is not an escaped template.
inline bool unescape_nul(std::string_view s, std::string &out) {
  out.clear();
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\x01') {
      out.push_back(s[i]);
      continue;
    }
    if (i + 1 == s.size() || (s[i + 1] != '\x01' && s[i + 1] != '\x02')) return false;
    out.push_back(s[++i] == '\x01' ? '\0' : '\x01');
  }
  return true;
}

inline void write_delta_ids(std::ostream &out, const std::vector<uint64_t> &ids) {
  uint64_t prev = 0;
  for (uint64_t id : ids) {  // zigzag varint of the difference from the previous ID
    const int64_t delta = static_cast<int64_t>(id - prev);
    prev = id;
    uint64_t code = (static_cast<uint64_t>(delta) << 1) ^ static_cast<uint64_t>(delta >> 63);
    while (code >= 0x80) {
      out.put(static_cast<char>((code & 0x7F) | 0x80));
      code >>= 7;
    }
    out.put(static_cast<char>(code));
  }
}

// Contents of an ID file -> the IDs.
inline std::vector<uint64_t> decode_delta_ids(const std::string &bytes) {
  std::vector<uint64_t> ids;
  uint64_t code = 0, prev = 0;
  int shift = 0;
  for (unsigned char b : bytes) {
    code |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (b & 0x80) {
      shift += 7;
      continue;
    }
    prev += static_cast<uint64_t>(static_cast<int64_t>(code >> 1) ^ -static_cast<int64_t>(code & 1));
    ids.push_back(prev);
    code = 0;
    shift = 0;
  }
  return ids;
}

// Writes the front-coded line of `cur`, which follows `prev`.
inline void write_front_coded(std::ostream &out, std::string_view prev, std::string_view cur) {
  size_t shared = 0;
  while (shared < prev.size() && shared < cur.size() && prev[shared] == cur[shared]) ++shared;
  out << shared << '\t';
  out.write(cur.data() + shared, cur.size() - shared);
  out << '\n';
}

// Adds every template of a front-coded dump to id_to_template, with the IDs taken
// from `ids` in order. Returns false on a malformed dump, a duplicate ID, or a
// line count that does not match `ids`.
inline bool read_fc_dump(std::istream &in, std::unordered_map<uint64_t, std::string> &id_to_template,
                         const std::vector<uint64_t> &ids) {
  std::string prev, line;
  size_t next_id = 0;
  while (std::getline(in, line)) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || next_id >= ids.size()) return false;
    const size_t shared = std::stoul(line.substr(0, tab));
    if (shared > prev.size()) return false;
    std::string tmpl = prev.substr(0, shared) + line.substr(tab + 1);
    if (!id_to_template.emplace(ids[next_id++], tmpl).second) return false;
    prev = std::move(tmpl);
  }
  return next_id == ids.size();
}

#endif  // LOGMINIFY_DICT_DUMP_FORMAT_H
