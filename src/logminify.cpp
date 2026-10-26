#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <fstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "helper.h"
#include "rsspdc_template_index.h"
#include "profile.h"
#include "token_split.h"
#include "block_reader.h"
#include "lossless_format.h"

#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <sys/stat.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <pcre2.h>
#include <stdexcept>
#include <future>
#include <filesystem>
#include <cstdlib>


// parameters
size_t fg_n = std::max(1u, std::thread::hardware_concurrency());  // LOGMINIFY_FG_THREADS overrides it

std::atomic<uint64_t> key_cnt(0);
uint64_t key_cnt_increment() {
    return key_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
}

#define PCRE2_CODE_UNIT_WIDTH 8

// An integer setting from the environment variable `name` if set, else `value`;
// aborts on anything but 0..max.
size_t env_setting(const char *name, size_t value, size_t max) {
  if (const char *env = std::getenv(name)) {
    char *end = nullptr;
    const unsigned long v = std::strtoul(env, &end, 10);
    if (end == env || *end != '\0' || v > max) {
      COUT_N_EXIT("invalid " << name << "=" << env << " (expected 0.." << max << ")");
    }
    value = v;
  }
  return value;
}

// fg_n from LOGMINIFY_FG_THREADS (1..1024) if set: how many blocks are compressed
// at once.
void configure_workers() {
  fg_n = env_setting("LOGMINIFY_FG_THREADS", fg_n, 1024);
  if (fg_n == 0) {
    COUT_N_EXIT("invalid LOGMINIFY_FG_THREADS=0 (expected 1..1024)");
  }
  std::cout << "Worker threads: " << fg_n << std::endl;
}


RsspdcTemplateIndex *rsspdc_index = nullptr;

// Compression backend for block archives and the dictionary archive.
// Overridable with LOGMINIFY_COMPRESS, e.g. "gzip -9", "bzip2 -3", "lzma -1".
std::string g_compress_prog = "lzma -6";

// The dictionary: all templates in byte order, front coded
// (dict_dump_format.h), with the IDs in a separate file, packed into one archive.
std::string dict_files(const std::string &name) {
  return name + "_fc.txt " + name + "_ids.bin";
}

template <class Index>
bool dump_dictionary(Index &index, const std::string &name) {
  return index.dump(name + "_fc.txt", name + "_ids.bin");
}


// After a block's templates: this worker will not resolve templates for a while
// (it parses and compresses), so it must not hold back RSSPDC's reclamation.
void leave_template_index() {
  rsspdc_index->thread_offline();
}


// Template -> ID through the RSSPDC index.
uint64_t resolve_template(const std::string &tmpl) {
  PROF_SCOPE(index);
  return rsspdc_index->get_or_insert(tmpl);
}

// Elastic coding (zigzag varints) for numbers and IDs, as in LogReducer/LogShrink.
int64_t zigzag_encode(int64_t num) {
    return (num << 1) ^ (num >> 63);
}

// zigzag_encode(num) appended to out as a varint
void elastic_encode_append(int64_t num, std::string &out) {
    uint64_t cur = zigzag_encode(num);
    while (cur >= 0x80) {
        out.push_back(static_cast<char>((cur & 0x7F) | 0x80));
        cur >>= 7;
    }
    out.push_back(static_cast<char>(cur));
}

// The numbers of one tag: delta-transformed unless the tag is <I>, <a>, <b> or
// <c>, elastic encoded, as one write.
void write_numbers(const std::string &filename, const std::string &tag, const std::vector<int64_t> &nums) {
    std::ofstream file(filename, std::ios::out | std::ios::binary);
    std::string bytes;
    bytes.reserve(nums.size() * 3);
    if (tag != "<I>" && tag != "<a>" && tag != "<b>" && tag != "<c>") {
        // delta coding for tags without an arithmetic relationship
        int64_t last = 0;
        for (size_t i = 0; i < nums.size(); ++i) {
            elastic_encode_append(i == 0 ? nums[0] : nums[i] - last, bytes);
            last = nums[i];
        }
    } else {
        for (int64_t num : nums) elastic_encode_append(num, bytes);
    }
    file.write(bytes.data(), bytes.size());
}

// A num tag's numbers grouped by context (stable: contexts in ID order, numbers in
// block order within one), so that delta encoding runs within a context.
std::vector<int64_t> group_by_context(const std::vector<int64_t> &nums, const std::vector<uint32_t> &ctx) {
    if (ctx.size() != nums.size()) return nums;
    std::vector<size_t> order(nums.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ctx[a] < ctx[b]; });
    std::vector<int64_t> out;
    out.reserve(nums.size());
    for (size_t i : order) out.push_back(nums[i]);
    return out;
}

// A block's shapes (lossless_format.h), one per line by ID; nothing if none.
void write_shapes(const std::string &filename, const std::vector<std::string> &shapes) {
    if (shapes.empty()) return;
    std::string text;
    for (const auto &s : shapes) text.append(s).push_back('\n');
    std::ofstream(filename, std::ios::binary).write(text.data(), text.size());
}


