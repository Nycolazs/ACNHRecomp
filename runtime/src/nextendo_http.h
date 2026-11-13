// Minimal HTTPS helper for Nextendo API / OAuth: a curl subprocess (as the launcher's update check), and on Android,
// which has no curl, HttpURLConnection (platform_android.cpp).
#pragma once

#include <map>
#include <string>

namespace nx::nextendo {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
};

// GET or POST. headers: name -> value. body empty => GET.
HttpResponse http_request(const std::string& method, const std::string& url,
                          const std::map<std::string, std::string>& headers = {},
                          const std::string& body = "", const std::string& content_type = "");

}  // namespace nx::nextendo
