#include "protocol.h"
#include "common.h"
#include "utils.h"

#include <cstring>
#include <stdexcept>

namespace cloud {

std::string Protocol::encode(FrameType type, const std::string& payload, uint16_t flags) {
    if (payload.size() > kMaxFrameSize) {
        throw CloudError(ErrorCode::PayloadTooLarge, "frame too large");
    }
    std::string out;
    out.resize(kHeaderSize + payload.size());
    std::memcpy(&out[0], kFrameMagic, 4);
    out[4] = 1;  // version
    out[5] = static_cast<uint8_t>(type);
    writeU16Be(reinterpret_cast<uint8_t*>(&out[6]), flags);
    writeU32Be(reinterpret_cast<uint8_t*>(&out[8]), static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(&out[kHeaderSize], payload.data(), payload.size());
    }
    return out;
}

bool Protocol::parseHeader(const uint8_t* hdr, uint8_t& version, FrameType& type,
                           uint16_t& flags, uint32_t& length) {
    if (std::memcmp(hdr, kFrameMagic, 4) != 0) {
        return false;
    }
    version = hdr[4];
    type = static_cast<FrameType>(hdr[5]);
    flags = readU16Be(hdr + 6);
    length = readU32Be(hdr + 8);
    return version == 1 && length <= kMaxFrameSize;
}

std::string Protocol::encodeChunk(const std::string& fileId, uint64_t offset,
                                  const void* data, uint32_t len) {
    if (fileId.size() != 32) {
        throw CloudError(ErrorCode::BadRequest, "file_id must be 32 hex chars");
    }
    std::string payload;
    payload.resize(32 + 8 + len);
    std::memcpy(&payload[0], fileId.data(), 32);
    writeU64Be(reinterpret_cast<uint8_t*>(&payload[32]), offset);
    if (len && data) {
        std::memcpy(&payload[40], data, len);
    }
    return encode(FrameType::Chunk, payload);
}

bool Protocol::decodeChunk(const std::string& payload, ChunkPayload& out) {
    if (payload.size() < 40) {
        return false;
    }
    out.fileId.assign(payload.data(), 32);
    out.offset = readU64Be(reinterpret_cast<const uint8_t*>(payload.data() + 32));
    out.data.assign(payload.data() + 40, payload.size() - 40);
    return true;
}

}  // namespace cloud
