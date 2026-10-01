#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "http.h"

class FileServer {
 public:
  // root must already be canonical (see Server::run). Files above max_file bytes get 403.
  explicit FileServer(std::filesystem::path root, size_t max_file = 50u << 20)
      : root_(std::move(root)), max_file_(max_file) {}

  Response get(std::string_view decoded_path) const;  // 200 | listing | 403 | 404 | 500
  const std::filesystem::path& root() const { return root_; }

  // Lexically resolves a decoded URL path to a relative path under the root
  // ("" for the root itself). False when it escapes the root. Only whole
  // components are special, so "a..b.txt" is an ordinary file name.
  static bool safe_relative(std::string_view decoded, std::string& rel);

 private:
  Response serve_file(const std::filesystem::path& p) const;
  Response listing(const std::filesystem::path& dir, std::string_view url_path) const;

  std::filesystem::path root_;
  size_t max_file_;
};
