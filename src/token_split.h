// Word splitting for variable_extract(), without std::regex. The delimiters that
// delimiter_mining() picks are single characters; a line splits into each
// delimiter character as its own token and the maximal runs of other characters
// in between.
#ifndef LOGMINIFY_TOKEN_SPLIT_H
#define LOGMINIFY_TOKEN_SPLIT_H

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct DelimiterSet {
  bool is_delim[256] = {};
};

// Each delimiter must be one character.
inline DelimiterSet make_delimiter_set(const std::vector<std::string> &delimiters) {
  DelimiterSet set;
  for (const auto &d : delimiters) {
    if (d.size() != 1) throw std::runtime_error("delimiter is not one character: " + d);
    set.is_delim[static_cast<unsigned char>(d[0])] = true;
  }
  return set;
}

// Appends the tokens of `line` to `out` (views into `line`).
inline void split_keep_delimiters(const DelimiterSet &set, std::string_view line,
                                  std::vector<std::string_view> &out) {
  size_t word_start = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    if (!set.is_delim[static_cast<unsigned char>(line[i])]) continue;
    if (i > word_start) out.push_back(line.substr(word_start, i - word_start));
    out.push_back(line.substr(i, 1));
    word_start = i + 1;
  }
  if (word_start < line.size()) out.push_back(line.substr(word_start));
}

// Whether `s` contains 0-9, as std::regex_search(s, std::regex("\\d")) does.
inline bool has_ascii_digit(std::string_view s) {
  for (char c : s)
    if (c >= '0' && c <= '9') return true;
  return false;
}

#endif  // LOGMINIFY_TOKEN_SPLIT_H
