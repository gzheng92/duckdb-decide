#pragma once
#include "json.hpp"
#include "providers.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <curl/curl.h>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
namespace decide {
using Json = nlohmann::ordered_json;
constexpr size_t MAX_BYTES = 16 * 1024 * 1024;
// A provider resolved for one query: the provider file plus model, credentials and URL values.
struct Config {
    const Provider *provider = nullptr;
    std::string key;
    std::string model;                         // empty means the provider's default model
    std::string endpoint;                      // empty, or a loopback URL for mock tests
    std::map<std::string, std::string> params; // URL placeholder values, e.g. account_id
    const ProviderSpec &Spec() const { return provider->spec; }
    const std::string &Name() const { return Spec().display_name; }
    std::string Model() const { return model.empty() ? Spec().default_model : model; }
    // Largest request body to send: the provider's limit, or the extension's 16 MiB.
    size_t MaxBytes() const { return Spec().max_request_mib ? Spec().max_request_mib * 1024 * 1024 : MAX_BYTES; }
    std::string LimitText() const {
        return Spec().max_request_mib ? Name() + "'s " + std::to_string(Spec().max_request_mib) + " MiB request limit" : "extension's 16 MiB limit";
    }
    std::string Url() const {
        if (!endpoint.empty()) return endpoint;
        std::string url = Spec().url;
        auto fill = [&](const std::string &name, const std::string &value) {
            for (size_t at; (at = url.find("{" + name + "}")) != std::string::npos;) url.replace(at, name.size() + 2, value);
        };
        fill("model", Model());
        for (const auto &param : Spec().params) fill(param.name, params.at(param.name));
        return url;
    }
};
inline void Validate(const Config &c) {
    const auto &s = c.Spec();
    // The endpoint is checked first: it decides where the key would be sent.
    if (!c.endpoint.empty()) {
        if (c.endpoint.rfind("http://127.0.0.1:", 0) != 0) throw std::runtime_error("Endpoint must be the provider's default or a loopback mock server");
        auto start = c.endpoint.find("://") + 3;
        auto end = c.endpoint.find('/', start);
        if (c.endpoint.substr(start, end - start).find('@') != std::string::npos) throw std::runtime_error("Invalid endpoint authority");
    }
    if (c.key.empty()) throw std::runtime_error("Create a decisions secret or set " + s.key_env);
    if (c.key.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Invalid API key");
    auto model = c.Model();
    if (!s.models.empty() && std::find(s.models.begin(), s.models.end(), model) == s.models.end()) {
        std::string list;
        for (const auto &m : s.models) list += (list.empty() ? "'" : ", '") + m + "'";
        throw std::runtime_error("decisions_model for " + s.display_name + " must be one of " + list);
    }
    if (!SafeModelId(model, s.url.find("{model}") != std::string::npos)) throw std::runtime_error("Invalid decisions_model '" + model + "'");
    for (const auto &param : s.params) {
        auto it = c.params.find(param.name);
        if (it == c.params.end() || it->second.empty())
            throw std::runtime_error("Set " + param.name + " in the decisions secret" + (param.env.empty() ? "" : " or " + param.env));
        if (!std::regex_match(it->second, std::regex(param.pattern))) throw std::runtime_error("Invalid " + param.name + " for " + s.display_name);
    }
}
inline Json Question(const std::string &type, const std::string &instructions, const Json &labels = nullptr) {
    if (instructions.empty()) throw std::runtime_error("Question must not be empty");
    Json q = {{"type", type}, {"name", "decision"}, {"instructions", instructions}};
    if (type == "choice" || type == "score") {
        if (!labels.is_array() || labels.size() < (type == "score" ? 2u : 1u)) throw std::runtime_error("Insufficient choices or score levels");
        Json options = Json::array();
        for (const auto &label : labels) {
            if (!label.is_string() || label.get<std::string>().empty()) throw std::runtime_error("Labels must be nonempty strings");
            for (const auto &option : options) if (option[type == "choice" ? "value" : "label"] == label) throw std::runtime_error("Labels must be unique");
            options.push_back({{type == "choice" ? "value" : "label", label}, {"description", label}});
        }
        q[type == "choice" ? "choices" : "levels"] = options;
    }
    return q;
}
inline double Number(const Json &a, const char *field, double low, double high) {
    if (!a.contains(field) || !a[field].is_number()) throw std::runtime_error("Invalid numeric decision answer");
    double n = a[field].get<double>();
    if (!std::isfinite(n) || n < low || n > high) throw std::runtime_error("Decision answer out of range");
    return n;
}
inline Json Answers(const Json &response, const Json &questions) {
    if (!response.is_object() || !response.contains("answers") || !response["answers"].is_array() || response["answers"].size() != questions.size()) throw std::runtime_error("Decision response answer count mismatch");
    const auto &answers = response["answers"];
    for (size_t i = 0; i < questions.size(); ++i) {
        const auto &a = answers[i]; const auto &q = questions[i];
        if (!a.is_object() || !a.contains("type") || !a.contains("name") || a["name"] != q.value("name", Json(nullptr))) throw std::runtime_error("Decision answer identity mismatch");
        if (a["type"] == "refusal") continue;
        if (a["type"] != q["type"]) throw std::runtime_error("Decision answer type mismatch");
        if (a["type"] == "predicate") Number(a, "probability", 0, 1);
        if (a["type"] == "score") Number(a, "score", 0, q.at("levels").size() - 1);
        if (a["type"] == "choice") {
            bool found = false;
            for (const auto &option : q.at("choices")) if (a.contains("choice") && option["value"] == a["choice"]) found = true;
            if (!found) throw std::runtime_error("Decision answer contains unknown choice");
        }
    }
    return answers;
}
inline size_t Receive(char *data, size_t size, size_t count, void *target) {
    auto &buffer = *static_cast<std::string *>(target);
    size_t bytes = size * count;
    if (bytes > MAX_BYTES - buffer.size()) return 0;
    buffer.append(data, bytes); return bytes;
}
constexpr long MAX_RETRY_WAIT_MS = 20000; // longest server-requested wait honoured before giving up
// Response headers used for retries and error messages.
struct ResponseHeaders {
    long retry_after_ms = -1; // from retry-after-ms or retry-after (seconds); -1 if absent
    std::string request_id;
};
inline size_t ReceiveHeader(char *data, size_t size, size_t count, void *target) {
    auto &headers = *static_cast<ResponseHeaders *>(target);
    std::string line(data, size * count);
    auto colon = line.find(':');
    if (colon == std::string::npos) return size * count;
    std::string name = line.substr(0, colon), value = line.substr(colon + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return std::tolower(ch); });
    auto trim = [](std::string &v) {
        v.erase(0, v.find_first_not_of(" \t"));
        auto end = v.find_last_not_of(" \t\r\n");
        v.erase(end == std::string::npos ? 0 : end + 1);
    };
    trim(value);
    // Only plain numbers are honoured; an HTTP-date Retry-After falls back to the default backoff.
    auto number = [](const std::string &v) -> long {
        if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos) return -1;
        return std::stol(v);
    };
    if (name == "retry-after-ms") { auto ms = number(value); if (ms >= 0) headers.retry_after_ms = ms; }
    if (name == "retry-after" && headers.retry_after_ms < 0) { auto sec = number(value); if (sec >= 0) headers.retry_after_ms = sec * 1000; }
    if ((name == "x-typesafe-request-id" || name == "x-request-id" || name == "cf-ray") && headers.request_id.empty()) {
        static const std::regex safe("[A-Za-z0-9._:-]{1,128}");
        if (std::regex_match(value, safe)) headers.request_id = value;
    }
    return size * count;
}
// A short, single-line explanation from an error body: TypeSafe's "detail", OpenAI's
// error.message, or Cloudflare's errors[0].message. Empty when the body has none.
inline std::string ErrorDetail(const std::string &body, const std::string &key) {
    auto json = Json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return "";
    Json detail;
    if (json.contains("detail")) detail = json["detail"];
    else if (json.contains("error") && json["error"].is_object() && json["error"].contains("message")) detail = json["error"]["message"];
    else if (json.contains("errors") && json["errors"].is_array() && !json["errors"].empty() && json["errors"][0].is_object() && json["errors"][0].contains("message")) detail = json["errors"][0]["message"];
    else return "";
    auto text = detail.is_string() ? detail.get<std::string>() : detail.dump();
    if (!key.empty()) for (size_t at; (at = text.find(key)) != std::string::npos;) text.replace(at, key.size(), "[redacted]");
    for (auto &ch : text) if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    if (text.size() > 300) text = text.substr(0, 300) + "...";
    return text;
}
inline Json Evaluate(const Config &c, const std::string &input, const Json &questions) {
    Validate(c);
    if (!questions.is_array() || questions.empty()) throw std::runtime_error("Questions must be a nonempty array");
    for (const auto &q : questions) {
        if (!q.is_object() || !q.contains("type") || !q.contains("instructions")) throw std::runtime_error("Invalid decision question");
        if (q["type"] != "predicate" && q["type"] != "choice" && q["type"] != "score") throw std::runtime_error("Unknown question type");
        if (q["type"] == "choice" && (!q.contains("choices") || !q["choices"].is_array() || q["choices"].empty())) throw std::runtime_error("Missing choices");
        if (q["type"] == "score" && (!q.contains("levels") || !q["levels"].is_array() || q["levels"].size() < 2)) throw std::runtime_error("Missing score levels");
    }
    CheckLimits(c.Spec(), questions);
    static const auto initialized = [] { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("HTTP initialization failed"); return true; }();
    (void)initialized;
    auto payload = c.provider->wire->Encode(c.Model(), input, questions).dump();
    if (payload.size() > c.MaxBytes()) throw std::runtime_error("Request exceeds " + c.LimitText());
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("HTTP initialization failed");
    auto *raw = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!raw) throw std::runtime_error("HTTP header allocation failed");
    auto *next = curl_slist_append(raw, (c.Spec().auth_header + ": " + c.Spec().auth_prefix + c.key).c_str());
    if (!next) { curl_slist_free_all(raw); throw std::runtime_error("HTTP header allocation failed"); }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(next, curl_slist_free_all);
    auto url = c.Url();
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, ReceiveHeader);
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string buffer;
        ResponseHeaders received;
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &received);
        if (curl_easy_perform(curl.get()) != CURLE_OK) throw std::runtime_error(c.Name() + " transport failed; request not retried");
        long status; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        // 429 (rate limited) and 529 (overloaded) mean the request was not processed, so a retry
        // cannot double-bill. Wait at least as long as the server asks, up to a cap.
        bool busy = status == 429 || status == 529;
        if (busy && received.retry_after_ms > MAX_RETRY_WAIT_MS)
            throw std::runtime_error(c.Name() + " HTTP " + std::to_string(status) + "; server asked to retry after " +
                                     std::to_string((received.retry_after_ms + 999) / 1000) + " s, so the request was not retried");
        if (busy && attempt < 2) {
            long wait = std::max<long>(500L * (1 << attempt), received.retry_after_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));
            continue;
        }
        if (status < 200 || status >= 300) {
            auto message = c.Name() + " HTTP " + std::to_string(status);
            auto detail = ErrorDetail(buffer, c.key);
            if (!detail.empty()) message += ": " + detail;
            if (!received.request_id.empty()) message += " (request id " + received.request_id + ")";
            throw std::runtime_error(message);
        }
        auto response = Json::parse(buffer, nullptr, false);
        if (response.is_discarded()) throw std::runtime_error("Invalid " + c.Name() + " JSON");
        return Answers(c.provider->wire->Decode(response, questions), questions);
    }
    throw std::runtime_error(c.Name() + " retries exhausted");
}
}
