#include <aegis/iso8583/codec.hpp>
#include <aegis/iso8583/fields.hpp>
#include <aegis/iso8583/message.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using aegis::iso8583::Encoding;
using aegis::iso8583::FieldSpec;
using aegis::iso8583::LengthKind;
using aegis::iso8583::Message;
using aegis::iso8583::Mti;
using aegis::iso8583::kFieldTable;
using aegis::iso8583::serialise;

std::string sample_field_value(const FieldSpec& spec) {
    if (spec.encoding == Encoding::Binary) {
        return std::string(spec.max_length, '\xA5');
    }
    if (spec.encoding == Encoding::Ascii) {
        return std::string(spec.max_length, 'A');
    }
    if (spec.length_kind == LengthKind::Llvar) {
        return std::string(spec.min_length, '4');
    }
    return std::string(spec.max_length, '1');
}

void write_seed(const std::filesystem::path& dir, const std::string& name, const Message& message) {
    const auto encoded = serialise(message);
    if (!encoded.has_value()) {
        std::cerr << "serialise failed for " << name << '\n';
        std::exit(1);
    }
    const auto path = dir / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "failed to write " << path << '\n';
        std::exit(1);
    }
    for (const std::byte b : encoded.value()) {
        out.put(static_cast<char>(static_cast<unsigned char>(b)));
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path out_dir = argc > 1 ? argv[1] : std::filesystem::path{"fuzz/corpus"};
    std::filesystem::create_directories(out_dir);

    const std::pair<Mti, const char*> empty_mtis[] = {
        {Mti::AuthorizationRequest, "empty_0100.bin"},
        {Mti::AuthorizationResponse, "empty_0110.bin"},
        {Mti::CaptureRequest, "empty_0200.bin"},
        {Mti::CaptureResponse, "empty_0210.bin"},
        {Mti::ReversalRequest, "empty_0400.bin"},
        {Mti::ReversalResponse, "empty_0410.bin"},
        {Mti::NetworkManagementRequest, "empty_0800.bin"},
        {Mti::NetworkManagementResponse, "empty_0810.bin"},
    };
    for (const auto& [mti, name] : empty_mtis) {
        write_seed(out_dir, name, Message{mti});
    }

    {
        Message message{Mti::AuthorizationRequest};
        (void)message.set(aegis::iso8583::FieldId::ResponseCode, "00");
        write_seed(out_dir, "ascii_39.bin", message);
    }
    {
        Message message{Mti::AuthorizationRequest};
        (void)message.set(aegis::iso8583::FieldId::Stan, "000042");
        write_seed(out_dir, "bcd_fixed_even_11.bin", message);
    }
    {
        Message message{Mti::AuthorizationRequest};
        (void)message.set(aegis::iso8583::FieldId::Currency, "840");
        write_seed(out_dir, "bcd_fixed_odd_49.bin", message);
    }
    {
        Message message{Mti::AuthorizationRequest};
        (void)message.set(aegis::iso8583::FieldId::Pan, "4242424242424242");
        write_seed(out_dir, "llvar_pan_2.bin", message);
    }
    {
        Message message{Mti::AuthorizationRequest};
        (void)message.set(aegis::iso8583::FieldId::PinBlock, std::string("\x01\x23\x45\x67\x89\xAB\xCD\xEF", 8));
        write_seed(out_dir, "binary_52.bin", message);
    }
    {
        Message message{Mti::CaptureRequest};
        (void)message.set(aegis::iso8583::FieldId::OriginalDataElements, std::string(42, '1'));
        write_seed(out_dir, "secondary_90.bin", message);
    }
    {
        Message message{Mti::AuthorizationRequest};
        for (const FieldSpec& spec : kFieldTable) {
            (void)message.set(spec.id, sample_field_value(spec));
        }
        write_seed(out_dir, "kitchen_sink.bin", message);
    }

    std::cout << "wrote corpus to " << std::filesystem::absolute(out_dir) << '\n';
    return 0;
}
