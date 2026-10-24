#include <catch_amalgamated.hpp>

#include <cstdint>
#include <fstream>
#include <vector>

#include "daphne/engine/file_bytes.h"
#include "test_util.h"

using daphne::FileBytes;
using daphne::FileCursor;

namespace {

std::vector<uint8_t> write_bytes(const std::filesystem::path& p, size_t n) {
    std::vector<uint8_t> b(n);
    uint32_t x = 12345;
    for (auto& v : b) v = static_cast<uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return b;
}

}  // namespace

TEST_CASE("file bytes: the whole file, read at any offset while it loads") {
    const auto p = test::temp_path("daphne_file_bytes.bin");
    const auto want = write_bytes(p, (size_t{5} << 20) + 123);   // more than two loader chunks
    auto f = FileBytes::open(p);
    REQUIRE(f);
    CHECK(f->size() == static_cast<int64_t>(want.size()));

    std::vector<uint8_t> got(4096);
    // the end first: the reader waits for the loader to get there
    const int64_t tail = f->size() - 100;
    REQUIRE(f->read(tail, got.data(), got.size()) == 100);
    CHECK(std::equal(got.begin(), got.begin() + 100, want.begin() + tail));
    CHECK(f->read(f->size(), got.data(), got.size()) == 0);
    CHECK(f->read(-1, got.data(), got.size()) == 0);

    FileCursor c;
    c.reset(f);
    std::vector<uint8_t> all;
    for (size_t n; (n = c.read(got.data(), got.size())) > 0;) all.insert(all.end(), got.begin(), got.begin() + static_cast<std::ptrdiff_t>(n));
    CHECK(all == want);
    CHECK(c.tell() == f->size());

    REQUIRE(c.seek(-10, 2));
    CHECK(c.tell() == f->size() - 10);
    REQUIRE(c.seek(4, 1));
    CHECK(c.tell() == f->size() - 6);
    CHECK_FALSE(c.seek(7, 1));   // past the end: the position stays
    CHECK(c.tell() == f->size() - 6);
    CHECK_FALSE(c.seek(-1, 0));
    REQUIRE(c.seek(1000, 0));
    REQUIRE(c.read(got.data(), 1) == 1);
    CHECK(got[0] == want[1000]);

    // the same unchanged file again: the bytes already read
    CHECK(FileBytes::open(p) == f);
    f.reset();
    c.reset(nullptr);
    std::filesystem::remove(p);
}

TEST_CASE("file bytes: a changed file is read again; a missing or empty one is not read ahead") {
    const auto p = test::temp_path("daphne_file_bytes_changed.bin");
    write_bytes(p, 1000);
    auto a = FileBytes::open(p);
    REQUIRE(a);
    const auto want = write_bytes(p, 2000);
    auto b = FileBytes::open(p);
    REQUIRE(b);
    CHECK(b != a);
    std::vector<uint8_t> got(2000);
    REQUIRE(b->read(0, got.data(), got.size()) == 2000);
    CHECK(got == want);
    a.reset();
    b.reset();
    std::filesystem::remove(p);

    CHECK_FALSE(FileBytes::open(test::temp_path("daphne_file_bytes_missing.bin")));
    const auto e = test::temp_path("daphne_file_bytes_empty.bin");
    write_bytes(e, 0);
    CHECK_FALSE(FileBytes::open(e));
    std::filesystem::remove(e);
}
