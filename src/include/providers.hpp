#pragma once
// Decision providers are data: each providers/*.json file describes one host (URL, model,
// credentials, limits) and names the wire format it speaks. Wire formats are code, created by
// a factory keyed by name. Questions and answers stay in one canonical form (the OpenAI array
// shape) everywhere else in the extension; a wire format translates at the HTTP boundary.
#include "json.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace decide {
using Json = nlohmann::ordered_json;

// A {placeholder} in the provider URL other than {model}, such as a Cloudflare account id.
struct UrlParam {
    std::string name;     // placeholder, and the decisions secret key that supplies it
    std::string env;      // environment fallback; empty for none
    std::string pattern;  // full-match regex every value must satisfy
};

struct ProviderSpec {
    std::string name, display_name, format, url, default_model, key_env, docs;
    std::vector<std::string> models;  // allowed model ids; empty allows any well-formed id
    std::vector<UrlParam> params;
    size_t max_questions = 0;         // per request; 0 means no provider limit
    std::string name_pattern;         // question-name regex; empty allows any non-empty name
    std::string name_rule;            // describes name_pattern in error messages
    std::string response_wrapper;     // object key around the response body, if any
    std::string auth_header = "Authorization";
    std::string auth_prefix = "Bearer ";
};

class WireFormat {
public:
    virtual ~WireFormat() = default;
    // Request body for canonical questions.
    virtual Json Encode(const std::string &model, const std::string &input, const Json &questions) const = 0;
    // Canonical {"answers": [...]} from a response body, in question order.
    virtual Json Decode(const Json &response, const Json &questions) const = 0;
};

// OpenAI Decisions: the canonical shape itself.
class OpenAIFormat : public WireFormat {
public:
    explicit OpenAIFormat(const ProviderSpec &) {}
    Json Encode(const std::string &model, const std::string &input, const Json &questions) const override {
        return {{"model", model}, {"input", input}, {"questions", questions}};
    }
    Json Decode(const Json &response, const Json &) const override { return response; }
};

