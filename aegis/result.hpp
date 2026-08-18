#pragma once

#include <optional>
#include <utility>

namespace aegis {

template <typename T, typename E>
class Result {
public:
    static Result ok(T value) {
        Result result;
        result.has_value_ = true;
        result.value_.emplace(std::move(value));
        return result;
    }

    static Result err(E error) {
        Result result;
        result.has_value_ = false;
        result.error_.emplace(std::move(error));
        return result;
    }

    [[nodiscard]] bool has_value() const {
        return has_value_;
    }

    [[nodiscard]] const T& value() const {
        return value_.value();
    }

    [[nodiscard]] const E& error() const {
        return error_.value();
    }

private:
    bool has_value_{false};
    std::optional<T> value_;
    std::optional<E> error_;
};

template <typename T, typename E>
Result<T, E> ok(T value) {
    return Result<T, E>::ok(std::move(value));
}

template <typename T, typename E>
Result<T, E> err(E error) {
    return Result<T, E>::err(std::move(error));
}

} // namespace aegis
