#define DUCKDB_EXTENSION_MAIN
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/config.hpp"
#include "client.hpp"
#include <cstdlib>
namespace duckdb {
using decide::Json;
static unique_ptr<BaseSecret> CreateSecret(ClientContext &, CreateSecretInput &input) {
    auto scopes = input.scope.empty() ? vector<string>{"decisions://"} : input.scope;
    auto secret = make_uniq<KeyValueSecret>(scopes, input.type, input.provider, input.name);
    for (const auto &entry : input.options) secret->secret_map[entry.first] = entry.second;
    secret->redact_keys.insert("api_key");
    return std::move(secret);
}
static decide::Config Configuration(ClientContext &context) {
    Value access;
    if (context.TryGetCurrentSetting("enable_external_access", access) && !access.GetValue<bool>()) throw PermissionException("Decisions requires external access");
    decide::Config config;
    KeyValueSecretReader reader(DatabaseInstance::GetDatabase(context), "decisions", "decisions://");
    reader.TryGetSecretKey("api_key", config.key);
    if (config.key.empty()) { const char *key = std::getenv("OPENAI_API_KEY"); if (key) config.key = key; }
    Value endpoint; if (context.TryGetCurrentSetting("decisions_endpoint", endpoint)) config.endpoint = endpoint.ToString();
    return config;
}
enum class Mode { Predicate, Probability, Choice, Score, Answers };
template <Mode MODE> static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
    result.SetVectorType(VectorType::FLAT_VECTOR);
    bool configured = false; decide::Config config;
    for (idx_t row = 0; row < args.size(); ++row) {
        bool missing = false;
        for (idx_t col = 0; col < args.ColumnCount(); ++col) if (args.data[col].GetValue(row).IsNull()) missing = true;
        if (missing) { result.SetValue(row, Value()); continue; }
        try {
            auto input = args.data[0].GetValue(row).ToString();
            auto instructions = args.data[1].GetValue(row).ToString();
            Json questions;
            if (MODE == Mode::Answers) questions = Json::parse(instructions);
            else {
                Json labels = nullptr;
                if (MODE == Mode::Choice || MODE == Mode::Score) {
                    labels = Json::array();
                    auto list = args.data[2].GetValue(row);
                    for (const auto &label : ListValue::GetChildren(list)) {
                        if (label.IsNull()) throw std::runtime_error("NULL option is not allowed");
                        labels.push_back(label.ToString());
                    }
                }
                auto type = MODE == Mode::Choice ? "choice" : MODE == Mode::Score ? "score" : "predicate";
                questions = Json::array({decide::Question(type, instructions, labels)});
            }
            double threshold = 0.5;
            if (MODE == Mode::Predicate && args.ColumnCount() == 3) {
                threshold = args.data[2].GetValue(row).GetValue<double>();
                if (!std::isfinite(threshold) || threshold < 0 || threshold > 1) throw std::runtime_error("Threshold must be between 0 and 1");
            }
            if (!configured) { config = Configuration(state.GetContext()); configured = true; }
            auto answers = decide::Evaluate(config, input, questions);
            if (MODE == Mode::Answers) { result.SetValue(row, Value(answers.dump())); continue; }
            const auto &answer = answers[0];
            if (answer["type"] == "refusal") throw std::runtime_error("OpenAI refused this decision");
            if (MODE == Mode::Predicate) result.SetValue(row, Value(answer["probability"].get<double>() >= threshold));
            if (MODE == Mode::Probability) result.SetValue(row, Value(answer["probability"].get<double>()));
            if (MODE == Mode::Choice) result.SetValue(row, Value(answer["choice"].get<string>()));
            if (MODE == Mode::Score) result.SetValue(row, Value(answer["score"].get<double>()));
        } catch (const std::exception &e) { throw InvalidInputException("Decisions: %s", e.what()); }
    }
}
static void LoadInternal(ExtensionLoader &loader) {
    SecretType type; type.name = "decisions"; type.default_provider = "config"; type.extension = "decide";
    type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>; loader.RegisterSecretType(type);
    CreateSecretFunction secret; secret.secret_type = "decisions"; secret.provider = "config"; secret.function = CreateSecret;
    secret.named_parameters["api_key"] = LogicalType::VARCHAR; loader.RegisterFunction(secret);
    DBConfig::GetConfig(loader.GetDatabaseInstance()).AddExtensionOption("decisions_endpoint", "OpenAI Decisions URL; loopback HTTP allowed for mock tests", LogicalType::VARCHAR, Value("https://api.openai.com/v1/decisions"));
    auto add = [&](const char *name, vector<LogicalType> args, LogicalType output, scalar_function_t fun) {
        ScalarFunction function(name, args, output, fun);
        function.SetStability(FunctionStability::VOLATILE);
        function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
        loader.RegisterFunction(function);
    };
    add("decision", {LogicalType::ANY, LogicalType::VARCHAR}, LogicalType::BOOLEAN, Execute<Mode::Predicate>);
    loader.AddFunctionOverload(ScalarFunction("decision", {LogicalType::ANY, LogicalType::VARCHAR, LogicalType::DOUBLE}, LogicalType::BOOLEAN, Execute<Mode::Predicate>, nullptr, nullptr, nullptr, nullptr, LogicalType::INVALID, FunctionStability::VOLATILE, FunctionNullHandling::SPECIAL_HANDLING));
    add("decision_prob", {LogicalType::ANY, LogicalType::VARCHAR}, LogicalType::DOUBLE, Execute<Mode::Probability>);
    add("decision_choice", {LogicalType::ANY, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)}, LogicalType::VARCHAR, Execute<Mode::Choice>);
    add("decision_score", {LogicalType::ANY, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)}, LogicalType::DOUBLE, Execute<Mode::Score>);
    add("decision_answers", {LogicalType::ANY, LogicalType::VARCHAR}, LogicalType::VARCHAR, Execute<Mode::Answers>);
}
}
extern "C" { DUCKDB_CPP_EXTENSION_ENTRY(decide, loader) { duckdb::LoadInternal(loader); } }
