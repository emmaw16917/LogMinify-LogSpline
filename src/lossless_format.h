// Byte-exact restoration helpers shared by logminify and decompress_logminify.
//
// Escaping: placeholders start with '<', so a raw '<' is written as 0x01 0x03 and a
// raw 0x01 as 0x01 0x02 before any processing (escape_line), and turned back after
// every placeholder is replaced (unescape_line).
//
// Shapes: a regex tag (<I>, <T>, <D>, ...) stores the digits of its match as one
// number; the match's shape keeps everything else: its literal characters, with
// each digit run written as 0x02 and the run's width (as 0x20 + width; for <I>,
// whose parts are padded to 3 digits, width 0 means the part has no leading zero,
// so its width follows from the value). The decompressor rebuilds the match from
// the number and the shape (apply_shape).
//
// Timestamps: each of a dataset's header timestamps (TsSpec) is stored as one number
// per line in its own column (<TS>, <TSB>, ...), delta encoded: an epoch as is,
// other formats as the mixed-radix count ((((Y*13+month)*32+day)*24+h)*60+m)*60+s,
// times 1000 or 10^6 plus the fraction if the format has milliseconds or
// microseconds. A timestamp is replaced only if formatting the number back gives the
// same bytes.
#ifndef LOGMINIFY_LOSSLESS_FORMAT_H
#define LOGMINIFY_LOSSLESS_FORMAT_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

inline std::string escape_line(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char c : s) {
    if (c == '<') out += "\x01\x03";
    else if (c == '\x01') out += "\x01\x02";
    else out += c;
  }
  return out;
}

inline void unescape_line(std::string &s) {
  if (s.find('\x01') == std::string::npos) return;
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x01' && i + 1 < s.size() && (s[i + 1] == '\x03' || s[i + 1] == '\x02')) {
      out += s[i + 1] == '\x03' ? '<' : '\x01';
      ++i;
    } else {
      out += s[i];
    }
  }
  s.swap(out);
}

// Number contexts: a num tag's numbers are stored grouped by context (contexts in
// order of first appearance in the block, numbers in block order within one), the
// context of a number being the nearest run of letters before it, skipping digits,
// other characters and tags ("<...>"). The compressor computes it on the line as
// tagged so far, the decompressor on the line with its variables restored and its
// regex tags not yet: both see the same letters.
inline std::string_view num_context(std::string_view s, size_t pos) {
  size_t i = pos;
  while (i > 0) {
    const char c = s[i - 1];
    if (c == '>') {  // a tag: skip to its '<'
      const size_t lt = s.rfind('<', i - 1);
      if (lt == std::string_view::npos) break;
      i = lt;
      continue;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) break;
    --i;
  }
  size_t j = i;
  while (j > 0 && ((s[j - 1] >= 'a' && s[j - 1] <= 'z') || (s[j - 1] >= 'A' && s[j - 1] <= 'Z'))) --j;
  return s.substr(j, i - j);
}

// match -> its shape, and the digits it stores (each run padded to 3 digits if
// pad3, as <I> does); false if the match cannot be stored (a run over 3 digits with
// pad3, or more than 18 digits in all), in which case it is kept as text.
inline bool shape_of(std::string_view match, bool pad3, std::string &shape, std::string &digits) {
  shape.clear();
  digits.clear();
  for (size_t k = 0; k < match.size();) {
    if (match[k] >= '0' && match[k] <= '9') {
      size_t j = k;
      while (j < match.size() && match[j] >= '0' && match[j] <= '9') ++j;
      const size_t w = j - k;
      bool natural = false;  // pad3: the width follows from the value (no leading zero)
      if (pad3) {
        if (w > 3) return false;
        digits.append(3 - w, '0');
        natural = w == 1 || match[k] != '0';
      }
      digits.append(match.substr(k, w));
      shape += '\x02';
      shape += char(0x20 + (natural ? 0 : w));
      k = j;
    } else {
      shape += match[k++];
    }
  }
  return !digits.empty() && digits.size() <= 18;
}

