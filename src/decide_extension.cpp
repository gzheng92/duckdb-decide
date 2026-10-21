#define DUCKDB_EXTENSION_MAIN
#include "duckdb.hpp"
#include "decide_extension.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/config.hpp"
#include "client.hpp"
#include <cstdlib>
#include <map>
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
    Value provider; if (context.TryGetCurrentSetting("decisions_provider", provider)) config.provider = provider.ToString();
    Value model; if (context.TryGetCurrentSetting("decisions_model", model)) config.model = model.ToString();
    Value endpoint; if (context.TryGetCurrentSetting("decisions_endpoint", endpoint)) config.endpoint = endpoint.ToString();
    if (config.Cloudflare()) {
        reader.TryGetSecretKey("account_id", config.account_id);
        if (config.key.empty()) { const char *key = std::getenv("CLOUDFLARE_API_TOKEN"); if (key) config.key = key; }
        if (config.account_id.empty()) { const char *id = std::getenv("CLOUDFLARE_ACCOUNT_ID"); if (id) config.account_id = id; }
    } else if (config.key.empty()) { const char *key = std::getenv("OPENAI_API_KEY"); if (key) config.key = key; }
    return config;
}
enum class Mode { Predicate, Probability, Choice, Score, Answers };
struct PendingRow {
    idx_t row;
    string input;
    Json questions;
    double threshold;
};
struct PendingBatch {
    vector<PendingRow *> rows;
    string input;
    Json questions;
};
// Split large batches before issuing any requests. A singleton keeps the original
// input and question, so batch_size=1 is a direct comparison baseline.
static void PlanBatch(const vector<PendingRow *> &rows, vector<PendingBatch> &batches) {
    PendingBatch batch;
    batch.rows = rows;
    if (rows.size() == 1) {
        batch.input = rows[0]->input;
        batch.questions = rows[0]->questions;
    } else {
        Json evidence = Json::array();
        batch.questions = Json::array();
        for (auto *row : rows) {
            auto id = "row_" + std::to_string(row->row);
            evidence.push_back({{"id", id}, {"text", row->input}});
            auto question = row->questions[0];
            question["name"] = id;
            question["instructions"] = "The input is a JSON object containing rows. Evaluate only the text of the "
                "record whose id is " + id + ". Ignore all other records. Question: " +
                question["instructions"].get<string>();
            batch.questions.push_back(std::move(question));
        }
        batch.input = Json({{"rows", evidence}}).dump();
    }
    auto bytes = Json({{"model", "gpt-6-luna"}, {"input", batch.input}, {"questions", batch.questions}}).dump().size();
    if (bytes > decide::MAX_BYTES) {
        if (rows.size() == 1) throw std::runtime_error("Request exceeds extension's 16 MiB limit");
        auto middle = rows.begin() + rows.size() / 2;
        PlanBatch(vector<PendingRow *>(rows.begin(), middle), batches);
        PlanBatch(vector<PendingRow *>(middle, rows.end()), batches);
        return;
    }
    batches.push_back(std::move(batch));
}
template <Mode MODE> static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
    result.SetVectorType(VectorType::FLAT_VECTOR);
    try {
        vector<PendingRow> pending;
        pending.reserve(args.size());
        // Validate the entire chunk before making paid requests.
        for (idx_t row = 0; row < args.size(); ++row) {
            bool missing = false;
            for (idx_t col = 0; col < args.ColumnCount(); ++col) if (args.data[col].GetValue(row).IsNull()) missing = true;
            if (missing) { result.SetValue(row, Value()); continue; }
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
            pending.push_back({row, std::move(input), std::move(questions), threshold});
        }
        if (pending.empty()) return;
        auto config = Configuration(state.GetContext());
        if (MODE == Mode::Answers) {
            // Preserve user-defined names and shared-input semantics for explicit questions.
            for (const auto &row : pending) result.SetValue(row.row, Value(decide::Evaluate(config, row.input, row.questions).dump()));
            return;
        }
        Value setting;
        state.GetContext().TryGetCurrentSetting("decisions_batch_size", setting);
        auto batch_size = setting.GetValue<uint64_t>();
        if (batch_size < 1 || batch_size > 128) throw std::runtime_error("decisions_batch_size must be between 1 and 128");
        if (config.Cloudflare()) batch_size = MinValue<uint64_t>(batch_size, decide::CLOUDFLARE_MAX_QUESTIONS);
        std::map<string, vector<PendingRow *>> groups;
        for (auto &row : pending) groups[row.questions.dump()].push_back(&row);
        vector<PendingBatch> batches;
        for (const auto &entry : groups) {
            const auto &rows = entry.second;
            for (idx_t start = 0; start < rows.size(); start += batch_size) {
                auto end = MinValue<idx_t>(start + batch_size, rows.size());
                PlanBatch(vector<PendingRow *>(rows.begin() + start, rows.begin() + end), batches);
            }
        }
        for (const auto &batch : batches) {
            auto answers = decide::Evaluate(config, batch.input, batch.questions);
            for (idx_t i = 0; i < batch.rows.size(); ++i) {
                const auto &row = *batch.rows[i];
                const auto &answer = answers[i];
                if (answer["type"] == "refusal") throw std::runtime_error(std::string(config.Name()) + " refused this decision");
                if (MODE == Mode::Predicate) result.SetValue(row.row, Value(answer["probability"].get<double>() >= row.threshold));
                if (MODE == Mode::Probability) result.SetValue(row.row, Value(answer["probability"].get<double>()));
                if (MODE == Mode::Choice) result.SetValue(row.row, Value(answer["choice"].get<string>()));
                if (MODE == Mode::Score) result.SetValue(row.row, Value(answer["score"].get<double>()));
            }
        }
    } catch (const Exception &) { throw; // Keep DuckDB's own error types, e.g. permission errors.
    } catch (const std::exception &e) { throw InvalidInputException("Decisions: %s", e.what()); }
}
static void LoadInternal(ExtensionLoader &loader) {
    SecretType type; type.name = "decisions"; type.default_provider = "config"; type.extension = "decide";
    type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>; loader.RegisterSecretType(type);
    CreateSecretFunction secret; secret.secret_type = "decisions"; secret.provider = "config"; secret.function = CreateSecret;
    secret.named_parameters["api_key"] = LogicalType::VARCHAR;
    secret.named_parameters["account_id"] = LogicalType::VARCHAR; loader.RegisterFunction(secret);
    DBConfig::GetConfig(loader.GetDatabaseInstance()).AddExtensionOption("decisions_endpoint", "OpenAI Decisions URL; loopback HTTP allowed for mock tests", LogicalType::VARCHAR, Value("https://api.openai.com/v1/decisions"));
    DBConfig::GetConfig(loader.GetDatabaseInstance()).AddExtensionOption("decisions_provider", "Backend: 'openai' (default) or 'cloudflare' (Workers AI Clef)", LogicalType::VARCHAR, Value("openai"));
    DBConfig::GetConfig(loader.GetDatabaseInstance()).AddExtensionOption("decisions_model", "Cloudflare model: 'clef' (default) or 'clef-flash'", LogicalType::VARCHAR, Value("clef"));
    DBConfig::GetConfig(loader.GetDatabaseInstance()).AddExtensionOption("decisions_batch_size", "Rows per request for typed functions (1 to 128)", LogicalType::UBIGINT, Value::UBIGINT(20));
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
void DecideExtension::Load(ExtensionLoader &loader) { LoadInternal(loader); }
std::string DecideExtension::Name() { return "decide"; }
std::string DecideExtension::Version() const {
#ifdef EXT_VERSION_DECIDE
    return EXT_VERSION_DECIDE; // From EXTENSION_VERSION in extension_config.cmake.
#else
    return "";
#endif
}
}
extern "C" { DUCKDB_CPP_EXTENSION_ENTRY(decide, loader) { duckdb::LoadInternal(loader); } }
