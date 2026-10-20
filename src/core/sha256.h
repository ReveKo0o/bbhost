#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Hex SHA-256 of a buffer.
std::string sha256_hex(const std::uint8_t* data, std::size_t len);
