#include <gtest/gtest.h>

#include <aegis/authorizer/authorizer.hpp>
#include <aegis/authorizer/screen.hpp>
#include <aegis/engine/engine.hpp>
#include <aegis/ids.hpp>
#include <aegis/iso8583/codec.hpp>
#include <aegis/iso8583/fields.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/issuersim/issuersim.hpp>
#include <aegis/ledger/genesis.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/idempotency.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/ledger/striped.hpp>
#include <aegis/money.hpp>
#include <aegis/net/framing.hpp>
#include <aegis/net/socket.hpp>
#include <shadow_model.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace aegis::ledger {
namespace {

constexpr std::size_t kCardholders = 8;
constexpr std::int64_t kOpeningMinor = 1'000'000;
constexpr std::int64_t kReserveMinor = 1000;
constexpr std::size_t kReservesPerCardholder = 25;
constexpr std::size_t kSeededPerCardholder = 50;

AccountId account(std::string id) {
    return AccountId{std::move(id)};
}

std::string cardholder_pan(std::size_t index) {
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "42424242%08zu", index);
    return std::string{buffer};
}

std::string terminal_id(std::size_t index) {
    char buffer[9] = {};
    std::snprintf(buffer, sizeof(buffer), "TERM%04zu", index);
    return std::string{buffer};
}

iso8583::Message make_auth_request(std::string_view pan, std::string_view terminal, std::size_t stan) {
    using iso8583::FieldId;
    using iso8583::Message;
    using iso8583::Mti;

    Message request{Mti::AuthorizationRequest};
    (void)request.set(FieldId::Pan, std::string{pan});
    (void)request.set(FieldId::Amount, "000000001000");
    (void)request.set(FieldId::TransmissionDateTime, "1002153045");

    char stan_text[7] = {};
    std::snprintf(stan_text, sizeof(stan_text), "%06zu", stan % 1000000U);
    (void)request.set(FieldId::Stan, std::string{stan_text});

    (void)request.set(FieldId::TerminalId, std::string{terminal});
    (void)request.set(FieldId::MerchantId, std::string{"m-1"} + std::string(12, ' '));
    (void)request.set(FieldId::Currency, "840");
    return request;
}

std::vector<GenesisRecord> stage2_records() {
    std::vector<GenesisRecord> records;
    records.reserve(kCardholders + 2);
    for (std::size_t i = 0; i < kCardholders; ++i) {
        records.push_back(GenesisRecord{
            WalletKind::Cardholder,
            AccountId{cardholder_pan(i)},
            Currency::Usd,
            kOpeningMinor,
        });
    }
    records.push_back(GenesisRecord{WalletKind::Merchant, AccountId{"m-1"}, Currency::Usd, 0});
    records.push_back(GenesisRecord{WalletKind::System, AccountId{"system"}, Currency::Usd, 0});
    return records;
}

Ledger make_stage2_ledger() {
    const std::vector<GenesisRecord> records = stage2_records();
    auto wallets = load_genesis(records);
    EXPECT_TRUE(wallets.has_value());
    return Ledger{std::move(wallets.value())};
}

ShadowLedger make_stage2_shadow() {
    const std::vector<GenesisRecord> records = stage2_records();
    auto wallets = load_genesis(records);
    EXPECT_TRUE(wallets.has_value());
    return ShadowLedger{std::move(wallets.value())};
}

