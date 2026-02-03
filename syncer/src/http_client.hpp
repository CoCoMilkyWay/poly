#pragma once

#include <string>
#include <curl/curl.h>
#include <cassert>

class HttpClient {
public:
    HttpClient() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_ = curl_easy_init();
        assert(curl_ && "curl 初始化失败");
    }
    
    ~HttpClient() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
        curl_global_cleanup();
    }
    
    // 禁止拷贝
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    
    std::string post(const std::string& url, const std::string& body, 
                     const std::string& auth_token = "") {
        assert(curl_ && "curl 未初始化");
        
        response_.clear();
        
        curl_easy_reset(curl_);
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_);
        
        // 设置请求头
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!auth_token.empty()) {
            std::string auth_header = "Authorization: Bearer " + auth_token;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        
        // 执行请求
        CURLcode res = curl_easy_perform(curl_);
        
        curl_slist_free_all(headers);
        
        assert(res == CURLE_OK && "HTTP 请求失败");
        
        return response_;
    }

private:
    static size_t write_callback(void* contents, size_t size, size_t nmemb, 
                                  std::string* userp) {
        size_t total = size * nmemb;
        userp->append(static_cast<char*>(contents), total);
        return total;
    }
    
    CURL* curl_ = nullptr;
    std::string response_;
};
