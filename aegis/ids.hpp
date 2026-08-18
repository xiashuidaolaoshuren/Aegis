#pragma once

#include <aegis/tagged.hpp>

#include <cstdint>
#include <string>

namespace aegis {

struct AccountIdTag {};
struct MerchantIdTag {};
struct TerminalIdTag {};
struct StanTag {};
struct RrnTag {};

using AccountId = Tagged<std::string, AccountIdTag>;
using MerchantId = Tagged<std::string, MerchantIdTag>;
using TerminalId = Tagged<std::string, TerminalIdTag>;
using Stan = Tagged<std::uint64_t, StanTag>;
using Rrn = Tagged<std::string, RrnTag>;

class Pan {
public:
    explicit Pan(std::string value) : value_(std::move(value)) {}

    [[nodiscard]] std::string masked() const {
        if (value_.size() <= 10) {
            return value_;
        }
        return value_.substr(0, 6) + "******" + value_.substr(value_.size() - 4);
    }

    [[nodiscard]] const std::string& full() const {
        return value_;
    }

private:
    std::string value_;
};

} // namespace aegis
