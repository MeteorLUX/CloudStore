#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cloud {

enum class FrameType : uint8_t {
    Json = 0,
    Chunk = 1
};

struct Frame {
    FrameType type = FrameType::Json;
    uint16_t flags = 0;
    std::string payload;
};

// 二进制分块载荷：fileId(32 hex) + offset(8) + data
struct ChunkPayload {
    std::string fileId;
    uint64_t offset = 0;
    std::string data;
};

class Protocol {
public:
    static constexpr size_t kHeaderSize = 12;  // magic(4)+ver(1)+type(1)+flags(2)+len(4)

    static std::string encode(FrameType type, const std::string& payload, uint16_t flags = 0);
    static bool parseHeader(const uint8_t* hdr, uint8_t& version, FrameType& type,
                            uint16_t& flags, uint32_t& length);
    static std::string encodeChunk(const std::string& fileId, uint64_t offset,
                                   const void* data, uint32_t len);
    static bool decodeChunk(const std::string& payload, ChunkPayload& out);
};

}  // namespace cloud
