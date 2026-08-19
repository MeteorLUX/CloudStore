#pragma once

#include <string>

#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#else
#include <json/json.h>
#endif

namespace cloud {

bool parseJson(const std::string& s, Json::Value& out, std::string* err = nullptr);
std::string dumpJson(const Json::Value& v);
Json::Value makeReply(uint64_t seq, int code, const std::string& msg,
                      const Json::Value& data = Json::Value(Json::objectValue));

}  // namespace cloud
