#include <gtest/gtest.h>

#include <aegis/concurrent/bounded_queue.hpp>
#include <aegis/concurrent/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <stop_token>
#include <thread>

namespace aegis::concurrent {
namespace {

TEST(QueueBackpressureTest, PushPastCapacityFailsAndSizeStaysAtCap) {
    BoundedQueue<int> queue{2};

    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    EXPECT_EQ(queue.size(), 2U);

    EXPECT_FALSE(queue.push(3));
    EXPECT_EQ(queue.size(), 2U);
}

TEST(QueueBackpressureTest, PopReturnsQueuedItemsInFifoOrder) {
    BoundedQueue<int> queue{3};
    ASSERT_TRUE(queue.push(10));
    ASSERT_TRUE(queue.push(20));
    ASSERT_TRUE(queue.push(30));

    const std::stop_token stop_token{};
    const auto first = queue.pop(stop_token);
    const auto second = queue.pop(stop_token);
    const auto third = queue.pop(stop_token);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(first.value(), 10);
    EXPECT_EQ(second.value(), 20);
    EXPECT_EQ(third.value(), 30);
    EXPECT_EQ(queue.size(), 0U);
}

TEST(QueueBackpressureTest, WaitingPopReturnsValuePushedByAnotherThread) {
    BoundedQueue<int> queue{1};
    std::optional<int> result;

    std::jthread consumer{[&](std::stop_token) {
        result = queue.pop(std::stop_token{});
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    ASSERT_TRUE(queue.push(42));

    consumer.join();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42);
}

TEST(QueueBackpressureTest, StopTokenUnblocksEmptyPopWithNullopt) {
    BoundedQueue<int> queue{2};
    std::optional<int> result;
    std::atomic<bool> finished{false};

    std::jthread consumer{[&](std::stop_token stop) {
        result = queue.pop(stop);
        finished.store(true);
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    consumer.request_stop();
    consumer.join();

    EXPECT_TRUE(finished.load());
    EXPECT_FALSE(result.has_value());
}

TEST(QueueBackpressureTest, SubmittedJobsAllRun) {
    std::atomic<int> counter{0};
    constexpr int job_count = 8;

    {
        ThreadPool pool{2, 8};
        for (int i = 0; i < job_count; ++i) {
            ASSERT_TRUE(pool.submit([&counter] { counter.fetch_add(1); }));
        }
    }

    EXPECT_EQ(counter.load(), job_count);
}

TEST(QueueBackpressureTest, ShutdownReturnsAndLaterSubmitFails) {
    ThreadPool pool{2, 4};
    ASSERT_TRUE(pool.submit([] {}));

    pool.shutdown();
    EXPECT_FALSE(pool.submit([] {}));
}

} // namespace
} // namespace aegis::concurrent
