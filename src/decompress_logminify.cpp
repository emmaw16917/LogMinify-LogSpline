// Decompressor: restores each block archive with the global template dictionary,
// then concatenates the blocks in order. The template map is fully populated before
// the workers start and read-only afterwards, so the read path takes no lock.
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <list>
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <regex>
#include <thread>
#include <mutex>
#include <future>
#include "dict_dump_format.h"
#include "lossless_format.h"

namespace fs = std::filesystem;
// establish mapping from key-value to template
std::unordered_map<uint64_t, std::string> id_to_template;
// The dictionary: every template in byte order, front coded in <name>_fc.txt,
// with the IDs, in the same order, in <name>_ids.bin (dict_dump_format.h).
bool get_id_to_template(const std::string &name) {
    const std::string text_path = name + "_fc.txt";
    const std::string ids_path = name + "_ids.bin";
    std::ifstream ids_file(ids_path, std::ios::binary);
    if (!ids_file) {
        std::cerr << "Failed to open " << ids_path << "\n";
        return false;
    }
    const std::vector<uint64_t> ids = decode_delta_ids(std::string(
        (std::istreambuf_iterator<char>(ids_file)), std::istreambuf_iterator<char>()));
    std::ifstream dict_file(text_path, std::ios::binary);
    if (!dict_file || !read_fc_dump(dict_file, id_to_template, ids)) {
        std::cerr << "Failed to read the front-coded dictionary (" << text_path << ")\n";
        return false;
    }
    return true;
}

// Zigzag/elastic decode
int64_t zigzag_decode(int64_t num) {
    return (num >> 1) ^ -(num & 1);
}
int64_t elastic_decode(const std::vector<unsigned char>& num_bytes) {
    int64_t ret = 0;
    int offset = 0;
    for (auto cur : num_bytes) {
        ret |= (static_cast<int64_t>(cur & 0x7F) << offset);
        if ((cur & 0x80) == 0) break;
        offset += 7;
    }
    return zigzag_decode(ret);
}
std::vector<int64_t> elastic_decode_bytes(const std::vector<unsigned char>& binary_bytes) {
    std::vector<int64_t> num_list;
    std::vector<unsigned char> num_byte;
    for (auto byt : binary_bytes) {
        num_byte.push_back(byt);
        if (byt < 128) {
            int64_t decode_num = elastic_decode(num_byte);
            num_list.push_back(decode_num);
            num_byte.clear();
        }
    }
    return num_list;
}
// delta decoding
std::list<int64_t> delta_restore(const std::list<int64_t>& delta_list) {
    std::list<int64_t> restored;
    if (delta_list.empty()) return restored;
    auto it = delta_list.begin();
    int64_t last = *it;
    restored.push_back(last);
    ++it;
    for (; it != delta_list.end(); ++it) {
        last += *it;
        restored.push_back(last);
    }
    return restored;
}

// read binary file and perform elastic decoding
std::list<int64_t> read_bin_numbers(const std::string& filename, bool use_delta) {
    std::ifstream file(filename, std::ios::binary);
    std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto nums = elastic_decode_bytes(buffer);
    std::list<int64_t> num_list(nums.begin(), nums.end());
    if (use_delta) return delta_restore(num_list);
    return num_list;
}

// read variable content
std::vector<std::string> read_mapping(const std::string& filename) {
    std::ifstream file(filename);
    std::vector<std::string> mapping;
    std::string line;
    while (std::getline(file, line)) mapping.push_back(line);
    return mapping;
}
// read the sequence of IDs
std::vector<int> read_ids(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto ids64 = elastic_decode_bytes(buffer);
    std::vector<int> ids;
    for (auto v : ids64) ids.push_back(static_cast<int>(v));
    return ids;
}

// decompress xz file
void decompress_xz(const std::string& xzfile, const std::string& outdir) {
    std::string cmd = "tar -xJf " + xzfile + " -C " + outdir;
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "Failed to decompress: " << xzfile << std::endl;
        exit(1);
    }
}

