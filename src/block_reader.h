// Splitting the input log into blocks of block_size lines without the main thread
// copying any line: BlockSplitter reads the file once through a reused buffer and
// yields each block's byte range, and the worker that compresses a block reads and
// splits its lines with read_block_lines(). Lines split as std::getline splits
// them: at '\n', which is dropped; the last line counts even without a final '\n',
// and a final '\n' starts no further (empty) line.
#ifndef LOGMINIFY_BLOCK_READER_H
#define LOGMINIFY_BLOCK_READER_H

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

struct BlockRange {
  uint64_t begin = 0;  // byte offset of the block's first line
  uint64_t end = 0;    // byte offset just past its last line ('\n' included)
};

// Yields the blocks of block_size lines of `fd`'s file in order, the last one
// possibly shorter, reading the file only as far as the blocks asked for.
class BlockSplitter {
 public:
  BlockSplitter(int fd, size_t block_size, size_t buffer_size = 8 << 20)
      : fd_(fd), block_size_(block_size), buf_(buffer_size) {
    if (block_size == 0) throw std::invalid_argument("BlockSplitter: block_size is 0");
  }

  // The next block's range, or false after the last block.
  bool next(BlockRange &range) {
    for (;;) {
      while (pos_ < len_) {
        const char *p = buf_.data() + pos_;
        const char *nl = static_cast<const char *>(std::memchr(p, '\n', len_ - pos_));
        if (nl == nullptr) {
          pos_ = len_;
          break;
        }
        pos_ = nl + 1 - buf_.data();
        if (++lines_ == block_size_) {
          range = {block_begin_, offset_ + pos_};
          block_begin_ = range.end;
          lines_ = 0;
          return true;
        }
      }
      offset_ += len_;
      pos_ = len_ = 0;
      ssize_t n;
      do n = ::pread(fd_, buf_.data(), buf_.size(), offset_); while (n < 0 && errno == EINTR);
      if (n < 0) throw std::runtime_error(std::string("BlockSplitter: read failed: ") + std::strerror(errno));
      if (n == 0) {  // end of file: a last, shorter block if anything is left
        if (offset_ == block_begin_) return false;
        range = {block_begin_, offset_};
        block_begin_ = offset_;
        lines_ = 0;
        return true;
      }
      len_ = n;
    }
  }

 private:
  int fd_;
  size_t block_size_;
  std::vector<char> buf_;
  uint64_t offset_ = 0;       // file offset of buf_[0]
  size_t pos_ = 0, len_ = 0;  // next byte to scan in buf_, bytes in buf_
  uint64_t block_begin_ = 0;  // start of the current block
  size_t lines_ = 0;          // complete lines in the current block
};

// The lines of block `r` of `fd`'s file.
inline std::vector<std::string> read_block_lines(int fd, BlockRange r) {
  std::string bytes(r.end - r.begin, '\0');
  for (size_t done = 0; done < bytes.size();) {
    ssize_t n = ::pread(fd, &bytes[done], bytes.size() - done, r.begin + done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("read_block_lines: short read");
    done += n;
  }
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < bytes.size()) {
    const char *nl = static_cast<const char *>(std::memchr(bytes.data() + start, '\n', bytes.size() - start));
    const size_t stop = nl ? nl - bytes.data() : bytes.size();
    lines.emplace_back(bytes, start, stop - start);
    start = stop + 1;
  }
  return lines;
}

#endif  // LOGMINIFY_BLOCK_READER_H