void expect_matches_shadow(const Ledger& ledger, const ShadowLedger& shadow) {
    const Bucket buckets[] = {Bucket::Available, Bucket::Holds, Bucket::Payable, Bucket::Interchange};

    for (std::size_t i = 0; i < kCardholders; ++i) {
        const AccountId cardholder = account(cardholder_pan(i));
        for (const Bucket bucket : buckets) {
            const auto actual = ledger.balance(cardholder, bucket);
            const auto expected = shadow.balance(cardholder, bucket);
            ASSERT_TRUE(actual.has_value());
            ASSERT_TRUE(expected.has_value());
            EXPECT_EQ(actual.value(), expected.value())
                << "cardholder " << i << " bucket " << static_cast<int>(bucket);
        }
    }

    for (const AccountId id : {account("m-1"), account("system")}) {
        for (const Bucket bucket : buckets) {
            const auto actual = ledger.balance(id, bucket);
            const auto expected = shadow.balance(id, bucket);
            ASSERT_TRUE(actual.has_value());
            ASSERT_TRUE(expected.has_value());
            EXPECT_EQ(actual.value(), expected.value())
                << id.value() << " bucket " << static_cast<int>(bucket);
        }
    }

    EXPECT_EQ(ledger.live_hold_count(), shadow.live_hold_count());
}

} // namespace

TEST(Stage2Test, StripeMappingIsStableAndOrdered) {
    const StripedLocks locks{StripedLocks::kDefaultStripeCount};

    EXPECT_EQ(locks.size(), StripedLocks::kDefaultStripeCount);

    const AccountId a = account("4242424200000000");
    const AccountId b = account("m-1");
    const AccountId c = account("system");

    EXPECT_EQ(locks.stripe_of(a), locks.stripe_of(a));
    for (const AccountId& id : {a, b, c}) {
        EXPECT_LT(locks.stripe_of(id), locks.size());
    }

    const std::vector<std::size_t> indices = locks.stripe_indices({a, b, c, a});
    EXPECT_TRUE(std::is_sorted(indices.begin(), indices.end()));
    EXPECT_EQ(indices, locks.stripe_indices({c, b, a}));
    EXPECT_EQ(locks.stripe_indices({a, a, a}).size(), 1U);

    std::vector<std::size_t> expected{locks.stripe_of(a), locks.stripe_of(b), locks.stripe_of(c)};
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
    EXPECT_EQ(indices, expected);
}

TEST(Stage2Test, AccountsSharingAStripeTakeOneLock) {
    const StripedLocks locks{StripedLocks::kDefaultStripeCount};

    const AccountId first = account("probe-0");
    std::string second_id;
    bool found = false;
    for (std::size_t i = 1; i < 512 && !found; ++i) {
        std::string candidate = "probe-" + std::to_string(i);
        if (locks.stripe_of(AccountId{candidate}) == locks.stripe_of(first)) {
            second_id = candidate;
            found = true;
        }
    }
    ASSERT_TRUE(found) << "expected two account ids to share a stripe within 512 probes";

    const AccountId second = account(second_id);
    const std::vector<std::size_t> indices = locks.stripe_indices({first, second});
    ASSERT_EQ(indices.size(), 1U);
    EXPECT_EQ(indices.front(), locks.stripe_of(first));
}

