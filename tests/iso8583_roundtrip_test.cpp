#include <gtest/gtest.h>

#include <aegis/iso8583/codec.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace aegis::iso8583 {
namespace {

std::vector<std::filesystem::path> corpus_files() {
    std::vector<std::filesystem::path> files;
#ifdef CORPUS_DIR
    const std::filesystem::path dir{CORPUS_DIR};
    if (!std::filesystem::is_directory(dir)) {
        return files;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".bin") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
#endif
    return files;
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::transform(raw.begin(), raw.end(), bytes.begin(), [](char c) {
        return static_cast<std::byte>(static_cast<unsigned char>(c));
    });
    return bytes;
}

TEST(Iso8583RoundTrip, CorpusIsNonEmpty) {
    const auto files = corpus_files();
    ASSERT_FALSE(files.empty()) << "no .bin seeds in corpus dir";
}

TEST(Iso8583RoundTrip, SerialiseParseEqualsOriginalForEverySeed) {
    const auto files = corpus_files();
    ASSERT_FALSE(files.empty()) << "no .bin seeds in corpus dir";
    for (const auto& path : files) {
        SCOPED_TRACE(path.filename().string());
        const auto bytes = read_file(path);
        const auto parsed = parse(bytes);
        ASSERT_TRUE(parsed.has_value()) << "parse failed for " << path.filename().string()
                                        << " error=" << static_cast<int>(parsed.error());
        const auto encoded = serialise(parsed.value());
        ASSERT_TRUE(encoded.has_value()) << "serialise failed for " << path.filename().string();
        EXPECT_EQ(encoded.value(), bytes) << "round-trip mismatch for " << path.filename().string();
    }
}

} // namespace
} // namespace aegis::iso8583
