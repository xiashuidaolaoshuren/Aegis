#pragma once

#include <functional>
#include <type_traits>

namespace aegis {

template <typename T, typename Tag>
class Tagged {
public:
    explicit Tagged(T value) : value_(std::move(value)) {}

    [[nodiscard]] const T& value() const {
        return value_;
    }

    friend bool operator==(const Tagged& lhs, const Tagged& rhs) {
        return lhs.value_ == rhs.value_;
    }

private:
    T value_;
};

} // namespace aegis

namespace std {

template <typename T, typename Tag>
struct hash<aegis::Tagged<T, Tag>> {
    size_t operator()(const aegis::Tagged<T, Tag>& tagged) const {
        return hash<T>{}(tagged.value());
    }
};

} // namespace std
