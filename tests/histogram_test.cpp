#include <gtest/gtest.h>

#include <aegis/metrics/histogram.hpp>

#include <chrono>

namespace aegis::metrics {
namespace {

TEST(HistogramTest, EmptyHistogramHasNoPercentile) {
    LatencyHistogram histogram;
    EXPECT_FALSE(histogram.percentile(Percentile::P50).has_value());
    EXPECT_EQ(histogram.percentile(Percentile::P50).error(), HistogramError::Empty);
}

TEST(HistogramTest, SingleSampleIsEveryPercentile) {
    LatencyHistogram histogram;
    histogram.record(std::chrono::nanoseconds{5});

    const auto p50 = histogram.percentile(Percentile::P50);
    const auto p99 = histogram.percentile(Percentile::P99);
    const auto p999 = histogram.percentile(Percentile::P999);
    ASSERT_TRUE(p50.has_value());
    ASSERT_TRUE(p99.has_value());
    ASSERT_TRUE(p999.has_value());
    EXPECT_EQ(p50.value(), std::chrono::nanoseconds{5});
    EXPECT_EQ(p99.value(), std::chrono::nanoseconds{5});
    EXPECT_EQ(p999.value(), std::chrono::nanoseconds{5});
}

TEST(HistogramTest, KnownSamplesMatchAcceptancePercentiles) {
    LatencyHistogram histogram;
    for (int i = 1; i <= 100; ++i) {
        histogram.record(std::chrono::nanoseconds{i});
    }

    const auto p50 = histogram.percentile(Percentile::P50);
    const auto p99 = histogram.percentile(Percentile::P99);
    const auto p999 = histogram.percentile(Percentile::P999);
    ASSERT_TRUE(p50.has_value());
    ASSERT_TRUE(p99.has_value());
    ASSERT_TRUE(p999.has_value());
    EXPECT_EQ(p50.value(), std::chrono::nanoseconds{50});
    EXPECT_EQ(p99.value(), std::chrono::nanoseconds{99});
    EXPECT_EQ(p999.value(), std::chrono::nanoseconds{100});
}

TEST(HistogramTest, CountMatchesRecordCalls) {
    LatencyHistogram histogram;
    EXPECT_EQ(histogram.count(), 0U);
    histogram.record(std::chrono::nanoseconds{1});
    histogram.record(std::chrono::nanoseconds{2});
    histogram.record(std::chrono::nanoseconds{3});
    EXPECT_EQ(histogram.count(), 3U);
}

} // namespace
} // namespace aegis::metrics
