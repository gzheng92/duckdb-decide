// Checks provider files with the extension's own parser, without building DuckDB.
// Usage: providers_test providers/*.json
#include "providers.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

using decide::Json;

static int failures = 0;

static void Expect(bool ok, const std::string &what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        failures++;
    }
}

// The parser must reject `spec` with an error mentioning `message`.
static void Rejects(const Json &spec, const std::string &message) {
    try {
        decide::ParseProvider(spec.dump());
        Expect(false, "accepted invalid spec, expected: " + message);
    } catch (const std::exception &e) {
        Expect(std::string(e.what()).find(message) != std::string::npos, "error '" + std::string(e.what()) + "' lacks '" + message + "'");
    }
}

int main(int argc, char **argv) {
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i]);
        std::stringstream text;
        text << in.rdbuf();
        if (!in) {
            std::cerr << "cannot read " << argv[i] << "\n";
            return 1;
        }
        try {
            auto provider = decide::ParseProvider(text.str());
            std::string expected = std::string(argv[i]);
            expected = expected.substr(expected.find_last_of('/') + 1);
            Expect(expected == provider.spec.name + ".json", std::string(argv[i]) + ": file name must be <name>.json");
            // A request must encode, and a canonical answer must decode, for every shipped provider.
            Json q = Json::array({{{"type", "predicate"}, {"name", "row_1"}, {"instructions", "Q"}}});
            auto body = provider.wire->Encode(provider.spec.default_model, "text", q);
            Expect(body.at("model") == provider.spec.default_model, provider.spec.name + ": model not encoded");
            files.push_back(text.str());
        } catch (const std::exception &e) {
            std::cerr << "FAIL: " << argv[i] << ": " << e.what() << "\n";
            failures++;
        }
    }
    if (files.empty()) {
        std::cerr << "no provider files given\n";
        return 1;
    }
    try {
        decide::ProviderRegistry registry(files);
    } catch (const std::exception &e) {
        std::cerr << "FAIL: registry: " << e.what() << "\n";
        failures++;
    }

    Json base = {{"name", "demo"}, {"display_name", "Demo"}, {"format", "systemone"},
                 {"url", "https://example.com/v1/{model}"}, {"default_model", "m1"}, {"key_env", "DEMO_KEY"}};
    decide::ParseProvider(base.dump()); // the baseline itself is valid
    auto with = [&](const char *key, Json value) { Json s = base; s[key] = value; return s; };
    Rejects(with("format", "grpc"), "unknown format 'grpc'");
    Rejects(with("url", "http://example.com/v1"), "url must use https");
    Rejects(with("url", "https://example.com/{account}/x"), "undeclared placeholder {account}");
    Rejects(with("colour", "blue"), "unknown provider field 'colour'");
    Rejects(with("name", "Bad Name"), "must be lowercase");
    Rejects(with("default_model", "../x"), "invalid default_model");
    Rejects(with("models", Json::array({"m2"})), "default_model must be listed in models");
    Rejects(with("max_questions", 0), "max_questions must be a positive integer");
    Rejects(with("params", Json::array({{{"name", "unused"}, {"pattern", "x"}}})), "param 'unused' is not used in url");
    Rejects(with("params", Json::array({{{"name", "api_key"}, {"pattern", "x"}}})), "invalid or duplicate param name");
    Rejects(with("auth", {{"header", "Bad Header"}}), "invalid auth header name");
    Rejects(with("question_names", {{"pattern", "[a-z]+"}}), "question_names needs exactly");
    Json missing = base;
    missing.erase("key_env");
    Rejects(missing, "'key_env' is required");
    try {
        decide::ProviderRegistry duplicate({base.dump(), base.dump()});
        Expect(false, "accepted duplicate provider names");
    } catch (const std::exception &e) {
        Expect(std::string(e.what()).find("duplicate provider 'demo'") != std::string::npos, "duplicate error");
    }

    Json prefixed = base;
    prefixed["name"] = "demo2";
    try {
        decide::ProviderRegistry clash({base.dump(), prefixed.dump()});
        Expect(false, "accepted provider names that are prefixes of each other");
    } catch (const std::exception &e) {
        Expect(std::string(e.what()).find("'demo' is a prefix of 'demo2'") != std::string::npos, "prefix error");
    }

    // The System One format applies a provider's question limits.
    Json limited = base;
    limited["max_questions"] = 1;
    limited["question_names"] = {{"pattern", "[a-z_0-9]+"}, {"rule", "lowercase"}};
    auto provider = decide::ParseProvider(limited.dump());
    Json two = Json::array({{{"type", "predicate"}, {"name", "a"}, {"instructions", "Q"}},
                            {{"type", "predicate"}, {"name", "b"}, {"instructions", "Q"}}});
    try {
        provider.wire->Encode("m1", "x", two);
        Expect(false, "accepted too many questions");
    } catch (const std::exception &e) {
        Expect(std::string(e.what()).find("at most 1 questions") != std::string::npos, "max_questions error");
    }

    if (failures) return 1;
    std::cout << "Provider files valid: " << files.size() << "; validator checks passed.\n";
    return 0;
}
