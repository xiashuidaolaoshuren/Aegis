#pragma once

#include <aegis/iso8583/fields.hpp>

#include <map>
#include <string>
#include <variant>

namespace aegis::iso8583 {

class Message {
public:
    explicit Message(Mti mti) : mti_(mti) {}

    [[nodiscard]] Mti mti() const {
        return mti_;
    }

    [[nodiscard]] bool has(FieldId id) const {
        return fields_.find(id) != fields_.end();
    }

    [[nodiscard]] Result<std::string, MessageError> get(FieldId id) const {
        const auto it = fields_.find(id);
        if (it == fields_.end()) {
            return Result<std::string, MessageError>::err(MessageError::FieldAbsent);
        }
        return Result<std::string, MessageError>::ok(it->second);
    }

    [[nodiscard]] Result<std::monostate, MessageError> set(FieldId id, std::string value) {
        if (find_field(id) == nullptr) {
            return Result<std::monostate, MessageError>::err(MessageError::UnknownField);
        }
        fields_[id] = std::move(value);
        return Result<std::monostate, MessageError>::ok({});
    }

private:
    Mti mti_;
    std::map<FieldId, std::string> fields_;
};

} // namespace aegis::iso8583
