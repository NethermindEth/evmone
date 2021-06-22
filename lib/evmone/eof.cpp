// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2020 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0

#include "eof.hpp"

#include <array>

namespace evmone
{
namespace
{
constexpr uint8_t FORMAT = 0xef;
constexpr uint8_t MAGIC[] = {0xca, 0xfe};
}  // namespace

size_t EOF1Header::code_begin() const noexcept
{
    if (code_size == 0)
        return 0;  // legacy code

    if (data_size == 0)
        return std::size(MAGIC) + 6;
    else
        return std::size(MAGIC) + 9;
}

size_t EOF1Header::code_end(size_t container_size) const noexcept
{
    if (code_size == 0)
        return container_size;  // legacy code
    else
        return code_begin() + static_cast<size_t>(code_size);
}

bool is_eof_code(const uint8_t* code, size_t code_size) noexcept
{
    static_assert(std::size(MAGIC) == 2);
    return code_size > 8 && code[0] == FORMAT && code[1] == MAGIC[0] && code[2] == MAGIC[1];
}

EOF1Header read_valid_eof1_header(const uint8_t* code) noexcept
{
    EOF1Header header;
    const auto code_size_offset = std::size(MAGIC) + 3;
    header.code_size = (code[code_size_offset] << 8) | code[code_size_offset + 1];
    if (code[code_size_offset + 2] == 2)  // is data section present
    {
        const auto data_size_offset = code_size_offset + 3;
        header.data_size = (code[data_size_offset] << 8) | code[data_size_offset + 1];
    }
    return header;
}
}  // namespace evmone