// Replacing pure numbers and structured tokens (IPs, timestamps, sizes) with tags.
struct RegexPattern {
    std::vector<std::string> patterns;
    std::vector<std::string> substitutions;
};

// LogProcessor class
class LogProcessor {
public:
    std::string logname;
    std::unordered_map<std::string, RegexPattern> regex_map;
    std::vector<pcre2_code *> compiled_patterns; // Used to store the compiled regular expression.
    pcre2_code *re_num;
    // Constructor.
    LogProcessor(const std::string &name) : logname(name) {
        // Predefined regular expressions and replacement symbols for different log names. You can modified these based on your systems.
        compile_num(&re_num, R"((?<![a-zA-Z0-9])\d+(?![a-zA-Z0-9]))");
        regex_map["Android"] = { {R"((\d+)\.(\d+)\.(\d+)\.(\d+))", R"((\d+)-(\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)"}, {"<I>", "<T>"} };
        regex_map["Apache"] = { {R"((\d+)\.(\d+)\.(\d+)\.(\d+))", R"((\d{2}) (\d+):(\d+):(\d+))"}, {"<I>", "<T>"} };
        regex_map["BGL"] = { {R"((\d+)-(\d+)-(\d+)-(\d+)\.(\d+)\.(\d+))", R"((\d+):(\d+):(\d+))",R"((\d+)\.(\d+)\.(\d+))"}, {"<E>", "<T>", "<F>"} };
        regex_map["Hadoop"] = { {R"((\d+)\-(\d+)\-(\d+))", R"((\d+):(\d+):(\d+),(\d+))"}, {"<D>", "<T>"} };
        regex_map["HDFS"] = { { R"((\d+)\.(\d+)\.(\d+)\.(\d+))", R"((\d+):(\d+):(\d+),(\d+))"}, {"<I>", "<T>"} };
        regex_map["HealthApp"] = { { R"((\d+):(\d+):(\d+):(\d+))"}, {"<T>"} };
        regex_map["HPC"] = { {R"((\d+)\.(\d+)\.(\d+)\.(\d+))", R"((\d+)-(\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)"}, {"<I>", "<T>"} };
        regex_map["Linux"] = { {R"((\d+)\.(\d+)\.(\d+)\.(\d+))", R"((\d+)-(\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)"}, {"<I>", "<T>"} };
        regex_map["Mac"] = { {R"((\d+)-(\d+)-(\d+)-(\d+))", R"((\d+):(\d+):(\d+)(?:\.(\d+))?)"}, {"<D>", "<T>"} };
        regex_map["OpenSSH"] = { {R"((\d+)\.(\d+)\.(\d+)\.(\d+))", R"((\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)",R"(sshd\[(\d+)\]:)"}, {"<I>", "<T>", "<S>"} };
        regex_map["OpenStack"] = { { R"(\.(\d+)-(\d+)-(\d+)_(\d+):(\d+):(\d+))",R"((\d+)-(\d+)-(\d+).(\d+):(\d+):(\d+)\.(\d+))"}, { "<D>", "<T>"} };
        regex_map["Proxifier"] = { { R"((\d+)\.(\d+) (\d+):(\d+):(\d+)(?:\.(\d+))?)"}, {"<T>"} };
        regex_map["Spark"] = { { R"((\d+)\.(\d+)\.(\d+)\.(\d+))", R"((\d{2})\/(\d{2})\/(\d{2}) (\d+):(\d+):(\d+))",R"((\d+)\.(\d{1}) MB)",R"((\d+)\.(\d{1}) KB)",R"((\d+)\.(\d{1}) GB)",R"((\d+)\.(\d{1}) B)"}, {"<I>", "<T>", "<M>", "<K>", "<G>", "<B>"} };
        regex_map["Thunderbird"] = { { R"((\d+)\.(\d+)\.(\d+)\.(\d+))",R"((\d+):(\d+):(\d+))",R"((\d{4})\.(\d+)\.(\d+))"}, {"<I>","<T>","<A>"}};
        regex_map["Windows"] = { {  R"((\d+)\.(\d+)\.(\d+)\.(\d+))",R"((\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+))", R"((\d+):(\d+):(\d+))"}, {"<I>","<T>","<D>"} };
        regex_map["Zookeeper"] = { {  R"((\d+)\.(\d+)\.(\d+)\.(\d+))",R"((\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+),(\d+))", R"((\d+):(\d+):(\d+))"}, {"<I>","<T>","<D>"} };
        // Compile the regular expression based on the log name.
        if (regex_map.find(logname) != regex_map.end()) {
            const auto &patterns = regex_map[logname].patterns;
            for (const auto &pattern : patterns) {
                pcre2_code *re = compile_pattern(pattern.c_str());
                compiled_patterns.push_back(re);
            }
        } else {
            throw std::runtime_error("Unknown logname");
        }
    }