// The text a shape and its stored number stand for.
inline std::string apply_shape(const std::string &shape, int64_t value, bool pad3) {
  size_t total = 0;
  for (size_t k = 0; k < shape.size(); ++k)
    if (shape[k] == '\x02' && k + 1 < shape.size()) {
      total += pad3 ? 3 : size_t(shape[k + 1] - 0x20);
      ++k;
    }
  std::string digits = std::to_string(value < 0 ? -value : value);
  if (digits.size() < total) digits.insert(0, total - digits.size(), '0');
  std::string out;
  size_t pos = 0;
  for (size_t k = 0; k < shape.size(); ++k) {
    if (shape[k] == '\x02' && k + 1 < shape.size()) {
      size_t w = size_t(shape[k + 1] - 0x20);
      if (pad3) {
        if (w == 0) {  // natural width: the part without its padding zeros
          w = 3;
          while (w > 1 && digits[pos + 3 - w] == '0') --w;
        }
        out.append(digits, pos + 3 - w, w);
        pos += 3;
      } else {
        out.append(digits, pos, w);
        pos += w;
      }
      ++k;
    } else {
      out += shape[k];
    }
  }
  return out;
}

// Header timestamp format: literal characters and %Y (4 digits), %y (2 digits, year
// 2000+), %m %d %H %M %S (2 digits), %e (day, space padded to 2), %f (3-digit
// milliseconds), %u (6-digit microseconds), %b (month abbreviation), %a (weekday
// abbreviation, checked against the date; needs %Y or %y). %-m %-d %-H %-M %-S %-f:
// the same without padding (1 or 2 digits; %-f 1 to 3). %E alone: an epoch, an
// integer without leading zeros, stored as is. The timestamp starts after `skip`
// words of the line (each followed by one space), counted on the line as the
// dataset's earlier timestamps left it.
struct TsSpec {
  const char *fmt;
  int skip = 0;
  bool epoch() const { return std::strcmp(fmt, "%E") == 0; }
  int64_t unit() const {  // fractions of a second the number counts
    if (std::strstr(fmt, "%u") != nullptr) return 1000000;
    if (std::strstr(fmt, "%f") != nullptr || std::strstr(fmt, "%-f") != nullptr) return 1000;
    return 1;
  }
};
struct TsSpecs {
  const TsSpec *spec = nullptr;
  int n = 0;
};

// A dataset's timestamps, in the order they are replaced; the i-th is tag <TS>,
// <TSB>, <TSC>, ...
inline TsSpecs ts_specs_for(const std::string &logname) {
  static const TsSpec android[] = {{"%m-%d %H:%M:%S.%f"}}, apache[] = {{"[%a %b %d %H:%M:%S %Y]"}},
      bgl[] = {{"%E", 1}, {"%Y.%m.%d", 2}, {"%Y-%m-%d-%H.%M.%S.%u", 4}}, hdfs[] = {{"%y%m%d %H%M%S"}},
      hpc[] = {{"%E", 4}}, hadoop[] = {{"%Y-%m-%d %H:%M:%S,%f"}}, healthapp[] = {{"%Y%-m%-d-%-H:%-M:%-S:%-f"}},
      syslog[] = {{"%b %e %H:%M:%S"}}, openstack[] = {{"%Y-%m-%d %H:%M:%S.%f", 1}}, proxifier[] = {{"[%m.%d %H:%M:%S]"}},
      spark[] = {{"%y/%m/%d %H:%M:%S"}}, thunderbird[] = {{"%E", 1}, {"%Y.%m.%d", 2}, {"%b %-d %H:%M:%S", 4}},
      windows[] = {{"%Y-%m-%d %H:%M:%S"}}, zookeeper[] = {{"%Y-%m-%d %H:%M:%S,%f"}};
  static const struct { const char *name; TsSpecs specs; } table[] = {
      {"Android", {android, 1}},     {"Apache", {apache, 1}}, {"BGL", {bgl, 3}},           {"HDFS", {hdfs, 1}},
      {"HPC", {hpc, 1}},             {"Hadoop", {hadoop, 1}}, {"HealthApp", {healthapp, 1}}, {"Linux", {syslog, 1}},
      {"Mac", {syslog, 1}},          {"OpenSSH", {syslog, 1}}, {"OpenStack", {openstack, 1}}, {"Proxifier", {proxifier, 1}},
      {"Spark", {spark, 1}},         {"Thunderbird", {thunderbird, 3}}, {"Windows", {windows, 1}}, {"Zookeeper", {zookeeper, 1}},
  };
  for (const auto &e : table)
    if (logname == e.name) return e.specs;
  return {};
}

