#include <gtest/gtest.h>

#include <aegis/observer/observer.hpp>

#include <type_traits>

namespace aegis::observer {
namespace {

static_assert(std::is_trivially_copyable_v<MetricsSnapshot>);
static_assert(std::is_trivially_copyable_v<Command>);

TEST(ObserverTest, SnapshotCopyDoesNotAliasLaterPublish) {
    Observer observer{8};

    MetricsSnapshot first{};
    first.throughput_per_s = 12.5;
    first.approval_rate = 0.75;
    first.p50_ns = 50;
    first.p99_ns = 99;
    first.p99_9_ns = 100;
    first.queue_depth = 3;
    first.running = true;
    observer.publish(first);

    const MetricsSnapshot copy = observer.snapshot();
    EXPECT_DOUBLE_EQ(copy.throughput_per_s, 12.5);
    EXPECT_DOUBLE_EQ(copy.approval_rate, 0.75);
    EXPECT_EQ(copy.p50_ns, 50);
    EXPECT_EQ(copy.p99_ns, 99);
    EXPECT_EQ(copy.p99_9_ns, 100);
    EXPECT_EQ(copy.queue_depth, 3U);
    EXPECT_TRUE(copy.running);

    MetricsSnapshot second{};
    second.throughput_per_s = 99.0;
    second.running = false;
    observer.publish(second);

    EXPECT_DOUBLE_EQ(copy.throughput_per_s, 12.5);
    EXPECT_TRUE(copy.running);
}

TEST(ObserverTest, DrainReturnsEventsInOrderThenEmpty) {
    Observer observer{8};

    TransactionEvent first{};
    first.terminal_id = "TERM0001";
    first.response_code = "00";
    TransactionEvent second{};
    second.terminal_id = "TERM0002";
    second.response_code = "51";

    observer.push(first);
    observer.push(second);

    const auto drained = observer.drain();
    ASSERT_EQ(drained.size(), 2U);
    EXPECT_EQ(drained[0].terminal_id, "TERM0001");
    EXPECT_EQ(drained[0].response_code, "00");
    EXPECT_EQ(drained[1].terminal_id, "TERM0002");
    EXPECT_EQ(drained[1].response_code, "51");
    EXPECT_TRUE(observer.drain().empty());
}

TEST(ObserverTest, FullRingKeepsNewestEvents) {
    Observer observer{2};

    for (int i = 1; i <= 3; ++i) {
        TransactionEvent event{};
        event.response_code = std::to_string(i);
        observer.push(event);
    }

    const auto drained = observer.drain();
    ASSERT_EQ(drained.size(), 2U);
    EXPECT_EQ(drained[0].response_code, "2");
    EXPECT_EQ(drained[1].response_code, "3");
}

TEST(ObserverTest, CommandsRoundTripAsValues) {
    Observer observer{4};

    observer.post(Command{CommandKind::Start, 0});
    observer.post(Command{CommandKind::Stop, 0});
    observer.post(Command{CommandKind::SetWorkerCount, 8});
    observer.post(Command{CommandKind::SetPartitionCount, 4});
    observer.post(Command{CommandKind::InjectSlowIssuer, 500});
    observer.post(Command{CommandKind::ForceTimeouts, 0});
    observer.post(Command{CommandKind::DropConnections, 0});

    const auto taken = observer.take_commands();
    ASSERT_EQ(taken.size(), 7U);
    EXPECT_EQ(taken[0].kind, CommandKind::Start);
    EXPECT_EQ(taken[1].kind, CommandKind::Stop);
    EXPECT_EQ(taken[2].kind, CommandKind::SetWorkerCount);
    EXPECT_EQ(taken[2].count, 8U);
    EXPECT_EQ(taken[3].kind, CommandKind::SetPartitionCount);
    EXPECT_EQ(taken[3].count, 4U);
    EXPECT_EQ(taken[4].kind, CommandKind::InjectSlowIssuer);
    EXPECT_EQ(taken[4].count, 500U);
    EXPECT_EQ(taken[5].kind, CommandKind::ForceTimeouts);
    EXPECT_EQ(taken[6].kind, CommandKind::DropConnections);
    EXPECT_TRUE(observer.take_commands().empty());
}

} // namespace
} // namespace aegis::observer