    // Destructor.
    ~LogProcessor() {
        // Release the compiled regular expression.
        for (auto re : compiled_patterns) {
            pcre2_code_free(re);
        }
        pcre2_code_free(re_num);
    }
    /*
    replace_and_group() : parse pure numbers and tokens containing only numbers and special characters 

    input: log list

    output: replaced log list, a unordered_map to store tag and corresponding numbers
    */

    // A block's lines with numbers replaced by tags, the numbers of each tag (and
    // <X>s: the shape IDs of regex tag <X>), and the block's shapes by ID - 1
    // (lossless_format.h).
    struct Grouped {
        std::vector<std::string> replaced;
        std::unordered_map<std::string, std::vector<int64_t>> patterns;
        std::vector<std::string> shapes;
        // per num tag, the context ID of each number (lossless_format.h; contexts
        // numbered by first appearance in the block)
        std::unordered_map<std::string, std::vector<uint32_t>> num_ctx;
    };

    Grouped replace_and_group(const std::vector<std::string> &lst) {
        Grouped g;
        auto &patterns = g.patterns;
        auto &replaced = g.replaced;

        const std::string alpha = "abcdefghijklmnopqrstuvwxyz";
        const auto &substitutions = regex_map[logname].substitutions;
        static const bool ts_on = [] { const char *e = std::getenv("LOGMINIFY_TS"); return e == nullptr || std::strcmp(e, "0") != 0; }();
        const TsSpecs ts = ts_on ? ts_specs_for(logname) : TsSpecs{};  // LOGMINIFY_TS=0: no timestamp columns
        std::vector<std::vector<int64_t> *> ts_nums(ts.n, nullptr);

        NumSlots num_slots{};
        Shapes shapes{g.shapes, {}};
        NumCtx ctx{&g.num_ctx, {}};
        replaced.reserve(lst.size());
        for (auto &item : lst) {
            std::string result = escape_line(item);
            for (int k = 0; k < ts.n; ++k) {  // the header timestamps, each as one number
                const size_t at = ts_start(ts.spec[k], result);
                int64_t tsv;
                size_t tsl;
                if (at == std::string::npos || !ts_parse(ts.spec[k], std::string_view(result).substr(at), tsv, tsl)) continue;
                const std::string tag = "<" + ts_tag(k) + ">";
                if (ts_nums[k] == nullptr) ts_nums[k] = &patterns[tag];
                ts_nums[k]->push_back(tsv);
                result.replace(at, tsl, tag);
            }
            for (size_t i = 0; i < compiled_patterns.size(); ++i) {
                result = process_with_pattern(result, compiled_patterns[i], substitutions[i], patterns, false, nullptr, &shapes);
            }
            replaced.push_back(process_with_pattern(result, re_num, alpha, patterns, true, &num_slots, nullptr, &ctx));
        }

        return g;
    }

private:
    // pcre2_match() runs the JIT code when there is some and interprets otherwise,
    // with the same results, so a pattern the JIT cannot handle still works.
    static void jit_compile(pcre2_code *re) {
        static std::once_flag warned;
        int rc = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
        if (rc != 0) std::call_once(warned, [rc] { std::cerr << "pcre2 JIT unavailable (" << rc << "), matching without it" << std::endl; });
    }
    // One match data per thread, for every pattern (they have at most 7 groups).
    static pcre2_match_data *thread_match_data() {
        thread_local std::unique_ptr<pcre2_match_data, void (*)(pcre2_match_data *)> md(
            pcre2_match_data_create(16, nullptr), pcre2_match_data_free);
        return md.get();
    }
    pcre2_code* compile_pattern(const char *pattern) {
        int errornumber;
        PCRE2_SIZE erroroffset;
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, 0, &errornumber, &erroroffset, nullptr);
        if (re == nullptr) {
            throw std::runtime_error("Regex compilation failed");
        }
        jit_compile(re);
        return re;
    }
    void compile_num(pcre2_code **re, const char *pattern) {
        int errornumber;
        PCRE2_SIZE erroroffset;
        *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, 0, &errornumber, &erroroffset, nullptr);
        if (*re == nullptr) {
            throw std::runtime_error("Regex compilation failed");
        }
        jit_compile(*re);
    }
    // The numbers of one tag, for the num pass: <L> or <LF> by length L and first
    // digit F (lengths 1-14); filled lazily, pointers into `patterns` (stable).
    using NumSlots = std::array<std::vector<int64_t> *, 14 * 10>;
    // A block's shapes: by ID - 1, and their IDs.
    struct Shapes {
        std::vector<std::string> &list;
        std::unordered_map<std::string, int64_t> ids;
        int64_t id(const std::string &s) {
            auto it = ids.try_emplace(s, int64_t(list.size()) + 1).first;
            if (it->second == int64_t(list.size()) + 1) list.push_back(s);
            return it->second;
        }
    };

    // The block's contexts and the per-tag context IDs.
    struct NumCtx {
        std::unordered_map<std::string, std::vector<uint32_t>> *of;
        std::unordered_map<std::string, uint32_t> ids;
    };

    std::string process_with_pattern(const std::string &input, pcre2_code *re, const std::string &substitution, std::unordered_map<std::string, std::vector<int64_t>> &patterns, bool is_num = false, NumSlots *num_slots = nullptr, Shapes *shapes = nullptr, NumCtx *ctx = nullptr) {
        std::string result;
        PCRE2_SIZE last_pos = 0;

        PCRE2_SPTR subject = (PCRE2_SPTR)input.c_str();
        size_t subject_length = strlen((char *)subject);

        pcre2_match_data *match_data = thread_match_data();
        std::vector<int64_t> *tag_nums = nullptr;  // patterns[substitution], when first needed

        int rc;
        while ((rc = pcre2_match(re, subject, subject_length, last_pos, 0, match_data, nullptr)) > 0) {
            PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
            const std::string_view match(input.data() + ovector[0], ovector[1] - ovector[0]);
            if (result.empty()) result.reserve(input.size() + 16);
            result.append(input, last_pos, ovector[0] - last_pos);

            if (is_num) {
                // a run of ASCII digits (\d without UTF), so below 15 digits it fits
                const size_t len = match.length();
                if (len < 15) {
                    const size_t slot = (len - 1) * 10 + (len >= 4 ? match[0] - '0' : 0);
                    char key[5] = {'<', substitution[len - 1], 0, 0, 0};
                    size_t key_len = 2;
                    if (len >= 4) key[key_len++] = substitution[match[0] - '0'];
                    key[key_len++] = '>';
                    std::vector<int64_t> *&nums = (*num_slots)[slot];
                    if (nums == nullptr) nums = &patterns[std::string(key, key_len)];
                    int64_t v = 0;
                    for (char c : match) v = v * 10 + (c - '0');
                    nums->push_back(v);
                    if (ctx != nullptr) {
                        const std::string c(num_context(result, result.size()));
                        const uint32_t id = ctx->ids.try_emplace(c, uint32_t(ctx->ids.size())).first->second;
                        (*ctx->of)[std::string(key, key_len)].push_back(id);
                    }
                    result.append(key, key_len);
                } else {
                    result.append(match);
                }
            } else {
                // the match's digits as one number (<I>: each part padded to 3
                // digits) and its shape; a match that does not fit stays text, and
                // the num pass takes its numbers
                thread_local std::string shape, digits;
                if (shape_of(match, substitution == "<I>", shape, digits)) {
                    if (tag_nums == nullptr) tag_nums = &patterns[substitution];
                    tag_nums->push_back(std::stoll(digits));
                    patterns[substitution + "s"].push_back(shapes->id(shape));
                    result.append(substitution);
                } else {
                    result.append(match);
                }
            }
            last_pos = ovector[1];
        }
        if (last_pos == 0) return input;  // no match
        result.append(input, last_pos, std::string::npos);
        return result;
    }
};


