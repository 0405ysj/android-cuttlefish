//
// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cuttlefish/host/libs/web/http_client/http_client.h"

#include <stdint.h>
#include <stdio.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <json/value.h>

#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/common/libs/fs/shared_fd_stream.h"
#include "cuttlefish/common/libs/utils/files.h"
#include "cuttlefish/common/libs/utils/json.h"
#include "cuttlefish/common/libs/utils/result.h"
#include "cuttlefish/host/libs/web/http_client/http_client_util.h"

namespace cuttlefish {
namespace {

std::string TrimWhitespace(const char* data, const size_t size) {
  std::string converted(data, size);
  return android::base::Trim(converted);
}

int LoggingCurlDebugFunction(CURL*, curl_infotype type, char* data, size_t size,
                             void*) {
  switch (type) {
    case CURLINFO_TEXT:
      LOG(VERBOSE) << "CURLINFO_TEXT ";
      LOG(DEBUG) << ScrubSecrets(TrimWhitespace(data, size));
      break;
    case CURLINFO_HEADER_IN:
      LOG(VERBOSE) << "CURLINFO_HEADER_IN ";
      LOG(DEBUG) << TrimWhitespace(data, size);
      break;
    case CURLINFO_HEADER_OUT:
      LOG(VERBOSE) << "CURLINFO_HEADER_OUT ";
      LOG(DEBUG) << ScrubSecrets(TrimWhitespace(data, size));
      break;
    case CURLINFO_DATA_IN:
      break;
    case CURLINFO_DATA_OUT:
      break;
    case CURLINFO_SSL_DATA_IN:
      break;
    case CURLINFO_SSL_DATA_OUT:
      break;
    case CURLINFO_END:
      LOG(VERBOSE) << "CURLINFO_END ";
      LOG(DEBUG) << ScrubSecrets(TrimWhitespace(data, size));
      break;
    default:
      LOG(ERROR) << "Unexpected cURL output type: " << type;
      break;
  }
  return 0;
}

enum class HttpMethod {
  kGet,
  kPost,
  kDelete,
};

size_t curl_to_function_cb(char* ptr, size_t, size_t nmemb, void* userdata) {
  HttpClient::DataCallback* callback = (HttpClient::DataCallback*)userdata;
  if (!(*callback)(ptr, nmemb)) {
    return 0;  // Signals error to curl
  }
  return nmemb;
}

using ManagedCurlSlist =
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

Result<ManagedCurlSlist> SlistFromStrings(
    const std::vector<std::string>& strings) {
  ManagedCurlSlist curl_headers(nullptr, curl_slist_free_all);
  for (const auto& str : strings) {
    curl_slist* temp = curl_slist_append(curl_headers.get(), str.c_str());
    CF_EXPECT(temp != nullptr,
              "curl_slist_append failed to add \"" << str << "\"");
    (void)curl_headers.release();  // Memory is now owned by `temp`
    curl_headers.reset(temp);
  }
  return curl_headers;
}

class CurlClient : public HttpClient {
 public:
  CurlClient(const bool use_logging_debug_function)
      : use_logging_debug_function_(use_logging_debug_function) {
    curl_ = curl_easy_init();
    if (!curl_) {
      LOG(ERROR) << "failed to initialize curl";
      return;
    }
  }
  ~CurlClient() { curl_easy_cleanup(curl_); }

  Result<HttpResponse<std::string>> GetToString(
      const std::string& url,
      const std::vector<std::string>& headers) override {
    return DownloadToString(HttpMethod::kGet, url, headers);
  }

  Result<HttpResponse<std::string>> PostToString(
      const std::string& url, const std::string& data_to_write,
      const std::vector<std::string>& headers) override {
    return DownloadToString(HttpMethod::kPost, url, headers, data_to_write);
  }

  Result<HttpResponse<Json::Value>> PostToJson(
      const std::string& url, const std::string& data_to_write,
      const std::vector<std::string>& headers) override {
    return DownloadToJson(HttpMethod::kPost, url, headers, data_to_write);
  }

  Result<HttpResponse<Json::Value>> PostToJson(
      const std::string& url, const Json::Value& data_to_write,
      const std::vector<std::string>& headers) override {
    std::stringstream json_str;
    json_str << data_to_write;
    return DownloadToJson(HttpMethod::kPost, url, headers, json_str.str());
  }

  Result<HttpResponse<void>> DownloadToCallback(
      DataCallback callback, const std::string& url,
      const std::vector<std::string>& headers) override {
    return DownloadToCallback(HttpMethod::kGet, callback, url, headers);
  }

