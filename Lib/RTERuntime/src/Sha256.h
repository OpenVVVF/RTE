#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rte::runtime {

std::string Sha256Hex(const void* data, std::size_t size);
std::string Sha256File(const std::string& path);

}  // namespace rte::runtime