void ensure_directory_exists(const std::string &dir);

// Extracting numeric variables and storing each block's variable and template IDs.
class DenumLogProcessor {
private:
    std::string logname;

public:
    DenumLogProcessor(std::string name) : logname(name) {}
// Splits each line at the mined delimiters, replaces every token containing a
// digit with <*>, and stores the token list as the block's variable set.
    std::vector<std::string> variable_extract(const std::vector<std::string>& logs, const std::string& chunkID) {
        std::vector<std::string> modified_lines;
        std::vector<std::string> variable_set;
        const DelimiterSet delimiters = delimiter_mining(logs);
        std::string modified_line;
        std::vector<std::string_view> split;
        for (const auto& log : logs) {
            modified_line.clear();
            split.clear();
            split_keep_delimiters(delimiters, log, split);
            for (const auto word : split) {
                if (has_ascii_digit(word)) {
                    modified_line += "<*>";
                    variable_set.emplace_back(word);
                } else {
                    modified_line += word;
                }
            }
            modified_lines.push_back(modified_line);
        }
        store_content_with_ids(variable_set, "variableset", chunkID);
        return modified_lines;
    }

    /*
    store_content_with_ids(): process variable set and save content with ID mapping

    input: variable set lines

    output: binary ID file and mapping text file
    */
    void store_content_with_ids(const std::vector<std::string>& input, const std::string& output, const std::string& chunkID) {
        std::unordered_map<std::string_view, int> content_to_id;  // views into input
        std::vector<std::string_view> contents;                    // by ID - 1
        std::vector<int> id_list;
        id_list.reserve(input.size());
        std::string id_dir = "output/" + logname + "/" + chunkID + "/";
        std::string ids_file_path = "output/" + logname + "/" + chunkID + "/" + logname + output + "ids.bin";
        std::string mapping_file_path = "output/" + logname + "/" + chunkID + "/" + logname + output + "mapping.txt";
        ensure_directory_exists(id_dir);

        for (const auto& line : input) {
            if (line.empty()) continue;
            auto it = content_to_id.try_emplace(line, int(contents.size()) + 1).first;
            if (it->second == int(contents.size()) + 1) contents.push_back(line);
            id_list.push_back(it->second);
        }

        // the contents in ID order, then the IDs
        std::string mapping;
        for (std::string_view c : contents) mapping.append(c).push_back('\n');
        write_elastic_ids(ids_file_path, id_list);
        std::ofstream mapping_file(mapping_file_path);
        mapping_file.write(mapping.data(), mapping.size());
    }