// The tag of a dataset's i-th timestamp, without '<' '>': TS, TSB, TSC, ...
inline std::string ts_tag(int i) { return i == 0 ? std::string("TS") : std::string("TS") + char('A' + i); }
// i for a tag name of ts_tag, -1 if it is none
inline int ts_index(const std::string &tag) {
  if (tag == "TS") return 0;
  if (tag.size() == 3 && tag[0] == 'T' && tag[1] == 'S' && tag[2] >= 'B' && tag[2] <= 'Z') return tag[2] - 'A';
  return -1;
}
// Where the timestamp of spec starts in line: after spec.skip words, each followed
// by one space; npos if the line has fewer.
inline size_t ts_start(const TsSpec &spec, std::string_view line) {
  size_t pos = 0;
  for (int k = 0; k < spec.skip; ++k) {
    const size_t sp = line.find(' ', pos);
    if (sp == std::string_view::npos || sp == pos) return std::string_view::npos;
    pos = sp + 1;
  }
  return pos;
}

namespace ts_detail {
inline const char *kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
inline const char *kDay[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
inline int weekday(int y, int m, int d) {  // Sakamoto, 0 = Sunday
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
struct Fields {
  int Y = 0, mo = 1, d = 1, H = 0, M = 0, S = 0, f = 0;
  int64_t epoch = 0;
};
inline bool num(std::string_view s, size_t i, size_t w, int &v) {
  if (i + w > s.size()) return false;
  v = 0;
  for (size_t k = 0; k < w; ++k) {
    const char c = s[i + k];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  return true;
}
inline int *field(Fields &t, char code) {
  switch (code) {
    case 'm': return &t.mo;
    case 'd': return &t.d;
    case 'H': return &t.H;
    case 'M': return &t.M;
    case 'S': return &t.S;
    case 'f': return &t.f;
    default: return nullptr;
  }
}
// Parse line[i..] by fmt (backtracking over the widths of unpadded fields); the
// end of the timestamp in `end`.
inline bool parse(const char *p, std::string_view line, size_t i, Fields &t, size_t &end) {
  for (; *p; ++p) {
    if (*p != '%') {
      if (i >= line.size() || line[i] != *p) return false;
      ++i;
      continue;
    }
    ++p;
    if (*p == '-') {  // unpadded: try the widest width first
      const char code = *++p;
      int *v = field(t, code);
      if (v == nullptr) return false;
      for (size_t w = code == 'f' ? 3 : 2; w >= 1; --w) {
        if (num(line, i, w, *v) && (w == 1 || line[i] != '0') && parse(p + 1, line, i + w, t, end)) return true;
      }
      return false;
    }
    switch (*p) {
      case 'Y': if (!num(line, i, 4, t.Y)) return false; i += 4; break;
      case 'y': if (!num(line, i, 2, t.Y)) return false; t.Y += 2000; i += 2; break;
      case 'm': case 'd': case 'H': case 'M': case 'S':
        if (!num(line, i, 2, *field(t, *p))) return false;
        i += 2;
        break;
      case 'f': if (!num(line, i, 3, t.f)) return false; i += 3; break;
      case 'u': if (!num(line, i, 6, t.f)) return false; i += 6; break;
      case 'E': {  // digits, no leading zero, at most 18
        size_t j = i;
        while (j < line.size() && j - i < 18 && line[j] >= '0' && line[j] <= '9') ++j;
        if (j == i || (line[i] == '0' && j - i > 1)) return false;
        t.epoch = 0;
        for (size_t k = i; k < j; ++k) t.epoch = t.epoch * 10 + (line[k] - '0');
        i = j;
        break;
      }
      case 'e':
        if (i + 2 > line.size()) return false;
        if (line[i] == ' ') {
          if (!num(line, i + 1, 1, t.d)) return false;
        } else if (!num(line, i, 2, t.d)) {
          return false;
        }
        i += 2;
        break;
      case 'b': {
        if (i + 3 > line.size()) return false;
        int k = 0;
        while (k < 12 && line.compare(i, 3, kMon[k]) != 0) ++k;
        if (k == 12) return false;
        t.mo = k + 1;
        i += 3;
        break;
      }
      case 'a': if (i + 3 > line.size()) return false; i += 3; break;  // checked by the round trip
      default: return false;
    }
  }
  end = i;
  return true;
}
}  // namespace ts_detail

inline std::string ts_format(const TsSpec &spec, int64_t value) {
  using namespace ts_detail;
  if (spec.epoch()) return std::to_string(value);
  int f = 0;
  const int64_t unit = spec.unit();
  if (unit > 1) {
    f = int(value % unit);
    value /= unit;
  }
  const int S = int(value % 60); value /= 60;
  const int M = int(value % 60); value /= 60;
  const int H = int(value % 24); value /= 24;
  const int d = int(value % 32); value /= 32;
  const int mo = int(value % 13); value /= 13;
  const int Y = int(value);
  std::string out;
  char buf[16];
  for (const char *p = spec.fmt; *p; ++p) {
    if (*p != '%') { out += *p; continue; }
    ++p;
    const bool unpadded = *p == '-';
    if (unpadded) ++p;
    int v = 0;
    switch (*p) {
      case 'm': v = mo; break;
      case 'd': v = d; break;
      case 'H': v = H; break;
      case 'M': v = M; break;
      case 'S': v = S; break;
      case 'f': v = f; break;
      default: break;
    }
    switch (*p) {
      case 'Y': std::snprintf(buf, sizeof(buf), "%04d", Y); break;
      case 'y': std::snprintf(buf, sizeof(buf), "%02d", Y - 2000); break;
      case 'm': case 'd': case 'H': case 'M': case 'S':
        std::snprintf(buf, sizeof(buf), unpadded ? "%d" : "%02d", v);
        break;
      case 'f': std::snprintf(buf, sizeof(buf), unpadded ? "%d" : "%03d", v); break;
      case 'u': std::snprintf(buf, sizeof(buf), "%06d", f); break;
      case 'e': std::snprintf(buf, sizeof(buf), "%2d", d); break;
      case 'b': std::snprintf(buf, sizeof(buf), "%s", (mo >= 1 && mo <= 12) ? kMon[mo - 1] : "???"); break;
      case 'a': std::snprintf(buf, sizeof(buf), "%s", (mo >= 1 && mo <= 12) ? kDay[weekday(Y, mo, d)] : "???"); break;
      default: buf[0] = 0; break;
    }
    out += buf;
  }
  return out;
}

// The timestamp at the start of line: its number and length, if it round-trips.
inline bool ts_parse(const TsSpec &spec, std::string_view line, int64_t &value, size_t &len) {
  ts_detail::Fields t;
  if (!ts_detail::parse(spec.fmt, line, 0, t, len)) return false;
  if (spec.epoch()) {
    value = t.epoch;
  } else {
    if (t.mo < 1 || t.mo > 12 || t.d < 1 || t.d > 31 || t.H > 23 || t.M > 59 || t.S > 59) return false;
    value = (((((int64_t(t.Y) * 13 + t.mo) * 32 + t.d) * 24 + t.H) * 60 + t.M) * 60 + t.S) * spec.unit() + t.f;
  }
  return ts_format(spec, value) == line.substr(0, len);
}

#endif
