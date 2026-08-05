#include "Sha256.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace rte::runtime {
namespace {

constexpr std::array<uint32_t, 64> kTable = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t Rotate(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32U - bits));
}

}  // namespace

std::string Sha256Hex(const void* raw, std::size_t size) {
    const auto* input = static_cast<const uint8_t*>(raw);
    std::vector<uint8_t> bytes(input, input + size);
    const uint64_t bitSize = static_cast<uint64_t>(size) * 8U;
    bytes.push_back(0x80);
    while (bytes.size() % 64 != 56) bytes.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<uint8_t>(bitSize >> shift));
    }

    std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                  0xa54ff53a, 0x510e527f, 0x9b05688c,
                                  0x1f83d9ab, 0x5be0cd19};
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
        std::array<uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t p = offset + i * 4;
            w[i] = (static_cast<uint32_t>(bytes[p]) << 24)
                   | (static_cast<uint32_t>(bytes[p + 1]) << 16)
                   | (static_cast<uint32_t>(bytes[p + 2]) << 8)
                   | bytes[p + 3];
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18)
                                ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19)
                                ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, hh] = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + s1 + choice + kTable[i] + w[i];
            const uint32_t s0 = Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + majority;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint32_t word : h) out << std::setw(8) << word;
    return out.str();
}

std::string Sha256File(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    return Sha256Hex(bytes.data(), bytes.size());
}

}  // namespace rte::runtime