    // ids elastic encoded, as one write
    static void write_elastic_ids(const std::string &path, const std::vector<int> &ids) {
        std::string bytes;
        bytes.reserve(ids.size() * 2);
        for (int id : ids) elastic_encode_append(id, bytes);
        std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
    }

    void store_template_with_ids(const std::vector<std::string>& input, const std::string& output, const std::string& chunkID) {
        std::vector<int> id_list;
        std::string id_dir = "output/" + logname + "/" + chunkID + "/";
        std::string ids_file_path = "output/" + logname + "/" + chunkID + "/" + logname + output + "ids.bin";
        ensure_directory_exists(id_dir);

        for (const auto& line : input) {  // empty lines too: the empty template
            const uint64_t tid = resolve_template(line);
            id_list.push_back(int(tid));
        }

        leave_template_index();
        write_elastic_ids(ids_file_path, id_list);
    }
    void store_template_with_ids(const std::string& output, const std::string& chunkID) {
        std::vector<int> id_list;
        std::string id_dir = "output/" + logname + "/" + chunkID + "/";
        std::string ids_file_path = "output/" + logname + "/" + chunkID + "/" + logname + output + "ids.bin";
        ensure_directory_exists(id_dir);

        std::string temps_file_path = "temp/" + logname + "/" + chunkID + "/" + "templates.txt";
        std::ifstream temps_file(temps_file_path);
        if(!temps_file.is_open()){
            std::cerr << "Cannot open template file:"+temps_file_path << std::endl;
            return;
        }
        std::string line;
        while (std::getline(temps_file, line)) {  // empty lines too: the empty template
            const uint64_t tid = resolve_template(line);
            id_list.push_back(int(tid));
        }
        temps_file.close();

        leave_template_index();
        write_elastic_ids(ids_file_path, id_list);
    }

