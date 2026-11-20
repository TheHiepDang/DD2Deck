#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace dd2::graphics {

// DXBC and DXIL both carry the pixel output signature in the DXBC container.
// Accept a verified color-only signature. Unrecognized or stripped metadata
// leaves the pipeline outside the override.
inline bool color_only_pixel_outputs(D3D12_SHADER_BYTECODE shader) {
    if (!shader.pShaderBytecode || shader.BytecodeLength < 32) return false;
    const auto* data = static_cast<const uint8_t*>(shader.pShaderBytecode);
    const auto read = [](const uint8_t* at) {
        uint32_t value;
        std::memcpy(&value, at, sizeof(value));
        return value;
    };
    if (std::memcmp(data, "DXBC", 4) != 0) return false;
    const size_t length = read(data + 24);
    const size_t parts = read(data + 28);
    if (length < 32 || length > shader.BytecodeLength || parts > (length - 32) / 4) return false;

    for (size_t i = 0; i < parts; ++i) {
        const size_t offset = read(data + 32 + i * 4);
        if (offset > length - 8) return false;
        const auto* part = data + offset;
        const size_t size = read(part + 4);
        if (size > length - offset - 8) return false;
        size_t stride{}, stream_prefix{};
        if (std::memcmp(part, "OSGN", 4) == 0) stride = 24;
        else if (std::memcmp(part, "OSG5", 4) == 0) { stride = 28; stream_prefix = 4; }
        else if (std::memcmp(part, "OSG1", 4) == 0) { stride = 32; stream_prefix = 4; }
        else continue;
        if (size < 8) return false;
        const auto* sig = part + 8;
        const size_t outputs = read(sig);
        const size_t first = read(sig + 4);
        if (first < 8 || first > size || !outputs || outputs > (size - first) / stride) return false;
        for (size_t output = 0; output < outputs; ++output) {
            const auto* entry = sig + first + output * stride + stream_prefix;
            const size_t name = read(entry);
            const uint32_t system_value = read(entry + 8);
            if (name >= size || (system_value != 0 && system_value != 64)) return false;
            constexpr std::string_view target = "sv_target";
            if (size - name <= target.size() || sig[name + target.size()] != 0) return false;
            for (size_t j = 0; j < target.size(); ++j) {
                char c = static_cast<char>(sig[name + j]);
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
                if (c != target[j]) return false;
            }
        }
        return true;
    }
    return false;
}

} // namespace dd2::graphics