  Result<HttpResponse<std::string>> DownloadToFile(
      const std::string& url, const std::string& path,
      const std::vector<std::string>& headers) override {
    LOG(DEBUG) << "Saving '" << url << "' to '" << path << "'";

    auto [shared_fd, temp_path] = CF_EXPECT(SharedFD::Mkostemp(path));
    SharedFDOstream stream(shared_fd);
    uint64_t total_dl = 0;
    uint64_t last_log = 0;
    auto callback = [&stream, &total_dl, &last_log](char* data,
                                                    size_t size) -> bool {
      total_dl += size;
      if (total_dl / 2 >= last_log) {
        LOG(DEBUG) << "Downloaded " << total_dl << " bytes";
        last_log = total_dl;
      }
      if (data == nullptr) {
        return !stream.fail();
      }
      stream.write(data, size);
      return !stream.fail();
    };
    HttpResponse<void> http_response = CF_EXPECT(DownloadToCallback(callback, url, headers));

    LOG(DEBUG) << "Downloaded '" << total_dl << "' total bytes from '" << url
               << "' to '" << path << "'.";

    if (http_response.HttpSuccess()) {
      CF_EXPECT(RenameFile(temp_path, path));
    } else {
      CF_EXPECTF(RemoveFile(temp_path), "Unable to remove temporary file \"{}\"\nMay require manual removal", temp_path);
    }
    return HttpResponse<std::string>{path, http_response.http_code};
  }

  Result<HttpResponse<Json::Value>> DownloadToJson(
      const std::string& url, const std::vector<std::string>& headers) override {
    return DownloadToJson(HttpMethod::kGet, url, headers);
  }

  std::string UrlEscape(const std::string& text) override {
    char* escaped_str = curl_easy_escape(curl_, text.c_str(), text.size());
    std::string ret{escaped_str};
    curl_free(escaped_str);
    return ret;
  }

 private:
  Result<HttpResponse<Json::Value>> DownloadToJson(
      HttpMethod method, const std::string& url,
      const std::vector<std::string>& headers,
      const std::string& data_to_write = "") {
    auto response =
        CF_EXPECT(DownloadToString(method, url, headers, data_to_write));
    auto result = ParseJson(response.data);
    if (!result.ok()) {
      Json::Value error_json;
      LOG(ERROR) << "Could not parse json: " << result.error().FormatForEnv();
      error_json["error"] = "Failed to parse json: " + result.error().Message();
      error_json["response"] = response.data;
      return HttpResponse<Json::Value>{error_json, response.http_code};
    }
    return HttpResponse<Json::Value>{*result, response.http_code};
  }

  Result<HttpResponse<std::string>> DownloadToString(
      HttpMethod method, const std::string& url,
      const std::vector<std::string>& headers,
      const std::string& data_to_write = "") {
    std::stringstream stream;
    auto callback = [&stream](char* data, size_t size) -> bool {
      if (data == nullptr) {
        stream = std::stringstream();
        return true;
      }
      stream.write(data, size);
      return true;
    };
    auto http_response = CF_EXPECT(
        DownloadToCallback(method, callback, url, headers, data_to_write));
    return HttpResponse<std::string>{stream.str(), http_response.http_code};
  }

  Result<HttpResponse<void>> DownloadToCallback(
      HttpMethod method, DataCallback callback, const std::string& url,
      const std::vector<std::string>& headers,
      const std::string& data_to_write = "") {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG(DEBUG) << "Downloading '" << url << "'";
    CF_EXPECT(data_to_write.empty() || method == HttpMethod::kPost,
              "data must be empty for non POST requests");
    CF_EXPECT(curl_ != nullptr, "curl was not initialized");
    CF_EXPECT(callback(nullptr, 0) /* Signal start of data */,
              "callback failure");
    auto curl_headers = CF_EXPECT(SlistFromStrings(headers));
    curl_easy_reset(curl_);
    if (method == HttpMethod::kDelete) {
      curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    curl_easy_setopt(curl_, CURLOPT_CAINFO,
                     "/etc/ssl/certs/ca-certificates.crt");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, curl_headers.get());
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    if (method == HttpMethod::kPost) {
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, data_to_write.size());
      curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, data_to_write.c_str());
    }
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, curl_to_function_cb);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &callback);
    char error_buf[CURL_ERROR_SIZE];
    curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, error_buf);
    curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1L);
    // CURLOPT_VERBOSE must be set for CURLOPT_DEBUGFUNCTION be utilized
    if (use_logging_debug_function_) {
      curl_easy_setopt(curl_, CURLOPT_DEBUGFUNCTION, LoggingCurlDebugFunction);
    }
    CURLcode res = curl_easy_perform(curl_);
    CF_EXPECT(res == CURLE_OK,
              "curl_easy_perform() failed. "
                  << "Code was \"" << res << "\". "
                  << "Strerror was \"" << curl_easy_strerror(res) << "\". "
                  << "Error buffer was \"" << error_buf << "\".");
    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    return HttpResponse<void>{{}, http_code};
  }

  CURL* curl_;
  std::mutex mutex_;
  bool use_logging_debug_function_;
};