    // The delimiters for splitting the block's lines into words (token_split.h).
    DelimiterSet delimiter_mining(const std::vector<std::string>& logs) {
        // random_shuffle makes the same swaps for any element type, so shuffling
        // indices samples the same lines as shuffling a copy of the lines would
        std::vector<size_t> order(logs.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::random_shuffle(order.begin(), order.end());
        std::unordered_set<size_t> lengths;
        std::vector<std::string> sample;
        size_t iteration_count = 0;
        for (size_t idx : order) {
            const auto& log = logs[idx];
            size_t log_len = log.size();
            if (lengths.find(log_len) == lengths.end()) {
                lengths.insert(log_len);
                sample.push_back(log);
            }
            iteration_count++;
            if (lengths.size() >= 10 || iteration_count >= 200) {
                break;
            }
        }
        std::vector<std::string> delimiters = find_special_chars_with_high_freq(sample);
        if (delimiters.empty()) {
            throw std::runtime_error("No delimiters found. Cannot create a valid regex pattern.");
        }

        return make_delimiter_set(delimiters);
    }

    std::vector<std::string> find_special_chars_with_high_freq(const std::vector<std::string>& str_list, size_t freq_threshold = 10) {
        std::vector<char> candidates = {',', ' ', '|', ';', '[', ']', '(', ')', '_', '/'};
        std::unordered_map<char, size_t> char_counter;
        for (const auto& s : str_list) {
            for (char c : s) {
                if (std::find(candidates.begin(), candidates.end(), c) != candidates.end()) {
                    char_counter[c]++;
                }
            }
        }

        std::vector<std::string> result;
        for (const auto& pair : char_counter) {
            if (pair.second > freq_threshold) {
                result.push_back(std::string(1, pair.first));
            }
        }

        // If the result is empty, add a space character.
        if (result.empty()) {
            result.push_back(" ");
        }

        return result;
    }
};

void ensure_directory_exists(const std::string& dir) {
    std::filesystem::create_directories(dir);
}


std::string sanitize_filename(std::string filename) {
    std::replace(filename.begin(), filename.end(), '<', '_');
    std::replace(filename.begin(), filename.end(), '>', '_');
    return filename;
}


void compressDirectory(const std::string& output_dir, int block_id) {
    std::string directoryPath = output_dir + "/" + std::to_string(block_id);
    std::string command = "tar -cf " + output_dir + "/compressed" + std::to_string(block_id) + ".tar.xz " + " --use-compress-program='" + g_compress_prog + "' " + directoryPath;
    int result = std::system(command.c_str());
    if (result != 0) {
        std::cerr << "Command failed with return code: " << result << std::endl;
    }
}
void store_template_to_temp(const std::vector<std::string>& input, const std::string& logname, const std::string& chunkID) {
    std::string temp_dir = "temp/" + logname + "/" + chunkID + "/";
    std::string temps_file_path = "temp/" + logname + "/" + chunkID + "/" + "templates.txt";
    ensure_directory_exists(temp_dir);
    std::ofstream temps_file(temps_file_path);
    for (const auto& line : input) temps_file << line << "\n";  // empty lines too (byte-exact restore)
    temps_file.close();
}

class AutoWorkers{
public:
    explicit AutoWorkers(uint32_t maxw)
    :max_workers(maxw) {
        free_ids.reserve(max_workers);
        for (uint32_t i = 0; i < max_workers; ++i) free_ids.push_back(i);
    }
    void processLogBlock(int stage,const std::vector<std::string>& block, int block_id, const std::string& logname, LogProcessor& log_processor, DenumLogProcessor& denum_processor){
        worker_id();
        if(stage==0){
            PROF_SCOPE(block0);
            auto [final_output, final_patterns, final_shapes, final_ctx] = log_processor.replace_and_group(block); // parse Pure numbers & Tokens containing only ***.
            std::string logname_dir = "output/" + logname + "/" + std::to_string(block_id) + "/";
            ensure_directory_exists(logname_dir);
            std::string temp_logname_dir = "temp/" + logname + "/" + std::to_string(block_id) + "/";
            ensure_directory_exists(temp_logname_dir);
            std::vector<std::string> modified_logs = denum_processor.variable_extract(final_output, std::to_string(block_id)); // parse Numeric variables

            store_template_to_temp(modified_logs,logname,std::to_string(block_id));

            // numbers are stored per tag: the tag's .bin file
            for (const auto &pair : final_patterns) {
                const std::string path = logname_dir + sanitize_filename(pair.first) + ".bin";
                auto c = final_ctx.find(pair.first);
                if (c == final_ctx.end()) write_numbers(path, pair.first, pair.second);
                else write_numbers(path, pair.first, group_by_context(pair.second, c->second));
            }
            write_shapes(logname_dir + logname + "shapes.txt", final_shapes);
        }
        else if(stage==1){
            PROF_SCOPE(block1);
            std::string temp_logname_dir = "temp/" + logname + "/" + std::to_string(block_id) + "/";
            ensure_directory_exists(temp_logname_dir);
            std::string temps_file_path = temp_logname_dir + "templates.txt";
            std::ifstream temps_file(temps_file_path);
            if(!temps_file.is_open()){
                auto [final_output, final_patterns, final_shapes, final_ctx] = [&]{ PROF_SCOPE(parse); return log_processor.replace_and_group(block); }(); // parse Pure numbers & Tokens containing only ***.
                std::string logname_dir = "output/" + logname + "/" + std::to_string(block_id) + "/";
                ensure_directory_exists(logname_dir);
                std::vector<std::string> modified_logs = [&]{ PROF_SCOPE(varext); return denum_processor.variable_extract(final_output, std::to_string(block_id)); }(); // parse Numeric variables

                { PROF_SCOPE(tmplstore);
                  denum_processor.store_template_with_ids(modified_logs, "all", std::to_string(block_id)); }

                // numbers are stored per tag: the tag's .bin file
                PROF_SCOPE(numwrite);
                for (const auto &pair : final_patterns) {
                    const std::string path = logname_dir + sanitize_filename(pair.first) + ".bin";
                    auto c = final_ctx.find(pair.first);
                    if (c == final_ctx.end()) write_numbers(path, pair.first, pair.second);
                    else write_numbers(path, pair.first, group_by_context(pair.second, c->second));
                }
                write_shapes(logname_dir + logname + "shapes.txt", final_shapes);
            }
            else{
                { PROF_SCOPE(tmplstore); denum_processor.store_template_with_ids("all", std::to_string(block_id)); }
            }
            // Compress the output directory of the current block.
            std::string output_dir = "output/" + logname;
            { PROF_SCOPE(compress); compressDirectory(output_dir, block_id); }
        }
    }
    // processLogBlock() for the block at `range` of the log open as `fd`. It takes a
    // worker slot before reading the block, so at most max_workers blocks are in
    // memory at once. Stage 1 of a block whose templates stage 0 already wrote does
    // not use the lines (processLogBlock reads the templates instead), so they are
    // not read.
    void processLogBlockAt(int stage, int fd, BlockRange range, int block_id, const std::string& logname, LogProcessor& log_processor, DenumLogProcessor& denum_processor){
        worker_id();
        std::vector<std::string> block;
        const std::string temps_file_path = "temp/" + logname + "/" + std::to_string(block_id) + "/templates.txt";
        if (stage == 0 || !std::ifstream(temps_file_path).is_open()) block = read_block_lines(fd, range);
        processLogBlock(stage, block, block_id, logname, log_processor, denum_processor);
    }
private:
    class ThreadSlot{
    public:
        ThreadSlot(AutoWorkers* _owner):owner(_owner), id(owner->acquire()){}
        ~ThreadSlot(){owner->release(id);}
        uint32_t getid()const{return id;}
    private:
        AutoWorkers* owner;
        uint32_t id;
    };
    uint32_t worker_id() {
        thread_local ThreadSlot slot(this);
        return slot.getid();
    }
    uint32_t acquire(){
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&]{ return !free_ids.empty(); });
        uint32_t id = free_ids.back();
        free_ids.pop_back();
        return id;
    }
    void release(uint32_t id) {
        std::lock_guard<std::mutex> g(mu);
        free_ids.push_back(id);
        cv.notify_one();
    }
    uint32_t max_workers;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint32_t> free_ids;
};

