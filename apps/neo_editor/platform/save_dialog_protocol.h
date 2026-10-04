#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Private, bounded pipe protocol. No paths or user text are passed on the command line.
namespace neo::dialogs::wire {
constexpr std::size_t kMaximumPacket = 60 * 1024;
constexpr std::uint32_t kMagic = 0x3153444e; // NDS1
struct Request {
    std::uint64_t owner = 0;
    std::vector<std::u16string> fields; // initial directory, name, filter, default extension
};
inline void number(std::string& out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out.push_back(static_cast<char>(value >> (i * 8)));
}
inline bool number(const std::string& in, std::size_t& at, std::uint64_t& value, unsigned bytes) {
    if (at > in.size() || in.size() - at < bytes) return false;
    value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= std::uint64_t(static_cast<unsigned char>(in[at++])) << (i * 8);
    return true;
}
inline bool text(std::string& out, const std::u16string& value) {
    if (value.size() > kMaximumPacket / 2 || out.size() + 4 + value.size() * 2 > kMaximumPacket) return false;
    number(out, value.size(), 4);
    for (char16_t ch : value) number(out, ch, 2);
    return true;
}
inline bool text(const std::string& in, std::size_t& at, std::u16string& value) {
    std::uint64_t count = 0;
    if (!number(in, at, count, 4) || count > (in.size() - at) / 2) return false;
    value.clear(); value.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0, ch; i < count; ++i) { number(in, at, ch, 2); value.push_back(static_cast<char16_t>(ch)); }
    return true;
}
inline bool encode(const Request& request, std::string& out) {
    out.clear();
    if (request.fields.size() != 4) return false;
    number(out, kMagic, 4); number(out, request.owner, 8);
    for (const auto& field : request.fields) if (!text(out, field)) { out.clear(); return false; }
    return true;
}
inline bool decode(const std::string& in, Request& request) {
    if (in.size() > kMaximumPacket) return false;
    std::size_t at = 0; std::uint64_t magic = 0;
    Request decoded;
    if (!number(in, at, magic, 4) || magic != kMagic || !number(in, at, decoded.owner, 8)) return false;
    decoded.fields.resize(4);
    for (auto& field : decoded.fields) if (!text(in, at, field)) return false;
    if (at != in.size()) return false;
    // Only the filter can contain embedded NULs; it must end in a double NUL.
    for (std::size_t i : {0u, 1u, 3u}) if (decoded.fields[i].find(u'\0') != std::u16string::npos) return false;
    const auto& filter = decoded.fields[2];
    if (filter.size() < 2 || filter[filter.size()-1] || filter[filter.size()-2]) return false;
    request = std::move(decoded);
    return true;
}
inline bool encodeResult(unsigned status, std::uint32_t error, const std::u16string& path, std::string& out) {
    out.clear();
    if (status > 2 || (status != 1 && !path.empty()) || (status == 1 && path.empty()) || path.find(u'\0') != std::u16string::npos) return false;
    number(out, kMagic, 4); number(out, status, 4); number(out, error, 4);
    return text(out, path);
}
inline bool decodeResult(const std::string& in, unsigned& status, std::uint32_t& error, std::u16string& path) {
    if (in.size() > kMaximumPacket) return false;
    std::size_t at = 0; std::uint64_t magic = 0, s = 0, e = 0;
    std::u16string decoded;
    if (!number(in,at,magic,4) || magic != kMagic || !number(in,at,s,4) || s > 2 ||
        !number(in,at,e,4) || !text(in,at,decoded) || at != in.size() || decoded.find(u'\0') != std::u16string::npos ||
        (s != 1 && !decoded.empty()) || (s == 1 && decoded.empty())) return false;
    status = static_cast<unsigned>(s); error = static_cast<std::uint32_t>(e); path = std::move(decoded);
    return true;
}
} // namespace neo::dialogs::wire