// System One (TypeSafe Jev, Cloudflare Clef): questions and answers are maps keyed by name,
// predicates are "noul", and the input is called "state".
class SystemOneFormat : public WireFormat {
public:
    explicit SystemOneFormat(const ProviderSpec &spec) : spec(spec) {
        if (!spec.name_pattern.empty()) names = std::regex(spec.name_pattern);
    }
    Json Encode(const std::string &model, const std::string &input, const Json &questions) const override {
        if (spec.max_questions && questions.size() > spec.max_questions)
            throw std::runtime_error(spec.display_name + " accepts at most " + std::to_string(spec.max_questions) + " questions per request");
        Json map = Json::object();
        for (const auto &q : questions) {
            if (!q.contains("name") || !q["name"].is_string() || q["name"].get<std::string>().empty())
                throw std::runtime_error(spec.display_name + " questions need unique names");
            auto name = q["name"].get<std::string>();
            if (!spec.name_pattern.empty() && !std::regex_match(name, names))
                throw std::runtime_error(spec.display_name + " question names may use only " + spec.name_rule);
            if (name == "__proto__") throw std::runtime_error(spec.display_name + " question name '__proto__' is reserved");
            if (map.contains(name)) throw std::runtime_error(spec.display_name + " question names must be unique");
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
        return {{"model", model}, {"state", input}, {"questions", map}};
    }
    Json Decode(const Json &response, const Json &questions) const override {
        const Json *body = &response;
        // Accept the bare body too, e.g. Workers AI bindings return it without the REST envelope.
        if (!spec.response_wrapper.empty() && response.is_object() && response.contains(spec.response_wrapper) &&
            response[spec.response_wrapper].is_object())
            body = &response[spec.response_wrapper];
        if (!body->is_object() || !body->contains("answers") || !(*body)["answers"].is_object())
            throw std::runtime_error("Invalid " + spec.display_name + " response");
        const auto &map = (*body)["answers"];
        if (map.size() != questions.size()) throw std::runtime_error("Decision response answer count mismatch");
        Json answers = Json::array();
        for (const auto &q : questions) {
            auto name = q["name"].get<std::string>();
            if (!map.contains(name)) throw std::runtime_error("Decision response answer count mismatch");
            auto a = map[name];
            if (!a.is_object() || !a.contains("type")) throw std::runtime_error("Decision answer identity mismatch");
            a["name"] = name;
            if (a["type"] == "noul") {
                a["type"] = "predicate";
                if (a.contains("noul")) a["probability"] = a["noul"];
            }
            answers.push_back(std::move(a));
        }
        return {{"answers", answers}};
    }

private:
    ProviderSpec spec;
    std::regex names;
};

using WireFormatFactory = std::function<std::unique_ptr<WireFormat>(const ProviderSpec &)>;

// Wire formats by the name provider files use in "format". A new format is one entry here.
inline const std::map<std::string, WireFormatFactory> &WireFormats() {
    static const std::map<std::string, WireFormatFactory> formats = {
        {"openai", [](const ProviderSpec &s) { return std::unique_ptr<WireFormat>(new OpenAIFormat(s)); }},
        {"systemone", [](const ProviderSpec &s) { return std::unique_ptr<WireFormat>(new SystemOneFormat(s)); }},
    };
    return formats;
}

struct Provider {
    ProviderSpec spec;
    std::unique_ptr<WireFormat> wire;
};

// Model ids go into request bodies and sometimes URL paths, so keep them to a safe alphabet.
inline bool SafeModelId(const std::string &model, bool in_url) {
    static const std::regex body_id("[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}");
    static const std::regex url_id("[A-Za-z0-9][A-Za-z0-9._-]{0,127}");
    return std::regex_match(model, in_url ? url_id : body_id) && model.find("..") == std::string::npos;
}

// Parses and checks one provider file. Unknown keys are errors so typos cannot pass silently.
inline Provider ParseProvider(const std::string &text) {
    auto json = Json::parse(text, nullptr, false);
    if (!json.is_object()) throw std::runtime_error("provider file is not a JSON object");
    static const std::set<std::string> known = {"name", "display_name", "format", "url", "default_model", "models", "key_env",
                                                "params", "max_questions", "question_names", "response_wrapper", "auth", "docs"};
    for (const auto &item : json.items())
        if (!known.count(item.key())) throw std::runtime_error("unknown provider field '" + item.key() + "'");
    auto text_field = [&](const Json &obj, const char *key, bool required) {
        if (!obj.contains(key)) {
            if (required) throw std::runtime_error(std::string("provider field '") + key + "' is required");
            return std::string();
        }
        if (!obj[key].is_string() || obj[key].get<std::string>().empty()) throw std::runtime_error(std::string("provider field '") + key + "' must be a nonempty string");
        return obj[key].get<std::string>();
    };
    Provider p;
    auto &s = p.spec;
    s.name = text_field(json, "name", true);
    if (!std::regex_match(s.name, std::regex("[a-z][a-z0-9_-]{0,31}"))) throw std::runtime_error("provider name '" + s.name + "' must be lowercase letters, digits, '_' or '-'");
    s.display_name = text_field(json, "display_name", true);
    s.format = text_field(json, "format", true);
    s.url = text_field(json, "url", true);
    if (s.url.rfind("https://", 0) != 0) throw std::runtime_error(s.name + ": url must use https");
    s.default_model = text_field(json, "default_model", true);
    s.key_env = text_field(json, "key_env", true);
    s.docs = text_field(json, "docs", false);
    s.response_wrapper = text_field(json, "response_wrapper", false);
    if (json.contains("models")) {
        if (!json["models"].is_array() || json["models"].empty()) throw std::runtime_error(s.name + ": models must be a nonempty array");
        for (const auto &m : json["models"]) {
            if (!m.is_string()) throw std::runtime_error(s.name + ": models must be strings");
            s.models.push_back(m.get<std::string>());
        }
    }
    if (json.contains("max_questions")) {
        if (!json["max_questions"].is_number_unsigned() || json["max_questions"].get<size_t>() == 0) throw std::runtime_error(s.name + ": max_questions must be a positive integer");
        s.max_questions = json["max_questions"].get<size_t>();
    }
    if (json.contains("question_names")) {
        const auto &names = json["question_names"];
        if (!names.is_object() || names.size() != 2) throw std::runtime_error(s.name + ": question_names needs exactly 'pattern' and 'rule'");
        s.name_pattern = text_field(names, "pattern", true);
        s.name_rule = text_field(names, "rule", true);
        std::regex check(s.name_pattern); // throws on an invalid pattern
    }
    if (json.contains("auth")) {
        const auto &auth = json["auth"];
        if (!auth.is_object() || !auth.contains("header") || auth.size() > 2 || (auth.size() == 2 && !auth.contains("prefix")))
            throw std::runtime_error(s.name + ": auth needs 'header' and optionally 'prefix'");
        s.auth_header = text_field(auth, "header", true);
        s.auth_prefix = auth.contains("prefix") ? auth["prefix"].get<std::string>() : "";
        if (!std::regex_match(s.auth_header, std::regex("[A-Za-z0-9-]+"))) throw std::runtime_error(s.name + ": invalid auth header name");
    }
    std::set<std::string> placeholders = {"model"};
    if (json.contains("params")) {
        if (!json["params"].is_array()) throw std::runtime_error(s.name + ": params must be an array");
        for (const auto &param : json["params"]) {
            if (!param.is_object()) throw std::runtime_error(s.name + ": each param must be an object");
            for (const auto &item : param.items())
                if (item.key() != "name" && item.key() != "env" && item.key() != "pattern") throw std::runtime_error(s.name + ": unknown param field '" + item.key() + "'");
            UrlParam u{text_field(param, "name", true), text_field(param, "env", false), text_field(param, "pattern", true)};
            if (!std::regex_match(u.name, std::regex("[a-z][a-z0-9_]*")) || u.name == "api_key" || !placeholders.insert(u.name).second)
                throw std::runtime_error(s.name + ": invalid or duplicate param name '" + u.name + "'");
            std::regex check(u.pattern);
            s.params.push_back(u);
        }
    }
    // Every {placeholder} in the URL must be declared, and every declared param must be used.
    std::set<std::string> used;
    static const std::regex placeholder("\\{([^}]*)\\}");
    for (std::sregex_iterator it(s.url.begin(), s.url.end(), placeholder), end; it != end; ++it) {
        auto name = (*it)[1].str();
        if (!placeholders.count(name)) throw std::runtime_error(s.name + ": url uses undeclared placeholder {" + name + "}");
        used.insert(name);
    }
    for (const auto &u : s.params)
        if (!used.count(u.name)) throw std::runtime_error(s.name + ": param '" + u.name + "' is not used in url");
    bool model_in_url = used.count("model") > 0;
    if (!SafeModelId(s.default_model, model_in_url)) throw std::runtime_error(s.name + ": invalid default_model");
    for (const auto &m : s.models)
        if (!SafeModelId(m, model_in_url)) throw std::runtime_error(s.name + ": invalid model id '" + m + "'");
    if (!s.models.empty() && std::find(s.models.begin(), s.models.end(), s.default_model) == s.models.end())
        throw std::runtime_error(s.name + ": default_model must be listed in models");
    auto format = WireFormats().find(s.format);
    if (format == WireFormats().end()) throw std::runtime_error(s.name + ": unknown format '" + s.format + "'");
    p.wire = format->second(s);
    return p;
}

// All providers, built once from the provider files compiled into the extension.
class ProviderRegistry {
public:
    explicit ProviderRegistry(const std::vector<std::string> &files) {
        for (const auto &file : files) {
            auto provider = ParseProvider(file);
            auto name = provider.spec.name;
            if (!providers.emplace(name, std::move(provider)).second) throw std::runtime_error("duplicate provider '" + name + "'");
        }
        // Secret scopes match by prefix, so a key scoped to decisions://jev would also reach a
        // provider named jev2. Names must not be prefixes of each other.
        for (const auto &a : providers)
            for (const auto &b : providers)
                if (a.first != b.first && b.first.rfind(a.first, 0) == 0)
                    throw std::runtime_error("provider name '" + a.first + "' is a prefix of '" + b.first + "'");
    }
    const Provider &Find(const std::string &name) const {
        auto it = providers.find(name);
        if (it == providers.end()) throw std::runtime_error("Unknown decisions_provider '" + name + "'; available: " + Names());
        return it->second;
    }
    std::string Names() const {
        std::string out;
        for (const auto &entry : providers) out += (out.empty() ? "" : ", ") + entry.first;
        return out;
    }
    const std::map<std::string, Provider> &All() const { return providers; }

private:
    std::map<std::string, Provider> providers;
};
}
