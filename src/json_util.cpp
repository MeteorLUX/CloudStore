#include "json_util.h"

#include <memory>

namespace cloud {

bool parseJson(const std::string& s, Json::Value& out, std::string* err) {
    Json::CharReaderBuilder b;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    std::string e;
    bool ok = reader->parse(s.data(), s.data() + s.size(), &out, &e);
    if (!ok && err) {
        *err = e;
    }
    return ok;
}

std::string dumpJson(const Json::Value& v) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    b["emitUTF8"] = true;
    return Json::writeString(b, v);
}

Json::Value makeReply(uint64_t seq, int code, const std::string& msg, const Json::Value& data) {
    Json::Value r;
    r["seq"] = static_cast<Json::UInt64>(seq);
    r["code"] = code;
    r["msg"] = msg;
    r["data"] = data;
    return r;
}

}  // namespace cloud
