/*******************************************************************************
 * benchmark/genqueries.cpp
 *
 * Copyright (C) 2022 Patrick Dinklage <patrick.dinklage@tu-dortmund.de>
 *
 * All rights reserved. Published under the BSD-2 license in the LICENSE file.
 ******************************************************************************/

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <tlx/cmdline_parser.hpp>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

struct {
  std::string file_text;
  std::string file_sa;
  std::string file_lcp;
  std::string out_dir = ".";
  uint width = 5;
  size_t limit = 100'000;
} options;

class BufferedReader {
private:
  int fd_;

  size_t bufsize_;
  std::unique_ptr<char[]> buf_;
  
  char* bufp_;
  char* bufend_;

  void underflow() {
    bufp_ = buf_.get();

    auto const r = ::read(fd_, bufp_, bufsize_);
    bufend_ = bufp_ + r;
  }

public:
  BufferedReader(int fd, size_t bufnum)
    : fd_(fd),
      bufsize_(options.width * bufnum),
      buf_(std::make_unique<char[]>(bufsize_)),
      bufp_(nullptr),
      bufend_(nullptr) {
  }

  uint64_t read() {
    if(bufp_ >= bufend_) underflow();

    static_assert(std::endian::native == std::endian::little);
    uint64_t value = 0; // nb: important to initialize as zero

    char* v = (char*)&value;
    for(size_t i = 0; i < options.width; i++) {
      *v++ = *bufp_++;
    }

    return value;
  }
};

int main(int argc, char** argv) {
  {
    tlx::CmdlineParser cp;
    cp.set_description("This program generates LCE queries for the benchmarks, "
                      "streaming the pre-generated suffix and LCP array for the"
                      "input text.");
    cp.set_author("Alexander Herlez <alexander.herlez@tu-dortmund.de>\n"
                  "        Florian Kurpicz  <florian.kurpicz@tu-dortmund.de>\n"
                  "        Patrick Dinklage <patrick.dinklage@tu-dortmund.de>");
    cp.add_param_string("file", options.file_text,
                        "The text to generate queries for.");
    cp.add_string('o', "out", options.out_dir,
                  "The output directory (default: working directory)");
    cp.add_string("sa", options.file_sa,
                  "The file containing the suffix array "
                  "(default: <file>.sa<width>)");
    cp.add_string("lcp", options.file_lcp,
                  "The file containing the LCP array "
                  "(default: <file>.lcp<width>)");
    cp.add_uint('w', "width", options.width,
                  "The number of bytes per suffix and LCP array in their "
                  "corresponding files (default: 5).");
    cp.add_bytes('l', "limit", options.limit,
                "the maximum number of queries to generate per CP length"
                "(default: 100,000)");
    if(!cp.process(argc, argv)) {
      return -1;
    }

    if(options.file_sa.empty()) {
      options.file_sa = options.file_text
                        + ".sa" + std::to_string(options.width);
    }
    if(options.file_lcp.empty()) {
      options.file_lcp = options.file_text
                        + ".lcp" + std::to_string(options.width);
    }

    if(options.width < 1 || options.width > 8) {
      std::cerr << "unsupported width: " << options.width << std::endl;
      return -1;
    }
    if(!std::filesystem::is_regular_file(options.file_text)) {
      std::cerr << "file not found: " << options.file_text << std::endl;
      return -1;
    }
    if(!std::filesystem::is_regular_file(options.file_sa)) {
      std::cerr << "file not found: " << options.file_sa << std::endl;
      return -1;
    }
    if(!std::filesystem::is_regular_file(options.file_lcp)) {
      std::cerr << "file not found: " << options.file_lcp << std::endl;
      return -1;
    }

    std::cout << "Generating LCE queries for \"" << options.file_text << "\""
              << " to \"" << options.out_dir << "\""
              << " ..." << std::endl;
  }

  // init
  size_t constexpr bufnum = 1024 * 1024;
  size_t constexpr max_lcp_exp = 20;
  size_t const n = std::filesystem::file_size(options.file_text);

  // open inputs
  int fd_sa = open(options.file_sa.c_str(), O_RDONLY);
  posix_fadvise(fd_sa, 0, 0, POSIX_FADV_SEQUENTIAL);
  int fd_lcp = open(options.file_lcp.c_str(), O_RDONLY);
  posix_fadvise(fd_lcp, 0, 0, POSIX_FADV_SEQUENTIAL);

  BufferedReader sa(fd_sa, bufnum * options.width);
  BufferedReader lcp(fd_lcp, bufnum * options.width);

  // open outputs
  std::array<std::ofstream, max_lcp_exp+1> out;
  std::array<size_t, max_lcp_exp+1> count;
  {
    for(size_t x = 0; x <= max_lcp_exp; x++) {
      auto outfile = std::filesystem::path(options.out_dir) /
                     ("lce_" + std::to_string(x));
      out[x] = std::ofstream(outfile);
      count[x] = 0;
    }
  }

  // read first SA entry and discard first LCP value
  size_t const one_pct = (size_t)(double(n) / 100.0);
  size_t next_progress = one_pct;

  auto sa_prev = sa.read();
  lcp.read();

  for(size_t i = 1; i < n; i++) {
    // progress
    {
      if(i >= next_progress) {
        std::cout << i << " / " << n << " ("
                  << (100.0 * double(i) / double(n)) << "%)"
                  << std::endl;

        next_progress += one_pct;
      }
    }

    // read i-th entry
    auto const sa_i = sa.read();
    auto const lcp_i = lcp.read();

    // find which query output to write to
    auto const x = std::min(std::bit_width(lcp_i), (uint64_t)max_lcp_exp);
    if(count[x] < options.limit) {
      ++count[x];

      // write query to file
      out[x] << sa_prev << "\n";
      out[x] << sa_i << "\n";
    }

    // keep SA entry
    sa_prev = sa_i;
  }

  // close inputs
  close(fd_lcp);
  close(fd_sa);

  // result
  std::cout << "Done:" << std::endl;
  for(size_t x = 0; x <= max_lcp_exp; x++) {
    std::cout << "\tQueries for LCP < 2^" << x << ": "
              << count[x] << std::endl;
  }

  return 0;
}