// Usage: logminify <logname> <block_size>
// Compresses Logs/<logname>/<logname>.log and writes CR/CS to info.txt.

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <logname> <block_size>" << std::endl;
        return 1;
    }

    if (const char *env = std::getenv("LOGMINIFY_COMPRESS"); env && *env) {
        g_compress_prog = env;
    }

    const std::string logname = argv[1];
    size_t BLOCK_SIZE;

    try {
        BLOCK_SIZE = std::stoul(argv[2]);
    } catch (const std::invalid_argument& e) {
        std::cerr << "Invalid block size argument: " << argv[2] << std::endl;
        return 1;
    } catch (const std::out_of_range& e) {
        std::cerr << "Block size value out of range: " << argv[2] << std::endl;
        return 1;
    }

    std::cout << "Block Size: " << BLOCK_SIZE << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    PROF_MARK("start");
    const std::string log_path = "Logs/" + logname + "/" + logname + ".log";
    
    configure_workers();
    const int num_threads = fg_n;
    std::vector<std::future<void>> futures;
    AutoWorkers auto_workers(num_threads);

    LogProcessor log_processor(logname);
    DenumLogProcessor denum_processor(logname);

    const int log_fd = open(log_path.c_str(), O_RDONLY);
    if (log_fd < 0) {
        std::cerr << "Unable to open log file: " << log_path << std::endl;
        return 1;
    }

    // Clean up and ensure the output directory exists.
    std::string command = "rm -rf output/" + logname + "/* ";
    if (std::system(command.c_str()) != 0) std::cerr << "Warning: " << command << " failed" << std::endl;
    command = "rm -rf temp/" + logname + "/* ";
    if (std::system(command.c_str()) != 0) std::cerr << "Warning: " << command << " failed" << std::endl;
    ensure_directory_exists("output/" + logname);
    {   // a log whose last line has no '\n': tell the decompressor (an empty marker file)
        struct stat st;
        char last = '\n';
        if (fstat(log_fd, &st) == 0 && st.st_size > 0 && pread(log_fd, &last, 1, st.st_size - 1) == 1 && last != '\n')
            std::ofstream("output/" + logname + "/noeol");
    }
    ensure_directory_exists("temp/" + logname);

    // The main thread only finds where each block ends; the worker of a block reads
    // its lines (block_reader.h). The index is initialized with the templates of
    // the first 21 blocks.
    BlockSplitter splitter(log_fd, BLOCK_SIZE);
    std::vector<BlockRange> initial_blocks;
    BlockRange range;
    while (initial_blocks.size() < 21 && splitter.next(range)) {
        const int block_id = initial_blocks.size();
        initial_blocks.push_back(range);
        futures.push_back(
            std::async(
                std::launch::async,
                &AutoWorkers::processLogBlockAt,
                &auto_workers,
                0,
                log_fd,
                range,
                block_id,
                logname,
                std::ref(log_processor),
                std::ref(denum_processor)
            )
        );
    }
    int block_index = int(initial_blocks.size()) - 1;  // the last block dispatched
    bool worker_failed = false;
    for (auto& future : futures) { // Wait for all threads to complete and rethrow any exceptions.
        try {
            future.get();
        } catch (const std::exception &e) {
            std::cerr << "Worker thread threw exception: " << e.what() << std::endl;
            worker_failed = true;
        } catch (...) {
            std::cerr << "Worker thread threw unknown exception." << std::endl;
            worker_failed = true;
        }
    }
    if (worker_failed) return 1;
    futures.clear();

    PROF_MARK("stage0 (read 21 blocks+wait)");
    // initialize the template index
    auto start_build = std::chrono::high_resolution_clock::now();
    // Each initial block's templates.txt has one template per log line, but only a
    // few thousand distinct ones in all: read and deduplicate the blocks in
    // parallel, then merge.
    std::vector<std::future<std::unique_ptr<std::unordered_set<std::string>>>> loads;
    for (int i = 0; i <= block_index; i++) {
        loads.push_back(std::async(std::launch::async, [&logname, i]() -> std::unique_ptr<std::unordered_set<std::string>> {
            const std::string temps_file_path = "temp/" + logname + "/" + std::to_string(i) + "/" + "templates.txt";
            std::ifstream temps_file(temps_file_path);
            if (!temps_file.is_open()) {
                std::cerr << "Cannot open template file:" + temps_file_path << std::endl;
                return nullptr;
            }
            auto distinct = std::make_unique<std::unordered_set<std::string>>();
            std::string line;
            while (std::getline(temps_file, line)) {
                if (!line.empty()) distinct->insert(line);
            }
            return distinct;
        }));
    }
    std::unordered_set<std::string> distinct;
    bool loaded = true;
    for (auto &load : loads) {
        std::unique_ptr<std::unordered_set<std::string>> block = load.get();
        if (block == nullptr) {
            loaded = false;
            continue;
        }
        if (distinct.empty()) distinct.swap(*block);
        else distinct.insert(std::make_move_iterator(block->begin()), std::make_move_iterator(block->end()));
    }
    if (!loaded) return 1;
    std::vector<std::string> templates(std::make_move_iterator(distinct.begin()), std::make_move_iterator(distinct.end()));
    std::sort(templates.begin(), templates.end());
    // sorted, so the initial templates get the same IDs
    std::cout << "Building RSSPDC with " << templates.size() << " templates." << std::endl;
    rsspdc_index = new RsspdcTemplateIndex(key_cnt_increment, templates);  // bulk load, IDs in order
    auto end_build = std::chrono::high_resolution_clock::now();
    PROF_MARK("index build");

    // complete compression and storage for all logs in the following section
    auto dispatch_stage1 = [&](BlockRange r, int block_id) {
        futures.push_back(
            std::async(
                std::launch::async,
                &AutoWorkers::processLogBlockAt,
                &auto_workers,
                1,
                log_fd,
                r,
                block_id,
                logname,
                std::ref(log_processor),
                std::ref(denum_processor)
            )
        );
    };
    for (int i = 0; i <= block_index; ++i) dispatch_stage1(initial_blocks[i], i);
    while (splitter.next(range)) dispatch_stage1(range, ++block_index);
    PROF_MARK("stage1 read+dispatch");
    worker_failed = false;
    for (auto& future : futures) { // Wait for all threads to complete and rethrow any exceptions.
        try {
            future.get();
        } catch (const std::exception &e) {
            std::cerr << "Worker thread threw exception: " << e.what() << std::endl;
            worker_failed = true;
        } catch (...) {
            std::cerr << "Worker thread threw unknown exception." << std::endl;
            worker_failed = true;
        }
    }
    if (worker_failed) return 1;
    PROF_MARK("stage1 wait for workers");
    futures.clear();
    close(log_fd);

    std::string mapping_file_path, mapping_compressed_path;
    {
        const std::string name = "rsspdc";
        mapping_file_path = dict_files(name);
        mapping_compressed_path = "output/" + logname + "/" + name + ".tar.xz";
        if (!dump_dictionary(*rsspdc_index, name)) {
            std::cerr << "Failed to write the " << name << " dictionary (" << mapping_file_path << ")" << std::endl;
            return 1;
        }
    }
    PROF_MARK("write dictionary");  // index dump to files
    std::string compress_mapping_command = "tar -cf " + mapping_compressed_path + " --use-compress-program='" + g_compress_prog + "' " + mapping_file_path;
    const int result = std::system(compress_mapping_command.c_str());
    if (result != 0) {
        std::cerr << "Failed to compress mapping file with return code: " << result << std::endl;
        return 1;
    }

    PROF_MARK("dictionary tar+xz");
    PROF_REPORT();
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto build_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_build - start_build);
    std::uintmax_t fileSize = std::filesystem::file_size(log_path);
    double dataSizeInMB = static_cast<double>(fileSize) / (1024.0 * 1024.0);
    double speedInMBPerSecond = dataSizeInMB / duration.count() * 1000;
    double totalSize = 0;
    for (int i = 0; i <= block_index; ++i) {
        std::string compressed_path = "output/" + logname + "/compressed" + std::to_string(i) + ".tar.xz";
        const double blockSizeInMB = std::filesystem::file_size(compressed_path) / (1024.0 * 1024.0);
        totalSize += blockSizeInMB;
    }

    std::uintmax_t dictSize = std::filesystem::file_size(mapping_compressed_path);
    double dictSizeInMB = static_cast<double>(dictSize) / (1024.0 * 1024.0);

    std::ofstream info_file("info.txt",std::ios::app);
    info_file << "logminify" << std::endl;
    info_file << "Logname: " << logname << std::endl;
    info_file << "Compressor: " << g_compress_prog << std::endl;
    info_file << "Index building time: " << build_duration.count() << " milliseconds." << std::endl;
    info_file << "Replacement completed in " << duration.count() << " milliseconds." << std::endl;
    info_file << "Compression speed: " << std::fixed << std::setprecision(6) << speedInMBPerSecond << " MB/s" << std::endl;
    info_file << "Original size: " << dataSizeInMB << " MB" << std::endl;
    info_file << "Compressed Log Blocks size:" << totalSize << " MB" << std::endl;
    info_file << "Template Dictionary size: " << dictSizeInMB << " MB" << std::endl;
    info_file << "Original/(CompressedLogBlocks+TemplateDictionary), Compression ratio: " << std::fixed << std::setprecision(6) << dataSizeInMB / (totalSize + dictSizeInMB) << std::endl;
    info_file << "----------------------------------------" << std::endl;
    info_file.close();
    delete rsspdc_index;
    return 0;
}
