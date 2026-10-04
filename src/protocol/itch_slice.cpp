#include "lle/protocol/itch_slice.hpp"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

#include "lle/protocol/endian.hpp"
#include "lle/protocol/itch_encode.hpp"

namespace lle::itch {

SliceStats write_locate_slice(ItchFileReader& in, Locate locate, std::uint64_t last_index,
                              const std::string& out_path) {
    auto closer = [](std::FILE* f) { std::fclose(f); };
    std::unique_ptr<std::FILE, decltype(closer)> out(std::fopen(out_path.c_str(), "wb"), closer);
    if (!out) throw std::runtime_error("cannot open " + out_path);
    SliceStats st;
    std::vector<std::byte> frame_buf(2 + 65535);
    const std::byte* p = nullptr;
    std::size_t len = 0;
    while (st.read <= last_index && in.next(p, len)) {
        ++st.read;
        // Every ITCH message has the 2-byte stock locate at offset 1 (0 for system messages).
        const bool keep =
            len >= 3 && (static_cast<char>(p[0]) == 'S' || proto::load_be<std::uint16_t>(p + 1) == locate);
        if (!keep) continue;
        const std::size_t n = frame(frame_buf.data(), p, len);
        if (std::fwrite(frame_buf.data(), 1, n, out.get()) != n) throw std::runtime_error("write failed: " + out_path);
        ++st.written;
    }
    return st;
}

}  // namespace lle::itch
