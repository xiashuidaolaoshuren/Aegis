#pragma once

#include <aegis/authorizer/screen.hpp>
#include <aegis/ids.hpp>
#include <aegis/iso8583/fields.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/issuersim/issuersim.hpp>
#include <aegis/ledger/idempotency.hpp>
#include <aegis/ledger/ledger.hpp>

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aegis::authorizer {

class Authorizer {
public:
    Authorizer(ledger::Ledger& ledger,
                 ledger::IdempotencyStore& store,
                 issuersim::IssuerSim& issuer,
                 ScreenConfig& screen)
        : ledger_(ledger), store_(store), issuer_(issuer), screen_(screen) {}

    [[nodiscard]] iso8583::Message handle(const iso8583::Message& request) {
        if (const auto invalid = validate(request)) {
            return make_response(request, *invalid);
        }

        const ledger::IdempotencyKey key = make_key(request);
        if (const auto cached = find_cached_response(key)) {
            return make_response(request, *cached);
        }

        if (!screen(request, ledger_, screen_).has_value()) {
            return finish(request, key, "05");
        }

        const AccountId cardholder{request.get(iso8583::FieldId::Pan).value()};
        const Money amount = parse_money(request);
        const MerchantId merchant = parse_merchant(request);

        const auto reserve = ledger_.reserve(cardholder, amount, merchant);
        store_.store_reserve(key, reserve);

        if (!reserve.has_value()) {
            if (reserve.error() == ledger::LedgerError::InsufficientFunds) {
                return finish(request, key, "51");
            }
            return finish(request, key, "05");
        }

        const issuersim::Decision decision =
            issuersim::await_decision(issuer_, issuersim::WaitFn{});
        if (decision == issuersim::Decision::Approved) {
            return finish(request, key, "00");
        }

        (void)ledger_.reverse(reserve.value().id);
        return finish(request, key, "05");
    }

private:
    ledger::Ledger& ledger_;
    ledger::IdempotencyStore& store_;
    issuersim::IssuerSim& issuer_;
    ScreenConfig& screen_;
    std::unordered_map<ledger::IdempotencyKey, std::string, ledger::IdempotencyKeyHash>
        response_cache_;

    [[nodiscard]] const std::string* find_cached_response(const ledger::IdempotencyKey& key) const {
        const auto it = response_cache_.find(key);
        if (it == response_cache_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    [[nodiscard]] iso8583::Message finish(const iso8583::Message& request,
                                          const ledger::IdempotencyKey& key,
                                          std::string_view response_code) {
        response_cache_[key] = std::string{response_code};
        return make_response(request, response_code);
    }

    [[nodiscard]] static bool is_digits(std::string_view value) {
        if (value.empty()) {
            return false;
        }
        for (const char c : value) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static std::optional<std::string> validate(const iso8583::Message& request) {
        if (request.mti() != iso8583::Mti::AuthorizationRequest) {
            return std::string{"30"};
        }

        const iso8583::FieldId required_fields[] = {
            iso8583::FieldId::Pan,
            iso8583::FieldId::Amount,
            iso8583::FieldId::TransmissionDateTime,
            iso8583::FieldId::Stan,
            iso8583::FieldId::TerminalId,
            iso8583::FieldId::MerchantId,
            iso8583::FieldId::Currency,
        };

        for (const iso8583::FieldId field : required_fields) {
            if (!request.has(field)) {
                return std::string{"30"};
            }
            const auto value = request.get(field);
            if (!value.has_value() || value.value().empty()) {
                return std::string{"30"};
            }
        }

        const auto amount = request.get(iso8583::FieldId::Amount);
        if (!amount.has_value() || !is_digits(amount.value()) || amount.value().size() != 12U) {
            return std::string{"30"};
        }

        const auto transmission = request.get(iso8583::FieldId::TransmissionDateTime);
        if (!transmission.has_value() || !is_digits(transmission.value()) ||
            transmission.value().size() != 10U) {
            return std::string{"30"};
        }

        const auto stan = request.get(iso8583::FieldId::Stan);
        if (!stan.has_value() || !is_digits(stan.value()) || stan.value().size() != 6U) {
            return std::string{"30"};
        }

        const auto currency = request.get(iso8583::FieldId::Currency);
        if (!currency.has_value() ||
            !detail::parse_currency_code(currency.value()).has_value()) {
            return std::string{"30"};
        }

        return std::nullopt;
    }

    [[nodiscard]] static std::string trim_merchant(std::string_view value) {
        std::string trimmed{value};
        while (!trimmed.empty() && trimmed.back() == ' ') {
            trimmed.pop_back();
        }
        return trimmed;
    }

    [[nodiscard]] static ledger::IdempotencyKey make_key(const iso8583::Message& request) {
        const auto terminal = request.get(iso8583::FieldId::TerminalId).value();
        const auto stan = request.get(iso8583::FieldId::Stan).value();
        const auto transmission = request.get(iso8583::FieldId::TransmissionDateTime).value();
        const auto mmdd = static_cast<std::uint32_t>(detail::parse_amount_minor(transmission.substr(0, 4)));
        return ledger::IdempotencyKey{
            TerminalId{terminal},
            Stan{static_cast<std::uint64_t>(detail::parse_amount_minor(stan))},
            ledger::Mmdd{mmdd},
        };
    }

    [[nodiscard]] static Money parse_money(const iso8583::Message& request) {
        const auto amount = request.get(iso8583::FieldId::Amount).value();
        const auto currency =
            detail::parse_currency_code(request.get(iso8583::FieldId::Currency).value()).value();
        return Money{currency, detail::parse_amount_minor(amount)};
    }

    [[nodiscard]] static MerchantId parse_merchant(const iso8583::Message& request) {
        return MerchantId{trim_merchant(request.get(iso8583::FieldId::MerchantId).value())};
    }

    [[nodiscard]] static iso8583::Message make_response(const iso8583::Message& request,
                                                      std::string_view response_code) {
        iso8583::Message response{iso8583::Mti::AuthorizationResponse};
        (void)response.set(iso8583::FieldId::ResponseCode, std::string{response_code});

        if (const auto pan = request.get(iso8583::FieldId::Pan); pan.has_value()) {
            (void)response.set(iso8583::FieldId::Pan, pan.value());
        }
        if (const auto amount = request.get(iso8583::FieldId::Amount); amount.has_value()) {
            (void)response.set(iso8583::FieldId::Amount, amount.value());
        }
        if (const auto stan = request.get(iso8583::FieldId::Stan); stan.has_value()) {
            (void)response.set(iso8583::FieldId::Stan, stan.value());
        }
        if (const auto terminal = request.get(iso8583::FieldId::TerminalId); terminal.has_value()) {
            (void)response.set(iso8583::FieldId::TerminalId, terminal.value());
        }
        if (const auto merchant = request.get(iso8583::FieldId::MerchantId); merchant.has_value()) {
            (void)response.set(iso8583::FieldId::MerchantId, merchant.value());
        }
        if (const auto currency = request.get(iso8583::FieldId::Currency); currency.has_value()) {
            (void)response.set(iso8583::FieldId::Currency, currency.value());
        }
        if (const auto transmission = request.get(iso8583::FieldId::TransmissionDateTime);
            transmission.has_value()) {
            (void)response.set(iso8583::FieldId::TransmissionDateTime, transmission.value());
        }

        return response;
    }
};

} // namespace aegis::authorizer
