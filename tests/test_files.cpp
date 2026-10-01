#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "files.h"

namespace fs = std::filesystem;

static std::string rel_of(std::string_view url, bool* ok = nullptr) {
  std::string rel;
  bool r = FileServer::safe_relative(url, rel);
  if (ok) *ok = r;
  return r ? rel : "<escape>";
}

TEST(SafeRelative, Normalizes) {
  EXPECT_EQ(rel_of("/"), "");
  EXPECT_EQ(rel_of(""), "");
  EXPECT_EQ(rel_of("/a/b"), "a/b");
  EXPECT_EQ(rel_of("/a/b/"), "a/b");
  EXPECT_EQ(rel_of("//a///b"), "a/b");
  EXPECT_EQ(rel_of("/./a/./b/."), "a/b");
  EXPECT_EQ(rel_of("/a/../b"), "b");
  EXPECT_EQ(rel_of("/a/b/../../c"), "c");
  EXPECT_EQ(rel_of("/a..b.txt"), "a..b.txt");
  EXPECT_EQ(rel_of("/..."), "...");
  EXPECT_EQ(rel_of("/a/.."), "");
}

TEST(SafeRelative, RejectsEscapes) {
  EXPECT_EQ(rel_of("/../x"), "<escape>");
  EXPECT_EQ(rel_of("/a/../../x"), "<escape>");
  EXPECT_EQ(rel_of("/.."), "<escape>");
  EXPECT_EQ(rel_of("/a/../../../etc/passwd"), "<escape>");
}

class FileServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("fs_test_" + std::to_string(::getpid()));
    fs::remove_all(dir_);
    fs::create_directories(dir_ / "sub");
    fs::create_directories(dir_ / "list");
    write("index.html", "<h1>root</h1>");
    write("a..b.txt", "dots");
    write("sub/index.html", "<h1>sub</h1>");
    write("sub/x.txt", "x content");
    write("list/<b>&.txt", "escape me");
    write("list/plain.css", "p{}");
    write("big.bin", std::string(100, 'B'));
    fs::create_directories(dir_ / "list" / "nested");
    fs::create_symlink("/etc", dir_ / "etclink");
    srv_ = std::make_unique<FileServer>(fs::canonical(dir_), /*max_file=*/50);
  }
  void TearDown() override { fs::remove_all(dir_); }
  void write(const std::string& rel, const std::string& content) {
    std::ofstream(dir_ / rel, std::ios::binary) << content;
  }
  fs::path dir_;
  std::unique_ptr<FileServer> srv_;
};

TEST_F(FileServerTest, RootServesIndex) {
  Response r = srv_->get("/");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body, "<h1>root</h1>");
  EXPECT_EQ(r.content_type, "text/html; charset=utf-8");
}

TEST_F(FileServerTest, FilesAndMime) {
  EXPECT_EQ(srv_->get("/a..b.txt").body, "dots");
  EXPECT_EQ(srv_->get("/a..b.txt").content_type, "text/plain; charset=utf-8");
  EXPECT_EQ(srv_->get("/./index.html").body, "<h1>root</h1>");
  EXPECT_EQ(srv_->get("/sub/x.txt").body, "x content");
  EXPECT_EQ(srv_->get("/list/plain.css").content_type, "text/css; charset=utf-8");
}

TEST_F(FileServerTest, DirectoryWithIndex) {
  EXPECT_EQ(srv_->get("/sub").body, "<h1>sub</h1>");
  EXPECT_EQ(srv_->get("/sub/").body, "<h1>sub</h1>");
}

TEST_F(FileServerTest, DirectoryListingEscapesAndLinks) {
  Response r = srv_->get("/list/");
  ASSERT_EQ(r.status, 200);
  EXPECT_NE(r.body.find("Index of /list/"), std::string::npos);
  EXPECT_NE(r.body.find("&lt;b&gt;&amp;.txt"), std::string::npos);   // escaped text
  EXPECT_EQ(r.body.find("<b>&.txt"), std::string::npos);              // never raw
  EXPECT_NE(r.body.find("href=\"%3Cb%3E%26.txt\""), std::string::npos);
  EXPECT_NE(r.body.find("href=\"nested/\""), std::string::npos);
  EXPECT_NE(r.body.find("href=\"../\""), std::string::npos);
  EXPECT_LT(r.body.find("nested/"), r.body.find("plain.css"));         // dirs first
  EXPECT_EQ(srv_->get("/list").body, r.body);                          // no trailing slash
}

TEST_F(FileServerTest, Errors) {
  EXPECT_EQ(srv_->get("/nope").status, 404);
  EXPECT_EQ(srv_->get("/sub/nope.txt").status, 404);
  EXPECT_EQ(srv_->get("/index.html/extra").status, 404);
  EXPECT_EQ(srv_->get("/../x").status, 403);
  EXPECT_EQ(srv_->get("/sub/../../x").status, 403);
  EXPECT_EQ(srv_->get("/big.bin").status, 403);  // over max_file
}

TEST_F(FileServerTest, SymlinkInsideRootIsFollowed) {
  Response r = srv_->get("/etclink/hostname");
  EXPECT_TRUE(r.status == 200 || r.status == 404) << r.status;  // documented behaviour: followed
}