TEST(Stage2Test, ParallelReservesOnDistinctCardholdersMatchShadow) {
    Ledger ledger = make_stage2_ledger();
    ShadowLedger shadow = make_stage2_shadow();

    const Money amount{Currency::Usd, kReserveMinor};
    const MerchantId merchant_id{"m-1"};
    for (std::size_t i = 0; i < kCardholders; ++i) {
        for (std::size_t n = 0; n < kReservesPerCardholder; ++n) {
            const auto replayed = shadow.reserve(account(cardholder_pan(i)), amount, merchant_id);
            ASSERT_TRUE(replayed.has_value());
        }
    }

    std::latch start{kCardholders};
    std::atomic<std::size_t> failures{0};
    std::vector<std::jthread> threads;
    threads.reserve(kCardholders);

    for (std::size_t t = 0; t < kCardholders; ++t) {
        threads.emplace_back([&ledger, &start, &failures, t, amount, merchant_id] {
            start.arrive_and_wait();
            const AccountId cardholder = account(cardholder_pan(t));
            for (std::size_t n = 0; n < kReservesPerCardholder; ++n) {
                if (!ledger.reserve(cardholder, amount, merchant_id).has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    threads.clear();

    EXPECT_EQ(failures.load(), 0U);
    expect_matches_shadow(ledger, shadow);
    EXPECT_EQ(ledger.live_hold_count(), kCardholders * kReservesPerCardholder);
}

TEST(Stage2Test, ParallelCapturesAcrossSharedStripesMatchShadow) {
    Ledger ledger = make_stage2_ledger();
    ShadowLedger shadow = make_stage2_shadow();

    const Money amount{Currency::Usd, kReserveMinor};
    const MerchantId merchant_id{"m-1"};

    // Pre-seeded holds that the capture threads consume, interleaved with threads that keep
    // reserving. That mix is what the engine actually runs, and it mutates the holds map while
    // captures read and erase from it.
    std::vector<HoldId> seeded_ids;
    seeded_ids.reserve(kCardholders * kSeededPerCardholder);
    for (std::size_t n = 0; n < kSeededPerCardholder; ++n) {
        for (std::size_t i = 0; i < kCardholders; ++i) {
            const AccountId cardholder = account(cardholder_pan(i));
            const auto hold = ledger.reserve(cardholder, amount, merchant_id);
            const auto replayed = shadow.reserve(cardholder, amount, merchant_id);
            ASSERT_TRUE(hold.has_value());
            ASSERT_TRUE(replayed.has_value());
            ASSERT_EQ(hold.value().id, replayed.value().id);
            seeded_ids.push_back(hold.value().id);
        }
    }

    constexpr std::size_t kCaptureThreads = 4;
    constexpr std::size_t kReserveThreads = 4;
    constexpr std::size_t kWorkerCount = kCaptureThreads + kReserveThreads;

    std::latch start{kWorkerCount};
    std::atomic<std::size_t> failures{0};
    std::vector<std::jthread> threads;
    threads.reserve(kWorkerCount);

    for (std::size_t t = 0; t < kCaptureThreads; ++t) {
        threads.emplace_back([&ledger, &seeded_ids, &start, &failures, t, amount] {
            start.arrive_and_wait();
            for (std::size_t index = t; index < seeded_ids.size(); index += kCaptureThreads) {
                if (!ledger.capture(seeded_ids[index], amount).has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    for (std::size_t t = 0; t < kReserveThreads; ++t) {
        threads.emplace_back([&ledger, &start, &failures, t, amount, merchant_id] {
            start.arrive_and_wait();
            for (std::size_t n = 0; n < kReservesPerCardholder; ++n) {
                const AccountId cardholder = account(cardholder_pan(t));
                if (!ledger.reserve(cardholder, amount, merchant_id).has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    threads.clear();

    EXPECT_EQ(failures.load(), 0U);

    for (const HoldId id : seeded_ids) {
        ASSERT_TRUE(shadow.capture(id, amount).has_value());
    }
    for (std::size_t t = 0; t < kReserveThreads; ++t) {
        for (std::size_t n = 0; n < kReservesPerCardholder; ++n) {
            ASSERT_TRUE(shadow.reserve(account(cardholder_pan(t)), amount, merchant_id).has_value());
        }
    }

    expect_matches_shadow(ledger, shadow);
    EXPECT_EQ(ledger.live_hold_count(), kReserveThreads * kReservesPerCardholder);
}

TEST(Stage2Test, ParallelReverseAndExpiryMatchShadow) {
    Ledger ledger = make_stage2_ledger();
    ShadowLedger shadow = make_stage2_shadow();

    const Money amount{Currency::Usd, kReserveMinor};
    const MerchantId merchant_id{"m-1"};

    // Zero TTL: these holds are due the moment they exist and belong to the sweeper.
    ledger.set_hold_ttl(std::chrono::seconds{0});
    shadow.set_hold_ttl(std::chrono::seconds{0});
    std::vector<HoldId> expiring_ids;
    expiring_ids.reserve(kCardholders * kSeededPerCardholder);
    for (std::size_t n = 0; n < kSeededPerCardholder; ++n) {
        for (std::size_t i = 0; i < kCardholders; ++i) {
            const AccountId cardholder = account(cardholder_pan(i));
            const auto hold = ledger.reserve(cardholder, amount, merchant_id);
            const auto replayed = shadow.reserve(cardholder, amount, merchant_id);
            ASSERT_TRUE(hold.has_value());
            ASSERT_TRUE(replayed.has_value());
            ASSERT_EQ(hold.value().id, replayed.value().id);
            expiring_ids.push_back(hold.value().id);
        }
    }

    // A long TTL keeps these alive so the threads can reverse them.
    ledger.set_hold_ttl(std::chrono::hours{24});
    shadow.set_hold_ttl(std::chrono::hours{24});
    std::vector<HoldId> live_ids;
    live_ids.reserve(kCardholders * kReservesPerCardholder);
    for (std::size_t n = 0; n < kReservesPerCardholder; ++n) {
        for (std::size_t i = 0; i < kCardholders; ++i) {
            const AccountId cardholder = account(cardholder_pan(i));
            const auto hold = ledger.reserve(cardholder, amount, merchant_id);
            const auto replayed = shadow.reserve(cardholder, amount, merchant_id);
            ASSERT_TRUE(hold.has_value());
            ASSERT_TRUE(replayed.has_value());
            ASSERT_EQ(hold.value().id, replayed.value().id);
            live_ids.push_back(hold.value().id);
        }
    }

    std::latch start{kCardholders};
    std::atomic<std::size_t> failures{0};
    std::vector<std::jthread> threads;
    threads.reserve(kCardholders);
    for (std::size_t t = 0; t < kCardholders; ++t) {
        threads.emplace_back([&ledger, &live_ids, &start, &failures, t] {
            start.arrive_and_wait();
            for (std::size_t index = t; index < live_ids.size(); index += kCardholders) {
                if (!ledger.reverse(live_ids[index]).has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    threads.clear();

    EXPECT_EQ(failures.load(), 0U);

    const auto sweep_time = std::chrono::system_clock::now();
    EXPECT_EQ(ledger.expire_due(sweep_time), expiring_ids.size());

    for (const HoldId id : live_ids) {
        ASSERT_TRUE(shadow.reverse(id).has_value());
    }
    EXPECT_EQ(shadow.expire_due(sweep_time), expiring_ids.size());

    expect_matches_shadow(ledger, shadow);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(Stage2Test, ConcurrentIdempotencyStoresDistinctKeys) {
    IdempotencyStore store;

    constexpr std::size_t kStoreThreads = 8;
    constexpr std::size_t kKeysPerThread = 50;

    std::latch start{kStoreThreads};
    std::vector<std::jthread> threads;
    threads.reserve(kStoreThreads);

    for (std::size_t t = 0; t < kStoreThreads; ++t) {
        threads.emplace_back([&store, &start, t] {
            start.arrive_and_wait();
            for (std::size_t n = 0; n < kKeysPerThread; ++n) {
                const IdempotencyKey key{
                    TerminalId{terminal_id(t)},
                    Stan{n},
                    Mmdd{1023},
                };
                store.store_reserve(
                    key, Result<Hold, LedgerError>::err(LedgerError::InsufficientFunds));
            }
        });
    }
    threads.clear();

    EXPECT_EQ(store.size(), kStoreThreads * kKeysPerThread);

    // Lookups are copies, so they stay valid while other threads keep inserting.
    const IdempotencyKey probe{TerminalId{terminal_id(kStoreThreads - 1)}, Stan{kKeysPerThread - 1}, Mmdd{1023}};
    const auto found = store.find_reserve(probe);
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->has_value());
    EXPECT_EQ(found->error(), LedgerError::InsufficientFunds);

    const IdempotencyKey missing{TerminalId{terminal_id(99)}, Stan{0}, Mmdd{1023}};
    EXPECT_FALSE(store.find_reserve(missing).has_value());
}

TEST(Stage2Test, ConcurrentAuthorizerHandleDistinctKeys) {
    Ledger ledger = make_stage2_ledger();
    ShadowLedger shadow = make_stage2_shadow();
    IdempotencyStore store;
    issuersim::IssuerSim issuer{};
    authorizer::ScreenConfig screen{};
    authorizer::Authorizer authorizer{ledger, store, issuer, screen};

    constexpr std::size_t kRequestsPerTerminal = 500;

    std::latch start{kCardholders};
    std::atomic<std::size_t> failures{0};
    std::vector<std::jthread> threads;
    threads.reserve(kCardholders);

    for (std::size_t t = 0; t < kCardholders; ++t) {
        threads.emplace_back([&authorizer, &start, &failures, t] {
            start.arrive_and_wait();
            for (std::size_t n = 0; n < kRequestsPerTerminal; ++n) {
                const iso8583::Message request =
                    make_auth_request(cardholder_pan(t), terminal_id(t), (t * 1000) + n);
                const iso8583::Message response = authorizer.handle(request);
                const auto code = response.get(iso8583::FieldId::ResponseCode);
                if (!code.has_value() || code.value() != "00") {
                    failures.fetch_add(1);
                }
            }
        });
    }
    threads.clear();

    EXPECT_EQ(failures.load(), 0U);
    EXPECT_EQ(issuer.calls, kCardholders * kRequestsPerTerminal);
    EXPECT_EQ(ledger.live_hold_count(), kCardholders * kRequestsPerTerminal);

    const Money amount{Currency::Usd, kReserveMinor};
    const MerchantId merchant_id{"m-1"};
    for (std::size_t t = 0; t < kCardholders; ++t) {
        for (std::size_t n = 0; n < kRequestsPerTerminal; ++n) {
            ASSERT_TRUE(shadow.reserve(account(cardholder_pan(t)), amount, merchant_id).has_value());
        }
    }

    expect_matches_shadow(ledger, shadow);
}

TEST(Stage2Test, EngineStageTwoHandlesConcurrentAuth) {
    engine::EngineConfig config{};
    config.genesis_path = std::filesystem::path{FIXTURES_DIR} / "genesis_stage2.csv";
    config.io_threads = 2;
    config.worker_count = 4;
    config.inbound_capacity = 64;
    config.ledger_stage = engine::LedgerStage::Striped;

    engine::Engine server;
    ASSERT_TRUE(server.start(config).has_value());

    constexpr std::size_t kClients = 4;
    constexpr std::size_t kRequestsPerClient = 25;

    std::latch start{kClients};
    std::atomic<std::size_t> failures{0};
    std::vector<std::jthread> clients;
    clients.reserve(kClients);

    for (std::size_t t = 0; t < kClients; ++t) {
        clients.emplace_back([&server, &start, &failures, t] {
            start.arrive_and_wait();

            for (std::size_t n = 0; n < kRequestsPerClient; ++n) {
                // The engine answers one request per connection, so every authorization dials in.
                net::Connection client;
                if (!client.connect_loopback(server.port()).has_value()) {
                    failures.fetch_add(1);
                    continue;
                }

                const iso8583::Message request =
                    make_auth_request(cardholder_pan(t), terminal_id(t), (t * 1000) + n);
                const auto payload = iso8583::serialise(request);
                if (!payload.has_value() ||
                    !net::write_frame(client, payload.value()).has_value()) {
                    failures.fetch_add(1);
                    continue;
                }

                client.set_receive_timeout(std::chrono::seconds{5});
                const auto frame = net::read_frame(client);
                if (!frame.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }

                const auto parsed = iso8583::parse(frame.value());
                if (!parsed.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }

                const auto code = parsed.value().get(iso8583::FieldId::ResponseCode);
                if (!code.has_value() || code.value() != "00") {
                    failures.fetch_add(1);
                }
            }
        });
    }
    clients.clear();

    EXPECT_EQ(failures.load(), 0U);
    server.stop();
}

} // namespace aegis::ledger