class ServerErrorRetryClient : public HttpClient {
 public:
  ServerErrorRetryClient(HttpClient& inner, int retry_attempts,
                         std::chrono::milliseconds retry_delay)
      : inner_client_(inner),
        retry_attempts_(retry_attempts),
        retry_delay_(retry_delay) {}

  Result<HttpResponse<std::string>> GetToString(
      const std::string& url, const std::vector<std::string>& headers) override {
    auto fn = [&, this]() { return inner_client_.GetToString(url, headers); };
    return CF_EXPECT(RetryImpl<std::string>(fn));
  }

  Result<HttpResponse<std::string>> PostToString(
      const std::string& url, const std::string& data,
      const std::vector<std::string>& headers) override {
    auto fn = [&, this]() {
      return inner_client_.PostToString(url, data, headers);
    };
    return CF_EXPECT(RetryImpl<std::string>(fn));
  }

  Result<HttpResponse<Json::Value>> PostToJson(
      const std::string& url, const Json::Value& data,
      const std::vector<std::string>& headers) override {
    auto fn = [&, this]() {
      return inner_client_.PostToJson(url, data, headers);
    };
    return CF_EXPECT(RetryImpl<Json::Value>(fn));
  }

  Result<HttpResponse<Json::Value>> PostToJson(
      const std::string& url, const std::string& data,
      const std::vector<std::string>& headers) override {
    auto fn = [&, this]() {
      return inner_client_.PostToJson(url, data, headers);
    };
    return CF_EXPECT(RetryImpl<Json::Value>(fn));
  }

  Result<HttpResponse<std::string>> DownloadToFile(
      const std::string& url, const std::string& path,
      const std::vector<std::string>& headers) override {
    auto fn = [&, this]() {
      return inner_client_.DownloadToFile(url, path, headers);
    };
    return CF_EXPECT(RetryImpl<std::string>(fn));
  }

  Result<HttpResponse<Json::Value>> DownloadToJson(
      const std::string& url, const std::vector<std::string>& headers) override {
    auto fn = [&, this]() {
      return inner_client_.DownloadToJson(url, headers);
    };
    return CF_EXPECT(RetryImpl<Json::Value>(fn));
  }

  Result<HttpResponse<void>> DownloadToCallback(
      DataCallback cb, const std::string& url,
      const std::vector<std::string>& hdrs) override {
    auto fn = [&, this]() {
      return inner_client_.DownloadToCallback(cb, url, hdrs);
    };
    return CF_EXPECT(RetryImpl<void>(fn));
  }

  std::string UrlEscape(const std::string& text) override {
    return inner_client_.UrlEscape(text);
  }

 private:
  template <typename T>
  Result<HttpResponse<T>> RetryImpl(
      std::function<Result<HttpResponse<T>>()> attempt_fn) {
    HttpResponse<T> response;
    for (int attempt = 0; attempt != retry_attempts_; ++attempt) {
      if (attempt != 0) {
        std::this_thread::sleep_for(retry_delay_);
      }
      response = CF_EXPECT(attempt_fn());
      if (!response.HttpServerError()) {
        return response;
      }
    }
    return response;
  }

 private:
  HttpClient& inner_client_;
  int retry_attempts_;
  std::chrono::milliseconds retry_delay_;
};

}  // namespace

/* static */ std::unique_ptr<HttpClient> HttpClient::CurlClient(
    bool use_logging_debug_function) {
  return std::make_unique<class CurlClient>(use_logging_debug_function);
}

/* static */ std::unique_ptr<HttpClient> HttpClient::ServerErrorRetryClient(
    HttpClient& inner, int retry_attempts,
    std::chrono::milliseconds retry_delay) {
  return std::make_unique<class ServerErrorRetryClient>(inner, retry_attempts,
                                                        retry_delay);
}

HttpClient::~HttpClient() = default;

}  // namespace cuttlefish
