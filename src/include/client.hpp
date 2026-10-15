#pragma once
#include "json.hpp"
#include <curl/curl.h>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
namespace decide {
using Json = nlohmann::ordered_json;
constexpr const char *OPENAI_ENDPOINT = "https://api.openai.com/v1/decisions";
struct Config {
    std::string key;
    std::string endpoint = OPENAI_ENDPOINT;
    std::string provider = "openai";   // "openai" or "cloudflare"
    std::string model = "clef";        // Cloudflare model selector: "clef" or "clef-flash"
    std::string account_id;            // Cloudflare account id
    bool Cloudflare() const { return provider == "cloudflare"; }
    const char *Name() const { return Cloudflare() ? "Cloudflare Clef" : "OpenAI Decisions"; }
    // The URL requests are sent to: the Workers AI REST endpoint unless a loopback mock overrides it.
    std::string Url() const {
        if (Cloudflare() && endpoint == OPENAI_ENDPOINT)
            return "https://api.cloudflare.com/client/v4/accounts/" + account_id + "/ai/run/@cf/cloudflare/" + model;
        return endpoint;
    }
};
constexpr size_t MAX_BYTES = 16 * 1024 * 1024;
constexpr size_t CLOUDFLARE_MAX_QUESTIONS = 64;
inline void Validate(const Config &c) {
    if (c.provider != "openai" && c.provider != "cloudflare") throw std::runtime_error("decisions_provider must be 'openai' or 'cloudflare'");
    if (c.key.empty()) throw std::runtime_error(c.Cloudflare() ? "Create a decisions secret or set CLOUDFLARE_API_TOKEN" : "Create a decisions secret or set OPENAI_API_KEY");
    if (c.key.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Invalid API key");
    if (c.Cloudflare()) {
        if (c.model != "clef" && c.model != "clef-flash") throw std::runtime_error("decisions_model must be 'clef' or 'clef-flash'");
        if (c.account_id.empty()) throw std::runtime_error("Set account_id in the decisions secret or CLOUDFLARE_ACCOUNT_ID");
        if (c.account_id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) throw std::runtime_error("Invalid Cloudflare account id");
    }
    if (c.endpoint != OPENAI_ENDPOINT && c.endpoint.rfind("http://127.0.0.1:", 0) != 0)
        throw std::runtime_error("Endpoint must be the provider's default or a loopback mock server");
    auto start = c.endpoint.find("://") + 3;
    auto end = c.endpoint.find('/', start);
    if (c.endpoint.substr(start, end - start).find('@') != std::string::npos) throw std::runtime_error("Invalid endpoint authority");
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
// Cloudflare Clef takes a name-keyed question map and returns name-keyed answers. Translate to and
// from the OpenAI array form so batching and validation in the rest of the extension are shared.
inline Json ToClef(const Json &questions) {
    if (questions.size() > CLOUDFLARE_MAX_QUESTIONS) throw std::runtime_error("Cloudflare Clef accepts at most 64 questions per request");
    Json map = Json::object();
    for (const auto &q : questions) {
        if (!q.contains("name") || !q["name"].is_string() || q["name"].get<std::string>().empty()) throw std::runtime_error("Cloudflare Clef questions need unique names");
        auto name = q["name"].get<std::string>();
        if (name.size() > 100 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos)
            throw std::runtime_error("Cloudflare Clef question names may use only letters, digits, '_', '.', '-' (max 100)");
        if (map.contains(name)) throw std::runtime_error("Cloudflare Clef question names must be unique");
        auto describe = [](const Json &item, const char *fallback) {
            if (item.contains("description") && !item["description"].is_null()) return item["description"];
            return item.contains(fallback) ? item[fallback] : Json(nullptr);
        };
        Json out = {{"type", q["type"] == "predicate" ? "noul" : q["type"]}, {"instructions", q["instructions"]}};
        if (q["type"] == "choice") {
            out["criteria"] = Json::object();
            for (const auto &option : q["choices"]) out["criteria"][option["value"].get<std::string>()] = describe(option, "value");
        } else if (q["type"] == "score") {
            out["criteria"] = Json::array();
            for (const auto &level : q["levels"]) out["criteria"].push_back(describe(level, "label"));
        }
        map[name] = std::move(out);
    }
    return map;
}
inline Json FromClef(const Json &response, const Json &questions) {
    const Json &body = response.is_object() && response.contains("result") && response["result"].is_object() ? response["result"] : response;
    if (!body.is_object() || !body.contains("answers") || !body["answers"].is_object()) throw std::runtime_error("Invalid Cloudflare Clef response");
    Json answers = Json::array();
    for (const auto &q : questions) {
        auto name = q["name"].get<std::string>();
        if (!body["answers"].contains(name)) throw std::runtime_error("Decision response answer count mismatch");
        auto a = body["answers"][name];
        if (!a.is_object() || !a.contains("type")) throw std::runtime_error("Decision answer identity mismatch");
        a["name"] = name;
        if (a["type"] == "noul") { a["type"] = "predicate"; if (a.contains("noul")) a["probability"] = a["noul"]; }
        answers.push_back(std::move(a));
    }
    if (body["answers"].size() != questions.size()) throw std::runtime_error("Decision response answer count mismatch");
    return {{"answers", answers}};
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
    static const auto initialized = [] { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("HTTP initialization failed"); return true; }();
    (void)initialized;
    auto payload = (c.Cloudflare() ? Json({{"model", c.model}, {"state", input}, {"questions", ToClef(questions)}})
                                   : Json({{"model", "gpt-6-luna"}, {"input", input}, {"questions", questions}})).dump();
    if (payload.size() > MAX_BYTES) throw std::runtime_error("Request exceeds extension's 16 MiB limit");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("HTTP initialization failed");
    auto *raw = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!raw) throw std::runtime_error("HTTP header allocation failed");
    auto *next = curl_slist_append(raw, ("Authorization: Bearer " + c.key).c_str());
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
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string buffer;
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer);
        if (curl_easy_perform(curl.get()) != CURLE_OK) throw std::runtime_error(std::string(c.Name()) + " transport failed; request not retried");
        long status; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        if (status == 429 && attempt < 2) { std::this_thread::sleep_for(std::chrono::milliseconds(500 * (1 << attempt))); continue; }
        if (status < 200 || status >= 300) throw std::runtime_error(std::string(c.Name()) + " HTTP " + std::to_string(status));
        auto response = Json::parse(buffer, nullptr, false);
        if (response.is_discarded()) throw std::runtime_error(std::string("Invalid ") + c.Name() + " JSON");
        return Answers(c.Cloudflare() ? FromClef(response, questions) : response, questions);
    }
    throw std::runtime_error(std::string(c.Name()) + " retries exhausted");
}
}