// find all blocks
std::vector<int> find_block_ids(const std::string& outdir) {
    std::vector<int> ids;
    for (const auto& entry : fs::directory_iterator(outdir)) {
        std::string name = entry.path().filename().string();
        if (name.find("compressed") == 0 && name.find(".xz") != std::string::npos) {
            std::string num = name.substr(10, name.size() - 13);
            ids.push_back(std::stoi(num));
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// find all *.bin files (excluding variablesetids.bin and allids.bin)
std::vector<fs::path> find_bin_files(const std::string& block_dir) {
    std::vector<fs::path> bin_files;
    for (const auto& entry : fs::directory_iterator(block_dir)) {
        auto fname = entry.path().filename().string();
        if (fname.size() > 4 && fname.substr(fname.size()-4) == ".bin" &&
            fname != "variablesetids.bin" && fname != "allids.bin") {
            bin_files.push_back(entry.path());
        }
    }
    return bin_files;
}

// replace all occurrences of a placeholder in logs with corresponding replacements
void replace_placeholders(std::vector<std::string>& logs, const std::string& placeholder, const std::vector<std::string>& replacements) {
    auto it = replacements.begin();
    for (auto& log : logs) {
        size_t pos = 0;
        while ((pos = log.find(placeholder, pos)) != std::string::npos && it != replacements.end()) {
            log.replace(pos, placeholder.size(), *it);
            ++it;
            pos += 1;
        }
    }
}

// restore a single block
void restore_block(const std::string& block_dir, const std::string& logname) {
    

    // 1. read template and variable content
    std::vector<std::string> variables;
    std::vector<int> template_ids, variable_ids;
    std::string template_id_path = block_dir + logname + "allids.bin";
    std::string variable_path = block_dir + logname + "variablesetmapping.txt";
    std::string variable_id_path = block_dir + logname + "variablesetids.bin";

    if (!fs::exists(template_id_path)) return;
    template_ids = read_ids(template_id_path);

    if (fs::exists(variable_path) && fs::exists(variable_id_path)) {
        variables = read_mapping(variable_path);
        variable_ids = read_ids(variable_id_path);
    }

    // 2. restore log templates using template IDs
    std::vector<std::string> block_logs;
    const auto& dict = id_to_template;  // read-only after loading
    for (auto id : template_ids) {
        auto it = dict.find(id);
        if (it == dict.end()) {
            std::cerr << "[Warning] Template ID " << id << " not found in block " << block_dir << std::endl;
            continue;
        }
        block_logs.push_back(it->second);
    }

    // 3. restore variable content using variable IDs and replace <*>
    std::vector<std::string> var_replacements;
    for (auto id : variable_ids) {
        if (id > 0 && id <= (int)variables.size()) var_replacements.push_back(variables[id-1]);
        else var_replacements.push_back("");
    }
    replace_placeholders(block_logs, "<*>", var_replacements);

    // 4. find all *.bin files and replace special tags (lossless_format.h):
    //    <TS> the header timestamp, by the dataset's TsSpec;
    //    <X> (upper case) a regex tag: its number and its shape (_X_s.bin, IDs into
    //        <log>shapes.txt);
    //    <x>/<xy> (lower case) a run of digits whose length the first letter gives
    //        (a = 1), zero padded to it
    std::vector<std::string> shapes;
    if (fs::exists(block_dir + logname + "shapes.txt")) shapes = read_mapping(block_dir + logname + "shapes.txt");
    // *.bin files: the numbers of tag <x> in _x_.bin; delta encoded except for <I>,
    // <a>, <b> and <c>
    std::unordered_map<std::string, std::vector<int64_t>> num_values;  // lower case tags
    std::vector<std::string> upper_tags;
    for (const auto& bin_path : find_bin_files(block_dir)) {
        std::smatch m;
        static const std::regex pat(R"(^_([a-zA-Z]+)_\.bin$)");
        const std::string fname = bin_path.filename().string();
        if (!std::regex_search(fname, m, pat)) continue;
        const std::string tag = m[1];
        if (std::isupper(static_cast<unsigned char>(tag[0]))) {
            upper_tags.push_back(tag);
            continue;
        }
        auto nums = read_bin_numbers(bin_path.string(), !(tag == "a" || tag == "b" || tag == "c"));
        num_values[tag].assign(nums.begin(), nums.end());
    }

    // 4a. num tags, in one pass over the block: each occurrence's tag and context, in
    //     block order; a tag's numbers are stored grouped by context
    if (!num_values.empty()) {
        struct Occ { size_t line, pos, len; uint32_t ctx; std::vector<int64_t> *values; };
        std::vector<Occ> occs;
        std::unordered_map<std::string, uint32_t> ctx_ids;
        std::unordered_map<const std::vector<int64_t> *, std::vector<size_t>> of_tag;
        for (size_t li = 0; li < block_logs.size(); ++li) {
            const std::string &s = block_logs[li];
            for (size_t p = s.find('<'); p != std::string::npos; p = s.find('<', p + 1)) {
                size_t q = p + 1;
                while (q < s.size() && q - p <= 2 && s[q] >= 'a' && s[q] <= 'z') ++q;
                if (q == p + 1 || q >= s.size() || s[q] != '>') continue;
                auto it = num_values.find(s.substr(p + 1, q - p - 1));
                if (it == num_values.end()) continue;
                const uint32_t ctx = ctx_ids.try_emplace(std::string(num_context(s, p)), uint32_t(ctx_ids.size())).first->second;
                of_tag[&it->second].push_back(occs.size());
                occs.push_back({li, p, q + 1 - p, ctx, &it->second});
            }
        }
        std::vector<int64_t> value(occs.size(), 0);
        for (auto &kv : of_tag) {
            std::vector<size_t> order = kv.second;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return occs[a].ctx < occs[b].ctx; });
            for (size_t k = 0; k < order.size() && k < kv.first->size(); ++k) value[order[k]] = (*kv.first)[k];
        }
        // rebuild the lines, each num tag replaced by its number, zero padded to the
        // length the tag's first letter gives (a = 1)
        size_t k = 0;
        while (k < occs.size()) {
            const size_t li = occs[k].line;
            const std::string &s = block_logs[li];
            std::string out;
            size_t at = 0;
            for (; k < occs.size() && occs[k].line == li; ++k) {
                out.append(s, at, occs[k].pos - at);
                std::string d = std::to_string(value[k]);
                const size_t width = size_t(s[occs[k].pos + 1] - 'a') + 1;
                if (d.size() < width) d.insert(0, width - d.size(), '0');
                out += d;
                at = occs[k].pos + occs[k].len;
            }
            out.append(s, at, std::string::npos);
            block_logs[li].swap(out);
        }
    }

    // 4b. regex tags (shapes) and the header timestamp
    for (const std::string &tag : upper_tags) {
        auto nums = read_bin_numbers(block_dir + "_" + tag + "_.bin", tag != "I");
        std::vector<std::string> texts;
        texts.reserve(nums.size());
        const int ti = ts_index(tag);
        const TsSpecs specs = ts_specs_for(logname);
        if (ti >= 0 && ti < specs.n) {  // a header timestamp
            for (int64_t v : nums) texts.push_back(ts_format(specs.spec[ti], v));
        } else {
            auto shape_ids = read_bin_numbers(block_dir + "_" + tag + "_s.bin", true);
            auto sid = shape_ids.begin();
            for (int64_t v : nums) {
                const int64_t id = sid != shape_ids.end() ? *sid++ : 0;
                if (id >= 1 && id <= int64_t(shapes.size())) texts.push_back(apply_shape(shapes[id - 1], v, tag == "I"));
                else texts.push_back(std::to_string(v));
            }
        }
        replace_placeholders(block_logs, "<" + tag + ">", texts);
    }
    for (auto &line : block_logs) unescape_line(line);

    // 5. output restored logs (empty lines too)
    std::string tmp_log_path=block_dir+"decompressed.log";
    std::ofstream tmp_log_file(tmp_log_path, std::ios::binary);
    for(const auto& line:block_logs) tmp_log_file<<line<<"\n";
    tmp_log_file.close();
}
class AutoWorkers{
public:
  explicit AutoWorkers(uint32_t maxw)
  :max_workers(maxw) {
    free_ids.reserve(max_workers);
    for (uint32_t i = 0; i < max_workers; ++i) free_ids.push_back(i);
  }
  void process_block(int block_id,const std::string& logname) {
    uint32_t wid = worker_id();
    std::string xzfile = "output/" + logname + "/compressed" + std::to_string(block_id) + ".tar.xz";
    std::string block_dir = "tmp_decompress/output/" + logname + "/" + std::to_string(block_id) + "/";
    decompress_xz(xzfile, "tmp_decompress");

    if (!fs::exists(block_dir)) {
        std::cerr << "  [ERROR] Block directory does not exist after decompress: " << block_dir << std::endl;
        return;
    }
    restore_block(block_dir, logname);
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
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <logname>" << std::endl;
        return 1;
    }
    std::string logname = argv[1];
    std::string outdir = "output/" + logname;
    std::string tmpdir = "tmp_decompress/output/" + logname;
    std::string command = "rm -rf " + tmpdir;
    if (std::system(command.c_str()) != 0) std::cerr << "Warning: " << command << " failed" << std::endl;
    fs::create_directories(tmpdir);
    // The dictionary archive written by logminify.
    const std::string dict_name = "rsspdc";
    const std::string mapping_compressed_path = "output/" + logname + "/" + dict_name + ".tar.xz";
    for (const std::string leftover : {dict_name + "_fc.txt", dict_name + "_ids.bin"}) {
        fs::remove(leftover);  // a stale dictionary must not be picked up
    }

    auto start = std::chrono::high_resolution_clock::now();

    command = "tar -xJf " + mapping_compressed_path + " -C .";
    const int result = std::system(command.c_str());
    if (result != 0) {
        std::cerr << "Failed to decompress mapping file with return code: " << result << std::endl;
        return 1;
    }

    if(!get_id_to_template(dict_name)) {
        std::cerr << "Failed to get id to template mapping." << std::endl;
        return 1;
    }

    auto block_ids = find_block_ids(outdir);

    const int num_threads = [] {
        if (const char *env = std::getenv("LOGMINIFY_DEC_THREADS")) {
            const int v = std::atoi(env);
            if (v > 0) return v;
        }
        const unsigned hw = std::thread::hardware_concurrency();
        return hw ? static_cast<int>(hw) : 4;
    }();
    std::vector<std::future<void>> futures;
    AutoWorkers auto_workers(num_threads);
    for (int block_id : block_ids) {
        futures.push_back(
            std::async(
                std::launch::async, 
                &AutoWorkers::process_block, 
                &auto_workers, 
                block_id, 
                logname
            )
        );
    }
    for (auto& future : futures) { // Wait for all threads to complete and rethrow any exceptions.
        try {
            future.get();
        } catch (const std::exception &e) {
            std::cerr << "Worker thread threw exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Worker thread threw unknown exception." << std::endl;
        }
    }
    futures.clear();
    
    std::string out_log_path = outdir + "_decompressed.log";
    std::ofstream fout(out_log_path, std::ios::binary);
    for(int block_id : block_ids){
        std::string block_dir = tmpdir + "/" + std::to_string(block_id) + "/";
        std::string tmp_log_path=block_dir + "decompressed.log";
        std::ifstream fin(tmp_log_path, std::ios::binary);
        if(!fin.is_open()){
            std::cerr<<"[Warning] Cannot open decompressed log file for block "<<block_id<<std::endl;
            continue;
        }
        fout << fin.rdbuf();  // the block's lines as written, empty ones too
        fin.close();
    }
    fout.close();
    if (fs::exists(outdir + "/noeol")) {  // the input's last line had no '\n'
        const auto size = fs::file_size(out_log_path);
        if (size > 0) fs::resize_file(out_log_path, size - 1);
    }

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    std::ofstream info_file("info.txt",std::ios::app);
    info_file << "decompress_logminify" << std::endl;
    info_file << "Logname: " << logname << std::endl;
    info_file << "Decompression completed in " << duration.count() << " milliseconds." << std::endl;
    info_file << "Duration: " << std::setprecision(6) << duration.count()/1000.0 << " seconds" << std::endl;
    info_file << "----------------------------------------" << std::endl;
    info_file.close();
    std::cout << "Decompressed log saved to: " << out_log_path << std::endl;
    return 0;
}
