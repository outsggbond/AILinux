#include "reputation.hpp"

#include <sqlite3.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>

#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <numeric>
#include <limits>
#include <regex>
#include <sstream>
#include <tuple>

namespace ailinux::ai {
namespace {

constexpr int kSchemaVersion = 9;
constexpr std::size_t kMinimumEvaluatedTasks = 20;
constexpr std::size_t kRollingWindow = 100;
constexpr double kErrorRateLimit = 0.10;
const std::array<const char*, 12> kDomains = {
    "linux-kernel", "system-architecture", "cpp-development",
    "software-development", "python-ai", "system-diagnosis",
    "cyber-security", "system-deployment", "performance-optimization",
    "networking", "data-management", "testing-qa"};

[[nodiscard]] bool contains(const auto& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void require_text(const std::string& value, const char* field) {
    if (value.empty() || value.find('\0') != std::string::npos ||
        value.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::invalid_argument(std::string(field) + " must be non-empty text");
    }
}

void require_domain(const std::string& domain) {
    if (!contains(kDomains, domain)) {
        throw std::invalid_argument("unsupported professional domain");
    }
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
    gmtime_r(&time, &value);
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &value);
    return buffer.data();
}

std::int64_t to_microusd(double amount) {
    if (!std::isfinite(amount) || amount < 0 ||
        amount > static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
                     1'000'000.0) {
        throw std::invalid_argument("USD amount is invalid or out of range");
    }
    return static_cast<std::int64_t>(std::ceil(amount * 1'000'000.0));
}

double from_microusd(std::int64_t amount) {
    if (amount < 0) {
        throw ReputationError("stored USD amount is negative");
    }
    return static_cast<double>(amount) / 1'000'000.0;
}

std::string fingerprint(const std::string& provider, const std::string& model,
                       const std::string& model_version,
                       const std::string& prompt_version,
                       const std::string& tools_version) {
    const auto material = nlohmann::json::array(
                              {provider, model, model_version, prompt_version,
                               tools_version})
                              .dump(-1, ' ', true);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(material.data(), material.size(), digest.data(), &length,
                   EVP_sha256(), nullptr) != 1) {
        throw ReputationError("cannot calculate Agent configuration fingerprint");
    }
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < length; ++index) {
        result << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return result.str();
}

class Statement {
public:
    Statement(sqlite3* database, const char* query) : database_(database) {
        if (sqlite3_prepare_v2(database_, query, -1, &statement_, nullptr) != SQLITE_OK) {
            throw ReputationError(sqlite3_errmsg(database_));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.c_str(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK) {
            throw ReputationError(sqlite3_errmsg(database_));
        }
    }
    void bind(int index, int value) {
        if (sqlite3_bind_int(statement_, index, value) != SQLITE_OK) {
            throw ReputationError(sqlite3_errmsg(database_));
        }
    }
    void bind(int index, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw ReputationError(sqlite3_errmsg(database_));
        }
    }
    void bind_null(int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw ReputationError(sqlite3_errmsg(database_));
        }
    }
    [[nodiscard]] bool step() {
        const auto result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result == SQLITE_DONE) {
            return false;
        }
        throw ReputationError(sqlite3_errmsg(database_));
    }
    void run() {
        if (step()) {
            throw ReputationError("SQLite mutation unexpectedly returned a row");
        }
    }
    [[nodiscard]] std::string text(int column) const {
        const auto* value = sqlite3_column_text(statement_, column);
        return value == nullptr ? std::string{} :
                                  reinterpret_cast<const char*>(value);
    }
    [[nodiscard]] std::optional<std::string> optional_text(int column) const {
        if (sqlite3_column_type(statement_, column) == SQLITE_NULL) {
            return std::nullopt;
        }
        return text(column);
    }
    [[nodiscard]] std::int64_t integer(int column) const {
        return sqlite3_column_int64(statement_, column);
    }
    [[nodiscard]] std::optional<std::int64_t>
    optional_integer(int column) const {
        if (sqlite3_column_type(statement_, column) == SQLITE_NULL) {
            return std::nullopt;
        }
        return integer(column);
    }
    [[nodiscard]] bool boolean(int column) const { return integer(column) != 0; }

private:
    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* database) : database_(database) {
        char* error = nullptr;
        if (sqlite3_exec(database_, "BEGIN IMMEDIATE", nullptr, nullptr, &error) !=
            SQLITE_OK) {
            const std::string message = error == nullptr ? sqlite3_errmsg(database_) : error;
            sqlite3_free(error);
            throw ReputationError(message);
        }
    }
    ~Transaction() {
        if (!committed_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    void commit() {
        char* error = nullptr;
        if (sqlite3_exec(database_, "COMMIT", nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error == nullptr ? sqlite3_errmsg(database_) : error;
            sqlite3_free(error);
            throw ReputationError(message);
        }
        committed_ = true;
    }

private:
    sqlite3* database_;
    bool committed_ = false;
};

void exec(sqlite3* database, const char* query) {
    char* error = nullptr;
    if (sqlite3_exec(database, query, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error == nullptr ? sqlite3_errmsg(database) : error;
        sqlite3_free(error);
        throw ReputationError(message);
    }
}

} // namespace

class AgentReputationStore::Impl {
public:
    explicit Impl(std::string path) : path_(std::move(path)) {
        const std::filesystem::path database(path_);
        if (!database.is_absolute() || !std::filesystem::is_directory(database.parent_path())) {
            throw std::invalid_argument("reputation database path must be absolute");
        }
        struct stat parent {};
        if (stat(database.parent_path().c_str(), &parent) != 0 ||
            parent.st_uid != geteuid() || (parent.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            throw std::runtime_error(
                "reputation database directory must be owned by this user and private");
        }
        int descriptor = open(path_.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC |
                                                   O_NOFOLLOW,
                              0600);
        const bool created = descriptor >= 0;
        if (!created && errno == EEXIST) {
            descriptor = open(path_.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        }
        if (descriptor < 0) {
            throw ReputationError("cannot securely open reputation database");
        }
        struct stat info {};
        if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_uid != geteuid() || info.st_nlink != 1 ||
            (!created && (info.st_mode & 0077) != 0)) {
            close(descriptor);
            throw std::runtime_error("reputation database file permissions are unsafe");
        }
        if (created && fchmod(descriptor, 0600) != 0) {
            close(descriptor);
            throw ReputationError("cannot restrict reputation database permissions");
        }
        close(descriptor);
        if (sqlite3_open_v2(path_.c_str(), &database_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message =
                database_ == nullptr ? "cannot open SQLite database"
                                     : sqlite3_errmsg(database_);
            if (database_ != nullptr) {
                sqlite3_close(database_);
                database_ = nullptr;
            }
            throw ReputationError(message);
        }
        chmod(path_.c_str(), 0600);
        sqlite3_busy_timeout(database_, 10000);
        exec(database_, "PRAGMA foreign_keys=ON");
        exec(database_, "PRAGMA journal_mode=DELETE");
        initialize_schema();
    }

    ~Impl() {
        if (database_ != nullptr) {
            sqlite3_close(database_);
        }
    }

    void initialize_schema() {
        Statement version(database_, "PRAGMA user_version");
        const int schema = version.step() ? static_cast<int>(version.integer(0)) : 0;
        if (schema == kSchemaVersion) {
            return;
        }
        if (schema != 0 && schema != 1 && schema != 2 && schema != 3 &&
            schema != 4 && schema != 5 && schema != 6 && schema != 7 &&
            schema != 8) {
            throw ReputationError("unsupported reputation database schema version");
        }
        Transaction transaction(database_);
        if (schema == 0) {
            exec(database_, R"sql(
                CREATE TABLE agents(
                    agent_id TEXT PRIMARY KEY,provider TEXT NOT NULL,model TEXT NOT NULL,
                    model_version TEXT NOT NULL,prompt_version TEXT NOT NULL,
                    tool_config_version TEXT NOT NULL,fingerprint TEXT NOT NULL,
                    status TEXT NOT NULL CHECK(status IN
                        ('ready','offline','suspended','disabled')),
                    allow_leader INTEGER NOT NULL CHECK(allow_leader IN (0,1)),
                    allow_inspector INTEGER NOT NULL CHECK(allow_inspector IN (0,1)),
                    created_at TEXT NOT NULL,updated_at TEXT NOT NULL);
                CREATE TABLE assignments(
                    task_id TEXT NOT NULL,agent_id TEXT NOT NULL REFERENCES agents(agent_id),
                    domain TEXT NOT NULL,difficulty INTEGER NOT NULL
                        CHECK(difficulty BETWEEN 1 AND 5),
                    fingerprint TEXT NOT NULL,assigned_at TEXT NOT NULL,
                    PRIMARY KEY(task_id,agent_id));
                CREATE TABLE reviews(
                    task_id TEXT NOT NULL,agent_id TEXT NOT NULL REFERENCES agents(agent_id),
                    domain TEXT NOT NULL,difficulty INTEGER NOT NULL
                        CHECK(difficulty BETWEEN 1 AND 5),
                    fingerprint TEXT NOT NULL,verdict TEXT NOT NULL,
                    severity TEXT NOT NULL,evaluator_id TEXT NOT NULL,
                    evidence_sha256 TEXT NOT NULL,reviewed_at TEXT NOT NULL,resolved_at TEXT,
                    PRIMARY KEY(task_id,agent_id));
                CREATE INDEX reviews_recent_global
                    ON reviews(agent_id,fingerprint,reviewed_at DESC);
                CREATE INDEX reviews_recent_domain
                    ON reviews(agent_id,domain,fingerprint,reviewed_at DESC);
                CREATE TABLE elections(
                    election_id INTEGER PRIMARY KEY AUTOINCREMENT,role TEXT NOT NULL,
                    domain TEXT NOT NULL,selected_agent_id TEXT REFERENCES agents(agent_id),
                    challengers_json TEXT NOT NULL,rationale TEXT NOT NULL,
                    elected_at TEXT NOT NULL);
                CREATE TABLE active_roles(
                    role TEXT NOT NULL,domain TEXT NOT NULL,
                    agent_id TEXT NOT NULL REFERENCES agents(agent_id),
                    election_id INTEGER NOT NULL REFERENCES elections(election_id),
                    PRIMARY KEY(role,domain));
            )sql");
        } else if (schema == 1) {
            exec(database_, R"sql(
                ALTER TABLE agents ADD COLUMN allow_leader INTEGER NOT NULL DEFAULT 1
                    CHECK(allow_leader IN (0,1));
                ALTER TABLE agents ADD COLUMN allow_inspector INTEGER NOT NULL DEFAULT 1
                    CHECK(allow_inspector IN (0,1));
                ALTER TABLE assignments ADD COLUMN fingerprint TEXT NOT NULL DEFAULT '';
                CREATE TABLE elections(
                    election_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    role TEXT NOT NULL, domain TEXT NOT NULL,
                    selected_agent_id TEXT REFERENCES agents(agent_id),
                    challengers_json TEXT NOT NULL, rationale TEXT NOT NULL,
                    elected_at TEXT NOT NULL);
                CREATE TABLE active_roles(
                    role TEXT NOT NULL, domain TEXT NOT NULL,
                    agent_id TEXT NOT NULL REFERENCES agents(agent_id),
                    election_id INTEGER NOT NULL REFERENCES elections(election_id),
                    PRIMARY KEY(role,domain));
            )sql");
        }
        if (schema == 0 || schema == 1 || schema == 2) {
            exec(database_, R"sql(
                CREATE TABLE inspections(
                    task_id TEXT NOT NULL,
                    inspector_agent_id TEXT NOT NULL REFERENCES agents(agent_id),
                    inspected_agent_id TEXT NOT NULL REFERENCES agents(agent_id),
                    domain TEXT NOT NULL, difficulty INTEGER NOT NULL,
                    fingerprint TEXT NOT NULL, classification TEXT NOT NULL,
                    evaluator_id TEXT NOT NULL, evidence_sha256 TEXT NOT NULL,
                    reviewed_at TEXT NOT NULL,
                    PRIMARY KEY(task_id,inspector_agent_id));
                CREATE INDEX inspections_recent
                    ON inspections(inspector_agent_id,fingerprint,reviewed_at DESC);
                CREATE UNIQUE INDEX one_active_inspector ON active_roles(role)
                    WHERE role='inspector';
            )sql");
        }
        if (schema <= 3) {
            create_task_schema();
        }
        if (schema <= 4) {
            create_tool_audit_schema();
        }
        if (schema <= 5) {
            exec(database_, R"sql(
                ALTER TABLE tasks ADD COLUMN prompt_tokens INTEGER
                    CHECK(prompt_tokens IS NULL OR prompt_tokens>=0);
                ALTER TABLE tasks ADD COLUMN completion_tokens INTEGER
                    CHECK(completion_tokens IS NULL OR completion_tokens>=0);
            )sql");
        }
        if (schema <= 6) {
            exec(database_, R"sql(
                ALTER TABLE tasks ADD COLUMN cost_microusd INTEGER
                    CHECK(cost_microusd IS NULL OR cost_microusd>=0);
                CREATE TABLE agent_cost_budgets(
                    agent_id TEXT PRIMARY KEY REFERENCES agents(agent_id),
                    limit_microusd INTEGER NOT NULL CHECK(limit_microusd>=0),
                    spent_microusd INTEGER NOT NULL DEFAULT 0
                        CHECK(spent_microusd>=0));
                CREATE TABLE task_cost_reservations(
                    task_id TEXT PRIMARY KEY REFERENCES tasks(task_id),
                    agent_id TEXT NOT NULL REFERENCES agents(agent_id),
                    reserved_microusd INTEGER NOT NULL CHECK(reserved_microusd>=0),
                    actual_microusd INTEGER,
                    state TEXT NOT NULL CHECK(state IN ('reserved','settled','released')),
                    updated_at TEXT NOT NULL,
                    CHECK(actual_microusd IS NULL OR actual_microusd>=0));
                CREATE INDEX task_cost_agent_state
                    ON task_cost_reservations(agent_id,state);
            )sql");
        }
        if (schema <= 7) {
            exec(database_, R"sql(
                ALTER TABLE agents ADD COLUMN max_concurrent_tasks INTEGER NOT NULL
                    DEFAULT 2 CHECK(max_concurrent_tasks BETWEEN 1 AND 256);
            )sql");
        }
        if (schema <= 8) {
            exec(database_, R"sql(
                ALTER TABLE agents ADD COLUMN max_provider_requests INTEGER NOT NULL
                    DEFAULT 0 CHECK(max_provider_requests>=0);
                ALTER TABLE agents ADD COLUMN provider_requests_used INTEGER NOT NULL
                    DEFAULT 0 CHECK(provider_requests_used>=0);
                CREATE TABLE task_provider_requests(
                    task_id TEXT PRIMARY KEY REFERENCES tasks(task_id),
                    agent_id TEXT NOT NULL REFERENCES agents(agent_id),
                    reserved_at TEXT NOT NULL);
                CREATE INDEX task_provider_requests_agent
                    ON task_provider_requests(agent_id,reserved_at);
            )sql");
        }
        exec(database_, "PRAGMA user_version=9");
        transaction.commit();
    }

    void create_task_schema() {
        exec(database_, R"sql(
            CREATE TABLE tasks(
                task_id TEXT PRIMARY KEY, agent_id TEXT NOT NULL,
                state TEXT NOT NULL CHECK(state IN
                    ('queued','running','completed','failed','cancelled','interrupted')),
                submitted_at TEXT NOT NULL, started_at TEXT, finished_at TEXT,
                error_category TEXT,
                FOREIGN KEY(task_id,agent_id) REFERENCES assignments(task_id,agent_id));
            CREATE INDEX tasks_agent_history ON tasks(agent_id,submitted_at DESC);
            CREATE TABLE task_events(
                event_id INTEGER PRIMARY KEY AUTOINCREMENT,
                task_id TEXT NOT NULL REFERENCES tasks(task_id),
                from_state TEXT,
                to_state TEXT NOT NULL CHECK(to_state IN
                    ('queued','running','completed','failed','cancelled','interrupted')),
                occurred_at TEXT NOT NULL,error_category TEXT);
            CREATE INDEX task_events_history ON task_events(task_id,event_id);
        )sql");
    }

    void create_tool_audit_schema() {
        exec(database_, R"sql(
            CREATE TABLE tool_audit(
                event_id INTEGER PRIMARY KEY AUTOINCREMENT,
                request_id TEXT NOT NULL,
                task_id TEXT NOT NULL,
                agent_id TEXT NOT NULL REFERENCES agents(agent_id),
                tool_name TEXT NOT NULL,
                arguments_sha256 TEXT NOT NULL
                    CHECK(length(arguments_sha256)=64),
                decision TEXT NOT NULL CHECK(decision IN
                    ('denied','approval-required','started','executed','failed')),
                outcome_category TEXT,
                occurred_at TEXT NOT NULL,
                FOREIGN KEY(task_id) REFERENCES tasks(task_id));
            CREATE INDEX tool_audit_task_history
                ON tool_audit(task_id,event_id);
        )sql");
    }

    [[nodiscard]] AgentIdentity agent(const std::string& id) const {
        Statement query(database_, R"sql(
            SELECT agent_id,provider,model,model_version,prompt_version,
                   tool_config_version,status,allow_leader,allow_inspector,
                   max_concurrent_tasks,max_provider_requests,provider_requests_used
            FROM agents WHERE agent_id=?
        )sql");
        query.bind(1, id);
        if (!query.step()) {
            throw std::out_of_range("unknown Agent: " + id);
        }
        return {query.text(0), query.text(1), query.text(2), query.text(3),
                query.text(4), query.text(5), query.text(6), query.boolean(7),
                query.boolean(8), static_cast<std::size_t>(query.integer(9)),
                static_cast<std::uint64_t>(query.integer(10)),
                static_cast<std::uint64_t>(query.integer(11))};
    }

    [[nodiscard]] std::string agent_fingerprint(const std::string& id) const {
        Statement query(database_, "SELECT fingerprint FROM agents WHERE agent_id=?");
        query.bind(1, id);
        if (!query.step()) {
            throw std::out_of_range("unknown Agent: " + id);
        }
        return query.text(0);
    }

    [[nodiscard]] bool unresolved_incident(const std::string& id) const {
        Statement query(database_, R"sql(
            SELECT 1 FROM reviews WHERE agent_id=? AND severity='critical-security'
              AND resolved_at IS NULL LIMIT 1
        )sql");
        query.bind(1, id);
        return query.step();
    }

    [[nodiscard]] Performance performance(const std::string& id,
                                          std::optional<std::string> domain) const {
        const auto identity = agent(id);
        if (domain.has_value()) {
            require_domain(*domain);
        }
        const auto current = agent_fingerprint(id);
        const auto fetch = [&](std::optional<std::string> target_domain) {
            Statement query(database_, target_domain.has_value()
                                           ? R"sql(SELECT verdict,difficulty FROM reviews
                                                WHERE agent_id=? AND fingerprint=?
                                                  AND domain=? AND verdict IN
                                                    ('correct','agent-error')
                                                ORDER BY rowid DESC LIMIT 100)sql"
                                           : R"sql(SELECT verdict,difficulty FROM reviews
                                                WHERE agent_id=? AND fingerprint=?
                                                  AND verdict IN ('correct','agent-error')
                                                ORDER BY rowid DESC LIMIT 100)sql");
            query.bind(1, id);
            query.bind(2, current);
            if (target_domain.has_value()) {
                query.bind(3, *target_domain);
            }
            std::vector<std::pair<std::string, int>> rows;
            while (query.step()) {
                rows.emplace_back(query.text(0), static_cast<int>(query.integer(1)));
            }
            return rows;
        };
        const auto global = fetch(std::nullopt);
        const auto rows = fetch(domain);
        const auto errors = static_cast<std::size_t>(std::count_if(
            rows.begin(), rows.end(), [](const auto& row) {
                return row.first == "agent-error";
            }));
        const auto global_errors = static_cast<std::size_t>(std::count_if(
            global.begin(), global.end(), [](const auto& row) {
                return row.first == "agent-error";
            }));
        const auto total_difficulty = std::accumulate(
            rows.begin(), rows.end(), std::int64_t{0},
            [](auto sum, const auto& row) { return sum + row.second; });
        const auto correct_difficulty = std::accumulate(
            rows.begin(), rows.end(), std::int64_t{0},
            [](auto sum, const auto& row) {
                return sum + (row.first == "correct" ? row.second : 0);
            });
        Performance result{id, domain, rows.size(), errors, std::nullopt,
                          std::nullopt, false, {}};
        if (!rows.empty()) {
            result.error_rate = static_cast<double>(errors) / rows.size();
        }
        if (total_difficulty > 0) {
            result.weighted_score =
                static_cast<double>(correct_difficulty) / total_difficulty;
        }
        const bool unresolved = unresolved_incident(id);
        const auto global_rate = global.empty()
                                     ? std::optional<double>{}
                                     : std::optional<double>(
                                           static_cast<double>(global_errors) /
                                           global.size());
        if (identity.status != "ready") {
            result.reason = "Agent status is " + identity.status;
        } else if (unresolved) {
            result.reason = "unresolved critical security incident";
        } else if (global.size() < kMinimumEvaluatedTasks) {
            result.reason = "requires 20 current-version globally reviewed tasks";
        } else if (global_rate.has_value() && *global_rate > kErrorRateLimit) {
            result.reason = "global error rate exceeds 10%";
        } else if (domain.has_value() && rows.size() < kMinimumEvaluatedTasks) {
            result.reason = "requires 20 current-version reviews in " + *domain;
        } else if (domain.has_value() && result.error_rate.has_value() &&
                   *result.error_rate > kErrorRateLimit) {
            result.reason = *domain + " error rate exceeds 10%";
        } else {
            result.eligible_for_governance = true;
            result.reason = "meets global/domain sample, status, incident, and error-rate gates";
        }
        return result;
    }

    [[nodiscard]] TaskRecord task_record(const std::string& id) const {
        Statement query(database_, R"sql(
            SELECT task_id,agent_id,state,submitted_at,started_at,finished_at,
                   error_category,prompt_tokens,completion_tokens,cost_microusd
            FROM tasks WHERE task_id=?
        )sql");
        query.bind(1, id);
        if (!query.step()) {
            throw std::out_of_range("unknown task: " + id);
        }
        const auto optional_token_count = [&](int column)
            -> std::optional<std::uint64_t> {
            const auto stored = query.optional_integer(column);
            if (!stored.has_value()) {
                return std::nullopt;
            }
            if (*stored < 0) {
                throw ReputationError("stored token usage is negative");
            }
            return static_cast<std::uint64_t>(*stored);
        };
        const auto cost = query.optional_integer(9);
        if (cost.has_value() && *cost < 0) {
            throw ReputationError("stored USD cost is negative");
        }
        return {query.text(0), query.text(1), query.text(2), query.text(3),
                query.optional_text(4), query.optional_text(5),
                query.optional_text(6), optional_token_count(7),
                optional_token_count(8),
                cost.has_value()
                    ? std::optional<double>(from_microusd(*cost))
                    : std::nullopt};
    }

    [[nodiscard]] sqlite3* db() const { return database_; }
    [[nodiscard]] std::recursive_mutex& mutex() const { return mutex_; }
    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
    sqlite3* database_ = nullptr;
    mutable std::recursive_mutex mutex_;
};

AgentReputationStore::AgentReputationStore(std::string database_path)
    : impl_(std::make_unique<Impl>(std::move(database_path))) {}

AgentReputationStore::~AgentReputationStore() = default;

void AgentReputationStore::remove_role(const std::string& role,
                                       const std::string& domain,
                                       const std::string& agent_id,
                                       const std::string& reason) {
    Statement election(impl_->db(), R"sql(
        INSERT INTO elections(role,domain,selected_agent_id,challengers_json,
                              rationale,elected_at)
        VALUES(?,?,NULL,'[]',?,?)
    )sql");
    election.bind(1, role);
    election.bind(2, domain);
    election.bind(3, reason);
    election.bind(4, utc_now());
    static_cast<void>(election.step());
    Statement remove(impl_->db(),
                     "DELETE FROM active_roles WHERE role=? AND domain=? AND agent_id=?");
    remove.bind(1, role);
    remove.bind(2, domain);
    remove.bind(3, agent_id);
    static_cast<void>(remove.step());
}

void AgentReputationStore::revoke_ineligible_roles(const std::string& agent_id,
                                                   const std::string& reason) {
    const auto identity = impl_->agent(agent_id);
    std::vector<std::pair<std::string, std::string>> active;
    Statement query(impl_->db(),
                    "SELECT role,domain FROM active_roles WHERE agent_id=?");
    query.bind(1, agent_id);
    while (query.step()) {
        active.emplace_back(query.text(0), query.text(1));
    }
    for (const auto& [role, domain] : active) {
        const bool eligible =
            role == "domain-leader"
                ? identity.allow_leader &&
                      impl_->performance(agent_id, domain).eligible_for_governance
                : role == "inspector" && inspector_performance(agent_id).eligible;
        if (!eligible) {
            remove_role(role, domain, agent_id, reason);
        }
    }
}

AgentIdentity AgentReputationStore::register_agent(
    const std::string& agent_id, const std::string& provider,
    const std::string& model, const std::string& model_version,
    const std::string& prompt_version, const std::string& tool_config_version,
    bool allow_leader, bool allow_inspector) {
    require_text(agent_id, "agent_id");
    require_text(provider, "provider");
    require_text(model, "model");
    require_text(model_version, "model_version");
    require_text(prompt_version, "prompt_version");
    require_text(tool_config_version, "tool_config_version");
    const auto digest =
        fingerprint(provider, model, model_version, prompt_version, tool_config_version);
    const auto now = utc_now();
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    std::optional<std::string> old_digest;
    std::string old_status = "ready";
    {
        Statement old(impl_->db(), "SELECT fingerprint,status FROM agents WHERE agent_id=?");
        old.bind(1, agent_id);
        if (old.step()) {
            old_digest = old.text(0);
            old_status = old.text(1);
        }
    }
    const auto status = old_digest.has_value() && *old_digest != digest
                            ? "offline"
                            : old_status;
    Statement insert(impl_->db(), R"sql(
        INSERT INTO agents(agent_id,provider,model,model_version,prompt_version,
            tool_config_version,fingerprint,status,allow_leader,allow_inspector,
            created_at,updated_at)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(agent_id) DO UPDATE SET provider=excluded.provider,
            model=excluded.model,model_version=excluded.model_version,
            prompt_version=excluded.prompt_version,
            tool_config_version=excluded.tool_config_version,
            fingerprint=excluded.fingerprint,status=excluded.status,
            allow_leader=excluded.allow_leader,
            allow_inspector=excluded.allow_inspector,updated_at=excluded.updated_at
    )sql");
    insert.bind(1, agent_id);
    insert.bind(2, provider);
    insert.bind(3, model);
    insert.bind(4, model_version);
    insert.bind(5, prompt_version);
    insert.bind(6, tool_config_version);
    insert.bind(7, digest);
    insert.bind(8, status);
    insert.bind(9, allow_leader ? 1 : 0);
    insert.bind(10, allow_inspector ? 1 : 0);
    insert.bind(11, old_digest.has_value() ? utc_now() : now);
    insert.bind(12, now);
    insert.run();
    if (status != "ready") {
        revoke_ineligible_roles(agent_id,
                                "Agent configuration changed or status is not ready");
    }
    transaction.commit();
    return impl_->agent(agent_id);
}

AgentIdentity AgentReputationStore::get_agent(const std::string& agent_id) const {
    std::lock_guard lock(impl_->mutex());
    return impl_->agent(agent_id);
}

AgentRuntimeSnapshot AgentReputationStore::runtime_snapshot(
    const std::string& agent_id) const {
    std::lock_guard lock(impl_->mutex());
    const auto identity = impl_->agent(agent_id);
    std::size_t queued = 0;
    std::size_t running = 0;
    Statement tasks(impl_->db(), R"sql(
        SELECT state,COUNT(*) FROM tasks
        WHERE agent_id=? AND state IN ('queued','running')
        GROUP BY state
    )sql");
    tasks.bind(1, agent_id);
    while (tasks.step()) {
        const auto count = tasks.integer(1);
        if (count < 0 ||
            static_cast<std::uint64_t>(count) >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
            throw ReputationError("Agent active task count is out of range");
        }
        if (tasks.text(0) == "queued") {
            queued = static_cast<std::size_t>(count);
        } else {
            running = static_cast<std::size_t>(count);
        }
    }
    std::optional<std::string> last_failure_state;
    std::optional<std::string> last_error_category;
    std::optional<std::string> last_error_at;
    Statement latest(impl_->db(), R"sql(
        SELECT e.to_state,e.error_category,e.occurred_at
        FROM task_events e JOIN tasks t ON t.task_id=e.task_id
        WHERE t.agent_id=? AND e.to_state IN
            ('completed','failed','cancelled','interrupted')
        ORDER BY e.event_id DESC LIMIT 1
    )sql");
    latest.bind(1, agent_id);
    if (latest.step()) {
        last_failure_state = latest.text(0);
        if (last_failure_state == "failed" ||
            last_failure_state == "interrupted") {
            last_error_category = latest.optional_text(1);
            last_error_at = latest.text(2);
        }
    }

    std::string state;
    if (identity.status == "offline" || identity.status == "suspended" ||
        identity.status == "disabled") {
        state = identity.status;
        std::transform(state.begin(), state.end(), state.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::toupper(ch));
                       });
    } else if (running > 0) {
        state = "BUSY";
    } else if (queued > 0) {
        state = "WAITING";
    } else if (last_failure_state == "failed" ||
               last_failure_state == "interrupted") {
        state = "ERROR";
    } else {
        state = "READY";
    }
    return {agent_id, std::move(state), queued, running,
            std::move(last_error_category), std::move(last_error_at)};
}

void AgentReputationStore::set_status(const std::string& agent_id,
                                      const std::string& status) {
    if (!contains(std::array<std::string, 4>{"ready", "offline", "suspended", "disabled"},
                  status)) {
        throw std::invalid_argument("invalid Agent status");
    }
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    if (status == "ready" && impl_->unresolved_incident(agent_id)) {
        throw EligibilityError("Agent has an unresolved critical security incident");
    }
    Statement update(impl_->db(),
                     "UPDATE agents SET status=?,updated_at=? WHERE agent_id=?");
    update.bind(1, status);
    update.bind(2, utc_now());
    update.bind(3, agent_id);
    update.run();
    if (sqlite3_changes(impl_->db()) == 0) {
        throw std::out_of_range("unknown Agent: " + agent_id);
    }
    if (status != "ready") {
        revoke_ineligible_roles(agent_id, "Agent status changed to " + status);
    }
    transaction.commit();
}

void AgentReputationStore::set_agent_max_concurrent_tasks(
    const std::string& agent_id, std::size_t limit) {
    if (limit == 0 || limit > 256) {
        throw std::invalid_argument(
            "Agent concurrent-task limit must be between 1 and 256");
    }
    std::lock_guard lock(impl_->mutex());
    Statement update(impl_->db(), R"sql(
        UPDATE agents SET max_concurrent_tasks=?,updated_at=? WHERE agent_id=?
    )sql");
    update.bind(1, static_cast<int>(limit));
    update.bind(2, utc_now());
    update.bind(3, agent_id);
    update.run();
    if (sqlite3_changes(impl_->db()) != 1) {
        throw std::out_of_range("unknown Agent: " + agent_id);
    }
}

void AgentReputationStore::set_agent_max_provider_requests(
    const std::string& agent_id, std::uint64_t limit) {
    if (limit >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("Agent request limit exceeds SQLite integer range");
    }
    std::lock_guard lock(impl_->mutex());
    Statement update(impl_->db(), R"sql(
        UPDATE agents SET max_provider_requests=?,updated_at=? WHERE agent_id=?
    )sql");
    update.bind(1, static_cast<std::int64_t>(limit));
    update.bind(2, utc_now());
    update.bind(3, agent_id);
    update.run();
    if (sqlite3_changes(impl_->db()) != 1) {
        throw std::out_of_range("unknown Agent: " + agent_id);
    }
}

bool AgentReputationStore::reserve_provider_request(
    const std::string& task_id, const std::string& agent_id) {
    require_text(task_id, "task_id");
    require_text(agent_id, "agent_id");
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    Statement task(impl_->db(),
                   "SELECT agent_id,state FROM tasks WHERE task_id=?");
    task.bind(1, task_id);
    if (!task.step() || task.text(0) != agent_id ||
        (task.text(1) != "queued" && task.text(1) != "running")) {
        throw ReputationError("Provider request task/Agent identity mismatch");
    }
    Statement existing(impl_->db(),
                       "SELECT 1 FROM task_provider_requests WHERE task_id=?");
    existing.bind(1, task_id);
    if (existing.step()) {
        throw TaskConflictError("task already reserved a Provider request");
    }
    Statement update(impl_->db(), R"sql(
        UPDATE agents
        SET provider_requests_used=provider_requests_used+1,updated_at=?
        WHERE agent_id=? AND provider_requests_used<9223372036854775807
          AND (max_provider_requests=0 OR
               provider_requests_used<max_provider_requests)
    )sql");
    update.bind(1, utc_now());
    update.bind(2, agent_id);
    update.run();
    if (sqlite3_changes(impl_->db()) != 1) {
        Statement exists_agent(impl_->db(),
                               "SELECT 1 FROM agents WHERE agent_id=?");
        exists_agent.bind(1, agent_id);
        if (!exists_agent.step()) {
            throw std::out_of_range("unknown Agent: " + agent_id);
        }
        transaction.commit();
        return false;
    }
    Statement insert(impl_->db(), R"sql(
        INSERT INTO task_provider_requests(task_id,agent_id,reserved_at)
        VALUES(?,?,?)
    )sql");
    insert.bind(1, task_id);
    insert.bind(2, agent_id);
    insert.bind(3, utc_now());
    insert.run();
    transaction.commit();
    return true;
}

void AgentReputationStore::record_assignment(const std::string& task_id,
                                            const std::string& agent_id,
                                            const std::string& domain,
                                            int difficulty) {
    require_text(task_id, "task_id");
    require_domain(domain);
    if (difficulty < 1 || difficulty > 5) {
        throw std::invalid_argument("difficulty must be from 1 through 5");
    }
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    const auto identity = impl_->agent(agent_id);
    if (identity.status != "ready") {
        throw EligibilityError("cannot assign work to Agent in " + identity.status +
                               " status");
    }
    const auto current_performance = impl_->performance(agent_id, std::nullopt);
    if (current_performance.error_rate.has_value() &&
        *current_performance.error_rate > kErrorRateLimit && difficulty > 1) {
        throw EligibilityError(
            "Agent error rate exceeds 10%; only difficulty-1 tasks are allowed");
    }
    Statement assignment(impl_->db(), R"sql(
        INSERT INTO assignments(task_id,agent_id,domain,difficulty,fingerprint,assigned_at)
        VALUES(?,?,?,?,?,?)
    )sql");
    assignment.bind(1, task_id);
    assignment.bind(2, agent_id);
    assignment.bind(3, domain);
    assignment.bind(4, difficulty);
    assignment.bind(5, impl_->agent_fingerprint(agent_id));
    assignment.bind(6, utc_now());
    assignment.run();
    transaction.commit();
}

TaskRecord AgentReputationStore::record_task_submission(
    const std::string& task_id, const std::string& agent_id,
    const std::string& domain, int difficulty, const std::string& submitted_at) {
    require_text(task_id, "task_id");
    require_text(submitted_at, "submitted_at");
    require_domain(domain);
    if (difficulty < 1 || difficulty > 5) {
        throw std::invalid_argument("difficulty must be from 1 through 5");
    }
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    Statement exists(impl_->db(), "SELECT 1 FROM tasks WHERE task_id=?");
    exists.bind(1, task_id);
    if (exists.step()) {
        throw TaskConflictError("task ID is already scheduled: " + task_id);
    }
    const auto identity = impl_->agent(agent_id);
    if (identity.status != "ready") {
        throw EligibilityError("cannot assign work to Agent in " + identity.status +
                               " status");
    }
    const auto current_performance = impl_->performance(agent_id, std::nullopt);
    if (current_performance.error_rate.has_value() &&
        *current_performance.error_rate > kErrorRateLimit && difficulty > 1) {
        throw EligibilityError(
            "Agent error rate exceeds 10%; only difficulty-1 tasks are allowed");
    }
    const auto digest = impl_->agent_fingerprint(agent_id);
    {
        Statement assignment(impl_->db(), R"sql(
            INSERT INTO assignments(task_id,agent_id,domain,difficulty,fingerprint,assigned_at)
            VALUES(?,?,?,?,?,?)
        )sql");
        assignment.bind(1, task_id);
        assignment.bind(2, agent_id);
        assignment.bind(3, domain);
        assignment.bind(4, difficulty);
        assignment.bind(5, digest);
        assignment.bind(6, submitted_at);
        assignment.run();
    }
    {
        Statement task(impl_->db(),
                       "INSERT INTO tasks(task_id,agent_id,state,submitted_at) "
                       "VALUES(?,?,'queued',?)");
        task.bind(1, task_id);
        task.bind(2, agent_id);
        task.bind(3, submitted_at);
        task.run();
    }
    {
        Statement event(impl_->db(),
                        "INSERT INTO task_events(task_id,from_state,to_state,occurred_at) "
                        "VALUES(?,NULL,'queued',?)");
        event.bind(1, task_id);
        event.bind(2, submitted_at);
        event.run();
    }
    transaction.commit();
    return impl_->task_record(task_id);
}

TaskRecord AgentReputationStore::transition_task(
    const std::string& task_id, const std::string& state,
    const std::string& occurred_at, std::optional<std::string> error_category,
    std::optional<std::uint64_t> prompt_tokens,
    std::optional<std::uint64_t> completion_tokens) {
    static const std::regex safe_category("^[A-Za-z0-9_.-]{1,64}$");
    require_text(task_id, "task_id");
    require_text(occurred_at, "occurred_at");
    if (error_category.has_value() &&
        !std::regex_match(*error_category, safe_category)) {
        throw std::invalid_argument("task error category must be a safe identifier");
    }
    constexpr auto max_sqlite_integer =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if ((prompt_tokens.has_value() && *prompt_tokens > max_sqlite_integer) ||
        (completion_tokens.has_value() &&
         *completion_tokens > max_sqlite_integer)) {
        throw std::invalid_argument("task token usage exceeds SQLite integer range");
    }
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    std::string previous;
    {
        Statement query(impl_->db(), "SELECT state FROM tasks WHERE task_id=?");
        query.bind(1, task_id);
        if (!query.step()) {
            throw std::out_of_range("unknown task: " + task_id);
        }
        previous = query.text(0);
    }
    const bool allowed =
        (previous == "queued" &&
         contains(std::array<std::string, 3>{"running", "failed", "cancelled"}, state)) ||
        (previous == "running" &&
         contains(std::array<std::string, 3>{"completed", "failed", "cancelled"}, state));
    if (!allowed) {
        throw ReputationError("invalid task state transition: " + previous + " -> " +
                              state);
    }
    const auto started = state == "running" ? std::optional<std::string>(occurred_at)
                                             : std::nullopt;
    const auto finished =
        (state == "completed" || state == "failed" || state == "cancelled")
            ? std::optional<std::string>(occurred_at)
            : std::nullopt;
    Statement update(impl_->db(), R"sql(
        UPDATE tasks SET state=?,started_at=COALESCE(?,started_at),
            finished_at=COALESCE(?,finished_at),error_category=?,
            prompt_tokens=COALESCE(?,prompt_tokens),
            completion_tokens=COALESCE(?,completion_tokens) WHERE task_id=?
    )sql");
    update.bind(1, state);
    if (started.has_value()) {
        update.bind(2, *started);
    } else {
        update.bind_null(2);
    }
    if (finished.has_value()) {
        update.bind(3, *finished);
    } else {
        update.bind_null(3);
    }
    if (error_category.has_value()) {
        update.bind(4, *error_category);
    } else {
        update.bind_null(4);
    }
    if (prompt_tokens.has_value()) {
        update.bind(5, static_cast<std::int64_t>(*prompt_tokens));
    } else {
        update.bind_null(5);
    }
    if (completion_tokens.has_value()) {
        update.bind(6, static_cast<std::int64_t>(*completion_tokens));
    } else {
        update.bind_null(6);
    }
    update.bind(7, task_id);
    update.run();
    Statement event(impl_->db(), R"sql(
        INSERT INTO task_events(task_id,from_state,to_state,occurred_at,error_category)
        VALUES(?,?,?,?,?)
    )sql");
    event.bind(1, task_id);
    event.bind(2, previous);
    event.bind(3, state);
    event.bind(4, occurred_at);
    if (error_category.has_value()) {
        event.bind(5, *error_category);
    } else {
        event.bind_null(5);
    }
    event.run();
    transaction.commit();
    return impl_->task_record(task_id);
}

TaskRecord AgentReputationStore::get_task_record(const std::string& task_id) const {
    std::lock_guard lock(impl_->mutex());
    return impl_->task_record(task_id);
}

std::vector<TaskEvent>
AgentReputationStore::task_history(const std::string& task_id) const {
    std::lock_guard lock(impl_->mutex());
    std::vector<TaskEvent> result;
    Statement query(impl_->db(), R"sql(
        SELECT event_id,task_id,from_state,to_state,occurred_at,error_category
        FROM task_events WHERE task_id=? ORDER BY event_id
    )sql");
    query.bind(1, task_id);
    while (query.step()) {
        result.push_back({query.integer(0), query.text(1), query.optional_text(2),
                          query.text(3), query.text(4), query.optional_text(5)});
    }
    if (result.empty()) {
        throw std::out_of_range("unknown task: " + task_id);
    }
    return result;
}

void AgentReputationStore::record_tool_audit(
    const std::string& request_id, const std::string& task_id,
    const std::string& agent_id, const std::string& tool_name,
    const std::string& arguments_sha256, const std::string& decision,
    std::optional<std::string> outcome_category, const std::string& occurred_at) {
    require_text(request_id, "request_id");
    require_text(task_id, "task_id");
    require_text(agent_id, "agent_id");
    require_text(tool_name, "tool_name");
    require_text(occurred_at, "occurred_at");
    static const std::regex digest("^[0-9a-f]{64}$");
    static const std::regex identifier("^[A-Za-z0-9_.-]{1,128}$");
    if (!std::regex_match(arguments_sha256, digest)) {
        throw EvidenceError("tool argument audit must be a lowercase SHA-256 digest");
    }
    if (!std::regex_match(request_id, identifier) ||
        !std::regex_match(tool_name, identifier)) {
        throw std::invalid_argument("tool audit identifiers contain unsupported characters");
    }
    if (!contains(std::array<std::string, 5>{
                      "denied", "approval-required", "started", "executed", "failed"},
                  decision)) {
        throw std::invalid_argument("invalid tool audit decision");
    }
    if (outcome_category.has_value() &&
        !std::regex_match(*outcome_category, identifier)) {
        throw std::invalid_argument("tool audit category must be a safe identifier");
    }
    std::lock_guard lock(impl_->mutex());
    Statement insert(impl_->db(), R"sql(
        INSERT INTO tool_audit(request_id,task_id,agent_id,tool_name,
            arguments_sha256,decision,outcome_category,occurred_at)
        VALUES(?,?,?,?,?,?,?,?)
    )sql");
    insert.bind(1, request_id);
    insert.bind(2, task_id);
    insert.bind(3, agent_id);
    insert.bind(4, tool_name);
    insert.bind(5, arguments_sha256);
    insert.bind(6, decision);
    if (outcome_category.has_value()) {
        insert.bind(7, *outcome_category);
    } else {
        insert.bind_null(7);
    }
    insert.bind(8, occurred_at);
    insert.run();
}

std::vector<ToolAuditRecord>
AgentReputationStore::tool_audit_history(const std::string& task_id) const {
    require_text(task_id, "task_id");
    std::lock_guard lock(impl_->mutex());
    std::vector<ToolAuditRecord> result;
    Statement query(impl_->db(), R"sql(
        SELECT event_id,request_id,task_id,agent_id,tool_name,arguments_sha256,
               decision,outcome_category,occurred_at
        FROM tool_audit WHERE task_id=? ORDER BY event_id
    )sql");
    query.bind(1, task_id);
    while (query.step()) {
        result.push_back({query.integer(0), query.text(1), query.text(2),
                          query.text(3), query.text(4), query.text(5),
                          query.text(6), query.optional_text(7), query.text(8)});
    }
    return result;
}

std::vector<TaskRecord>
AgentReputationStore::recover_incomplete_tasks(const std::string& occurred_at) {
    require_text(occurred_at, "occurred_at");
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    std::vector<std::pair<std::string, std::string>> incomplete;
    {
        Statement query(impl_->db(), R"sql(
            SELECT task_id,state FROM tasks WHERE state IN ('queued','running')
            ORDER BY submitted_at,task_id
        )sql");
        while (query.step()) {
            incomplete.emplace_back(query.text(0), query.text(1));
        }
    }
    for (const auto& [id, state] : incomplete) {
        Statement update(impl_->db(), R"sql(
            UPDATE tasks SET state='interrupted',finished_at=?,
                error_category='process_restart' WHERE task_id=?
        )sql");
        update.bind(1, occurred_at);
        update.bind(2, id);
        update.run();
        Statement event(impl_->db(), R"sql(
            INSERT INTO task_events(task_id,from_state,to_state,occurred_at,error_category)
            VALUES(?,?,'interrupted',?,'process_restart')
        )sql");
        event.bind(1, id);
        event.bind(2, state);
        event.bind(3, occurred_at);
        event.run();

        Statement reservation(impl_->db(), R"sql(
            SELECT agent_id,reserved_microusd FROM task_cost_reservations
            WHERE task_id=? AND state='reserved'
        )sql");
        reservation.bind(1, id);
        if (reservation.step()) {
            const auto agent_id = reservation.text(0);
            const auto reserved = reservation.integer(1);
            Statement budget(impl_->db(), R"sql(
                UPDATE agent_cost_budgets
                SET spent_microusd=spent_microusd+? WHERE agent_id=?
            )sql");
            budget.bind(1, reserved);
            budget.bind(2, agent_id);
            budget.run();
            if (sqlite3_changes(impl_->db()) != 1) {
                throw ReputationError(
                    "Agent cost budget disappeared during reservation recovery");
            }
            Statement settle(impl_->db(), R"sql(
                UPDATE task_cost_reservations
                SET actual_microusd=reserved_microusd,state='settled',updated_at=?
                WHERE task_id=? AND state='reserved'
            )sql");
            settle.bind(1, occurred_at);
            settle.bind(2, id);
            settle.run();
            Statement task_cost(impl_->db(),
                                "UPDATE tasks SET cost_microusd=? WHERE task_id=?");
            task_cost.bind(1, reserved);
            task_cost.bind(2, id);
            task_cost.run();
        }
    }
    transaction.commit();
    std::vector<TaskRecord> result;
    for (const auto& [id, _] : incomplete) {
        result.push_back(impl_->task_record(id));
    }
    return result;
}

std::vector<TaskRecord> AgentReputationStore::list_task_records(
    std::optional<std::string> agent_id) const {
    std::lock_guard lock(impl_->mutex());
    const char* query = agent_id.has_value()
                            ? "SELECT task_id,agent_id,state,submitted_at,started_at,"
                              "finished_at,error_category,prompt_tokens,"
                              "completion_tokens,cost_microusd FROM tasks WHERE agent_id=? "
                              "ORDER BY submitted_at,task_id"
                            : "SELECT task_id,agent_id,state,submitted_at,started_at,"
                              "finished_at,error_category,prompt_tokens,"
                              "completion_tokens,cost_microusd FROM tasks ORDER BY "
                              "submitted_at,task_id";
    Statement statement(impl_->db(), query);
    if (agent_id.has_value()) {
        statement.bind(1, *agent_id);
    }
    std::vector<TaskRecord> result;
    const auto token_count = [&](int column)
        -> std::optional<std::uint64_t> {
        const auto stored = statement.optional_integer(column);
        if (!stored.has_value()) {
            return std::nullopt;
        }
        if (*stored < 0) {
            throw ReputationError("stored token usage is negative");
        }
        return static_cast<std::uint64_t>(*stored);
    };
    const auto cost_amount = [&]() -> std::optional<double> {
        const auto stored = statement.optional_integer(9);
        if (!stored.has_value()) {
            return std::nullopt;
        }
        return from_microusd(*stored);
    };
    while (statement.step()) {
        result.push_back({statement.text(0), statement.text(1), statement.text(2),
                          statement.text(3), statement.optional_text(4),
                          statement.optional_text(5), statement.optional_text(6),
                          token_count(7), token_count(8), cost_amount()});
    }
    return result;
}

void AgentReputationStore::set_agent_cost_budget(const std::string& agent_id,
                                                 double limit_usd) {
    const auto limit = to_microusd(limit_usd);
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    static_cast<void>(impl_->agent(agent_id));
    Statement current(impl_->db(), R"sql(
        SELECT COALESCE(b.spent_microusd,0) +
               COALESCE((SELECT SUM(r.reserved_microusd)
                         FROM task_cost_reservations r
                         WHERE r.agent_id=? AND r.state='reserved'),0)
        FROM agent_cost_budgets b WHERE b.agent_id=?
    )sql");
    current.bind(1, agent_id);
    current.bind(2, agent_id);
    std::int64_t committed = 0;
    if (current.step()) committed = current.integer(0);
    if (limit < committed) {
        throw CostBudgetExceededError(
            "new cost budget is below spent and reserved usage");
    }
    Statement upsert(impl_->db(), R"sql(
        INSERT INTO agent_cost_budgets(agent_id,limit_microusd,spent_microusd)
        VALUES(?,?,0)
        ON CONFLICT(agent_id) DO UPDATE SET limit_microusd=excluded.limit_microusd
    )sql");
    upsert.bind(1, agent_id);
    upsert.bind(2, limit);
    upsert.run();
    transaction.commit();
}

bool AgentReputationStore::reserve_task_cost(const std::string& task_id,
                                             const std::string& agent_id,
                                             double maximum_cost_usd) {
    const auto reserved = to_microusd(maximum_cost_usd);
    require_text(task_id, "task_id");
    require_text(agent_id, "agent_id");
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    Statement task(impl_->db(),
                   "SELECT agent_id,state FROM tasks WHERE task_id=?");
    task.bind(1, task_id);
    if (!task.step() || task.text(0) != agent_id ||
        (task.text(1) != "queued" && task.text(1) != "running")) {
        throw ReputationError("cost reservation task/Agent identity mismatch");
    }
    Statement budget(impl_->db(), R"sql(
        SELECT limit_microusd,spent_microusd FROM agent_cost_budgets
        WHERE agent_id=?
    )sql");
    budget.bind(1, agent_id);
    if (!budget.step()) {
        transaction.commit();
        return false;
    }
    Statement existing(impl_->db(),
                       "SELECT 1 FROM task_cost_reservations WHERE task_id=?");
    existing.bind(1, task_id);
    if (existing.step()) {
        throw ReputationError("task already has a cost reservation");
    }
    const auto limit = budget.integer(0);
    const auto spent = budget.integer(1);
    Statement outstanding(impl_->db(), R"sql(
        SELECT COALESCE(SUM(reserved_microusd),0)
        FROM task_cost_reservations
        WHERE agent_id=? AND state='reserved'
    )sql");
    outstanding.bind(1, agent_id);
    if (!outstanding.step()) {
        throw ReputationError("cannot read outstanding cost reservations");
    }
    const auto pending = outstanding.integer(0);
    if (reserved > limit || spent > limit - reserved ||
        pending > limit - spent - reserved) {
        throw CostBudgetExceededError("Agent cost budget would be exceeded");
    }
    Statement insert(impl_->db(), R"sql(
        INSERT INTO task_cost_reservations(
            task_id,agent_id,reserved_microusd,state,updated_at)
        VALUES(?,?,?,'reserved',?)
    )sql");
    insert.bind(1, task_id);
    insert.bind(2, agent_id);
    insert.bind(3, reserved);
    insert.bind(4, utc_now());
    insert.run();
    transaction.commit();
    return true;
}

void AgentReputationStore::settle_task_cost(const std::string& task_id,
                                           double actual_cost_usd) {
    const auto actual = to_microusd(actual_cost_usd);
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    Statement reservation(impl_->db(), R"sql(
        SELECT agent_id FROM task_cost_reservations
        WHERE task_id=? AND state='reserved'
    )sql");
    reservation.bind(1, task_id);
    if (!reservation.step()) {
        throw ReputationError("task has no active cost reservation");
    }
    const auto agent_id = reservation.text(0);
    Statement update_budget(impl_->db(), R"sql(
        UPDATE agent_cost_budgets
        SET spent_microusd=spent_microusd+? WHERE agent_id=?
    )sql");
    update_budget.bind(1, actual);
    update_budget.bind(2, agent_id);
    update_budget.run();
    if (sqlite3_changes(impl_->db()) != 1) {
        throw ReputationError("Agent cost budget disappeared during settlement");
    }
    Statement update_reservation(impl_->db(), R"sql(
        UPDATE task_cost_reservations
        SET actual_microusd=?,state='settled',updated_at=? WHERE task_id=?
    )sql");
    update_reservation.bind(1, actual);
    update_reservation.bind(2, utc_now());
    update_reservation.bind(3, task_id);
    update_reservation.run();
    Statement update_task(impl_->db(),
                          "UPDATE tasks SET cost_microusd=? WHERE task_id=?");
    update_task.bind(1, actual);
    update_task.bind(2, task_id);
    update_task.run();
    transaction.commit();
}

void AgentReputationStore::release_task_cost(const std::string& task_id) {
    require_text(task_id, "task_id");
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    Statement update(impl_->db(), R"sql(
        UPDATE task_cost_reservations
        SET state='released',updated_at=?
        WHERE task_id=? AND state='reserved'
    )sql");
    update.bind(1, utc_now());
    update.bind(2, task_id);
    update.run();
    if (sqlite3_changes(impl_->db()) != 1) {
        throw ReputationError("task has no active cost reservation");
    }
    transaction.commit();
}

std::optional<CostBudgetSnapshot>
AgentReputationStore::cost_budget(const std::string& agent_id) const {
    std::lock_guard lock(impl_->mutex());
    Statement query(impl_->db(), R"sql(
        SELECT b.limit_microusd,b.spent_microusd,
               COALESCE((SELECT SUM(r.reserved_microusd)
                         FROM task_cost_reservations r
                         WHERE r.agent_id=b.agent_id AND r.state='reserved'),0)
        FROM agent_cost_budgets b WHERE b.agent_id=?
    )sql");
    query.bind(1, agent_id);
    if (!query.step()) {
        return std::nullopt;
    }
    return CostBudgetSnapshot{agent_id, from_microusd(query.integer(0)),
                              from_microusd(query.integer(1)),
                              from_microusd(query.integer(2))};
}

void AgentReputationStore::record_review(const std::string& task_id,
                                         const std::string& agent_id,
                                         const std::string& evaluator_id,
                                         const std::string& verdict,
                                         const std::string& severity,
                                         const std::string& evidence_sha256) {
    require_text(task_id, "task_id");
    require_text(evaluator_id, "evaluator_id");
    static const std::regex digest("^[0-9a-f]{64}$");
    if (evaluator_id == agent_id) {
        throw EvidenceError("an Agent cannot evaluate its own task");
    }
    if (!contains(std::array<std::string, 4>{
                      "correct", "agent-error", "external-failure", "undetermined"},
                  verdict) ||
        !contains(std::array<std::string, 3>{
                      "normal", "major", "critical-security"},
                  severity)) {
        throw std::invalid_argument("invalid review verdict or severity");
    }
    if (!std::regex_match(evidence_sha256, digest)) {
        throw EvidenceError("evidence must be a lowercase SHA-256 digest");
    }
    if (severity == "critical-security" && verdict != "agent-error") {
        throw std::invalid_argument(
            "critical security findings must be attributed as Agent errors");
    }
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    std::string domain;
    int difficulty = 0;
    std::string digest_version;
    {
        Statement assignment(impl_->db(), R"sql(
            SELECT a.domain,a.difficulty,g.fingerprint FROM assignments a
            JOIN agents g ON g.agent_id=a.agent_id
            WHERE a.task_id=? AND a.agent_id=? AND a.fingerprint=g.fingerprint
        )sql");
        assignment.bind(1, task_id);
        assignment.bind(2, agent_id);
        if (!assignment.step()) {
            throw ReputationError("review requires a current-version task assignment");
        }
        domain = assignment.text(0);
        difficulty = static_cast<int>(assignment.integer(1));
        digest_version = assignment.text(2);
    }
    Statement insert(impl_->db(), R"sql(
        INSERT INTO reviews(task_id,agent_id,domain,difficulty,fingerprint,verdict,
            severity,evaluator_id,evidence_sha256,reviewed_at)
        VALUES(?,?,?,?,?,?,?,?,?,?)
    )sql");
    insert.bind(1, task_id);
    insert.bind(2, agent_id);
    insert.bind(3, domain);
    insert.bind(4, difficulty);
    insert.bind(5, digest_version);
    insert.bind(6, verdict);
    insert.bind(7, severity);
    insert.bind(8, evaluator_id);
    insert.bind(9, evidence_sha256);
    insert.bind(10, utc_now());
    insert.run();
    if (severity == "critical-security") {
        Statement suspend(impl_->db(),
                          "UPDATE agents SET status='suspended',updated_at=? "
                          "WHERE agent_id=?");
        suspend.bind(1, utc_now());
        suspend.bind(2, agent_id);
        suspend.run();
    }
    revoke_ineligible_roles(
        agent_id, "new performance review invalidated governance eligibility");
    transaction.commit();
}

void AgentReputationStore::resolve_security_incident(const std::string& task_id,
                                                      const std::string& agent_id) {
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    Statement update(impl_->db(), R"sql(
        UPDATE reviews SET resolved_at=? WHERE task_id=? AND agent_id=?
            AND severity='critical-security' AND resolved_at IS NULL
    )sql");
    update.bind(1, utc_now());
    update.bind(2, task_id);
    update.bind(3, agent_id);
    update.run();
    if (sqlite3_changes(impl_->db()) == 0) {
        throw std::out_of_range("no unresolved critical incident matches this task and Agent");
    }
    transaction.commit();
}

Performance AgentReputationStore::performance(
    const std::string& agent_id, std::optional<std::string> domain) const {
    std::lock_guard lock(impl_->mutex());
    return impl_->performance(agent_id, std::move(domain));
}

LeaderElection AgentReputationStore::elect_domain_leader(const std::string& domain) {
    require_domain(domain);
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    struct Candidate {
        std::string id;
        std::string provider;
        double score;
        std::int64_t opportunities;
        std::int64_t appointments;
    };
    std::vector<Candidate> qualified;
    Statement ids(impl_->db(), R"sql(
        SELECT agent_id FROM agents WHERE allow_leader=1 AND status='ready'
          AND NOT EXISTS(SELECT 1 FROM active_roles r
                         WHERE r.role='inspector' AND r.agent_id=agents.agent_id)
        ORDER BY agent_id
    )sql");
    while (ids.step()) {
        const auto id = ids.text(0);
        const auto score = impl_->performance(id, domain);
        if (!score.eligible_for_governance || !score.weighted_score.has_value()) {
            continue;
        }
        const auto identity = impl_->agent(id);
        Statement opportunities(impl_->db(), R"sql(
            SELECT COUNT(*) FROM assignments WHERE agent_id=? AND domain=?
              AND fingerprint=(SELECT fingerprint FROM agents WHERE agent_id=?)
        )sql");
        opportunities.bind(1, id);
        opportunities.bind(2, domain);
        opportunities.bind(3, id);
        if (!opportunities.step()) {
            throw ReputationError("task-opportunity count query returned no result");
        }
        Statement appointments(impl_->db(), R"sql(
            SELECT COUNT(*) FROM elections
            WHERE role='domain-leader' AND domain=? AND selected_agent_id=?
        )sql");
        appointments.bind(1, domain);
        appointments.bind(2, id);
        if (!appointments.step()) {
            throw ReputationError("appointment count query returned no result");
        }
        qualified.push_back({id, identity.provider, *score.weighted_score,
                             opportunities.integer(0), appointments.integer(0)});
    }
    std::optional<std::string> selected;
    std::vector<std::string> challengers;
    std::string rationale;
    if (!qualified.empty()) {
        const auto highest = std::max_element(
            qualified.begin(), qualified.end(),
            [](const Candidate& left, const Candidate& right) {
                return left.score < right.score;
            })->score;
        std::vector<Candidate> close;
        std::copy_if(qualified.begin(), qualified.end(), std::back_inserter(close),
                     [highest](const Candidate& candidate) {
                         return highest - candidate.score <= 0.05 + 1e-12;
                     });
        std::sort(close.begin(), close.end(), [](const Candidate& left,
                                                 const Candidate& right) {
            return std::tie(left.opportunities, left.appointments) <
                       std::tie(right.opportunities, right.appointments) ||
                   (std::tie(left.opportunities, left.appointments) ==
                        std::tie(right.opportunities, right.appointments) &&
                    (left.score > right.score ||
                     (left.score == right.score && left.id < right.id)));
        });
        selected = close.front().id;
        std::sort(qualified.begin(), qualified.end(),
                  [](const Candidate& left, const Candidate& right) {
                      return left.score > right.score ||
                             (left.score == right.score && left.id < right.id);
                  });
        const auto elected = std::find_if(
            qualified.begin(), qualified.end(),
            [&](const Candidate& candidate) { return candidate.id == *selected; });
        for (const auto& candidate : qualified) {
            if (candidate.id != *selected && elected != qualified.end() &&
                candidate.provider != elected->provider) {
                challengers.push_back(candidate.id);
            }
        }
        for (const auto& candidate : qualified) {
            if (candidate.id != *selected &&
                std::find(challengers.begin(), challengers.end(), candidate.id) ==
                    challengers.end()) {
                challengers.push_back(candidate.id);
            }
        }
        if (challengers.size() > 2) {
            challengers.resize(2);
        }
        rationale = "selected among eligible Agents by weighted domain score and opportunity/appointment rotation";
    } else {
        rationale = "no Agent met the current-version sample, status, incident, error-rate, and role gates";
    }
    {
        Statement remove(impl_->db(),
                         "DELETE FROM active_roles WHERE role='domain-leader' AND domain=?");
        remove.bind(1, domain);
        remove.run();
    }
    Statement election(impl_->db(), R"sql(
        INSERT INTO elections(role,domain,selected_agent_id,challengers_json,rationale,elected_at)
        VALUES('domain-leader',?,?,?,?,?)
    )sql");
    election.bind(1, domain);
    if (selected.has_value()) {
        election.bind(2, *selected);
    } else {
        election.bind_null(2);
    }
    nlohmann::json challenger_json = challengers;
    election.bind(3, challenger_json.dump());
    election.bind(4, rationale);
    election.bind(5, utc_now());
    election.run();
    const auto election_id =
        static_cast<std::int64_t>(sqlite3_last_insert_rowid(impl_->db()));
    if (selected.has_value()) {
        Statement role(impl_->db(), R"sql(
            INSERT INTO active_roles(role,domain,agent_id,election_id)
            VALUES('domain-leader',?,?,?)
        )sql");
        role.bind(1, domain);
        role.bind(2, *selected);
        role.bind(3, election_id);
        role.run();
        std::vector<std::string> inspector_domains;
        Statement existing_inspectors(
            impl_->db(),
            "SELECT domain FROM active_roles WHERE role='inspector' AND agent_id=?");
        existing_inspectors.bind(1, *selected);
        while (existing_inspectors.step()) {
            inspector_domains.push_back(existing_inspectors.text(0));
        }
        for (const auto& inspector_domain : inspector_domains) {
            remove_role("inspector", inspector_domain, *selected,
                        "Agent was appointed Domain Leader");
        }
    }
    transaction.commit();
    return {domain, selected, challengers, rationale};
}

std::optional<std::string>
AgentReputationStore::current_domain_leader(const std::string& domain) {
    require_domain(domain);
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    std::optional<std::string> id;
    {
        Statement query(impl_->db(),
                        "SELECT agent_id FROM active_roles "
                        "WHERE role='domain-leader' AND domain=?");
        query.bind(1, domain);
        if (query.step()) {
            id = query.text(0);
        }
    }
    if (!id.has_value()) {
        transaction.commit();
        return std::nullopt;
    }
    revoke_ineligible_roles(*id, "leader qualification no longer passes");
    std::optional<std::string> current_id;
    {
        Statement current(impl_->db(),
                          "SELECT agent_id FROM active_roles "
                          "WHERE role='domain-leader' AND domain=?");
        current.bind(1, domain);
        if (current.step()) {
            current_id = current.text(0);
        }
    }
    transaction.commit();
    return current_id;
}

void AgentReputationStore::record_inspection(
    const std::string& task_id, const std::string& inspector_agent_id,
    const std::string& inspected_agent_id, const std::string& classification,
    const std::string& evaluator_id, const std::string& evidence_sha256) {
    if (inspector_agent_id == inspected_agent_id ||
        evaluator_id == inspector_agent_id || evaluator_id == inspected_agent_id) {
        throw EvidenceError("inspection requires an independent Inspector and evaluator");
    }
    if (!contains(std::array<std::string, 4>{
                      "true-positive", "false-positive", "true-negative",
                      "false-negative"},
                  classification)) {
        throw std::invalid_argument("invalid inspection classification");
    }
    static const std::regex digest("^[0-9a-f]{64}$");
    if (!std::regex_match(evidence_sha256, digest)) {
        throw EvidenceError("evidence must be a lowercase SHA-256 digest");
    }
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    const auto inspector = impl_->agent(inspector_agent_id);
    if (!inspector.allow_inspector || inspector.status != "ready") {
        throw EligibilityError("Agent is not eligible for Inspector work");
    }
    Statement leader(impl_->db(),
                     "SELECT 1 FROM active_roles WHERE role='domain-leader' "
                     "AND agent_id=? LIMIT 1");
    leader.bind(1, inspector_agent_id);
    if (leader.step()) {
        throw EligibilityError("Leader and Inspector roles are mutually exclusive");
    }
    {
        Statement inspected(impl_->db(), "SELECT 1 FROM agents WHERE agent_id=?");
        inspected.bind(1, inspected_agent_id);
        if (!inspected.step()) {
            throw std::out_of_range("unknown inspected Agent: " + inspected_agent_id);
        }
    }
    std::string domain;
    int difficulty = 0;
    std::string model_digest;
    {
        Statement assignment(impl_->db(), R"sql(
            SELECT domain,difficulty,fingerprint FROM assignments
            WHERE task_id=? AND agent_id=? AND fingerprint=?
        )sql");
        assignment.bind(1, task_id);
        assignment.bind(2, inspector_agent_id);
        assignment.bind(3, impl_->agent_fingerprint(inspector_agent_id));
        if (!assignment.step()) {
            throw ReputationError("inspection requires a current-version assignment");
        }
        domain = assignment.text(0);
        difficulty = static_cast<int>(assignment.integer(1));
        model_digest = assignment.text(2);
    }
    Statement insert(impl_->db(), R"sql(
        INSERT INTO inspections(task_id,inspector_agent_id,inspected_agent_id,domain,
            difficulty,fingerprint,classification,evaluator_id,evidence_sha256,reviewed_at)
        VALUES(?,?,?,?,?,?,?,?,?,?)
    )sql");
    insert.bind(1, task_id);
    insert.bind(2, inspector_agent_id);
    insert.bind(3, inspected_agent_id);
    insert.bind(4, domain);
    insert.bind(5, difficulty);
    insert.bind(6, model_digest);
    insert.bind(7, classification);
    insert.bind(8, evaluator_id);
    insert.bind(9, evidence_sha256);
    insert.bind(10, utc_now());
    insert.run();
    revoke_ineligible_roles(
        inspector_agent_id, "new inspection review invalidated Inspector eligibility");
    transaction.commit();
}

InspectorPerformance AgentReputationStore::inspector_performance(
    const std::string& agent_id) const {
    std::lock_guard lock(impl_->mutex());
    const auto identity = impl_->agent(agent_id);
    const auto task_performance = impl_->performance(agent_id, std::nullopt);
    std::size_t tp = 0, fp = 0, tn = 0, fn = 0;
    {
        Statement query(impl_->db(), R"sql(
            SELECT classification FROM inspections
            WHERE inspector_agent_id=? AND fingerprint=?
            ORDER BY rowid DESC LIMIT 100
        )sql");
        query.bind(1, agent_id);
        query.bind(2, impl_->agent_fingerprint(agent_id));
        while (query.step()) {
            const auto classification = query.text(0);
            tp += classification == "true-positive";
            fp += classification == "false-positive";
            tn += classification == "true-negative";
            fn += classification == "false-negative";
        }
    }
    const auto evaluated = tp + fp + tn + fn;
    const auto errors = fp + fn;
    InspectorPerformance result{agent_id, evaluated, errors, std::nullopt,
                                std::nullopt, std::nullopt, std::nullopt, false,
                                {}};
    if (evaluated > 0) {
        result.error_rate = static_cast<double>(errors) / evaluated;
    }
    if (tp + fp > 0) {
        result.precision = static_cast<double>(tp) / (tp + fp);
    }
    if (tp + fn > 0) {
        result.recall = static_cast<double>(tp) / (tp + fn);
    }
    if (tn + fp > 0) {
        result.specificity = static_cast<double>(tn) / (tn + fp);
    }
    Statement leader(impl_->db(),
                     "SELECT 1 FROM active_roles WHERE role='domain-leader' "
                     "AND agent_id=? LIMIT 1");
    leader.bind(1, agent_id);
    const bool is_leader = leader.step();
    if (!identity.allow_inspector) {
        result.reason = "Inspector permission is disabled";
    } else if (identity.status != "ready") {
        result.reason = "Agent status is " + identity.status;
    } else if (!task_performance.eligible_for_governance) {
        result.reason = "global Agent performance gate failed";
    } else if (impl_->unresolved_incident(agent_id)) {
        result.reason = "unresolved critical security incident";
    } else if (is_leader) {
        result.reason = "Leader and Inspector roles are mutually exclusive";
    } else if (evaluated < kMinimumEvaluatedTasks) {
        result.reason = "requires 20 current-version reviewed inspections";
    } else if (tp == 0 || tn == 0) {
        result.reason = "requires correct finding and non-finding evidence";
    } else if (result.error_rate.has_value() &&
               *result.error_rate > kErrorRateLimit) {
        result.reason = "inspection error rate exceeds 10%";
    } else {
        result.eligible = true;
        result.reason = "meets Inspector sample and quality gates";
    }
    return result;
}

InspectorRotation AgentReputationStore::rotate_inspector(const std::string& domain) {
    require_domain(domain);
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    std::optional<std::string> leader_provider;
    {
        Statement leader(impl_->db(), R"sql(
            SELECT a.provider FROM active_roles r JOIN agents a ON a.agent_id=r.agent_id
            WHERE r.role='domain-leader' AND r.domain=?
        )sql");
        leader.bind(1, domain);
        if (leader.step()) {
            leader_provider = leader.text(0);
        }
    }
    std::vector<std::tuple<std::string, std::string, double, std::int64_t>> candidates;
    Statement ids(impl_->db(), R"sql(
        SELECT agent_id FROM agents WHERE allow_inspector=1 AND status='ready'
          AND NOT EXISTS(SELECT 1 FROM active_roles WHERE role='inspector'
                         AND active_roles.agent_id=agents.agent_id)
        ORDER BY agent_id
    )sql");
    while (ids.step()) {
        const auto id = ids.text(0);
        const auto score = inspector_performance(id);
        if (!score.eligible) {
            continue;
        }
        const auto identity = impl_->agent(id);
        const auto accuracy = (score.precision.value_or(0.0) +
                               score.recall.value_or(0.0) +
                               score.specificity.value_or(0.0)) /
                              3.0;
        Statement served(impl_->db(),
                         "SELECT COUNT(*) FROM elections WHERE role='inspector' "
                         "AND selected_agent_id=?");
        served.bind(1, id);
        if (!served.step()) {
            throw ReputationError(
                "Inspector appointment count query returned no result");
        }
        candidates.emplace_back(id, identity.provider, accuracy, served.integer(0));
    }
    if (leader_provider.has_value() &&
        std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
            return std::get<1>(candidate) != *leader_provider;
        })) {
        std::erase_if(candidates, [&](const auto& candidate) {
            return std::get<1>(candidate) == *leader_provider;
        });
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return std::get<3>(left) < std::get<3>(right) ||
               (std::get<3>(left) == std::get<3>(right) &&
                (std::get<2>(left) > std::get<2>(right) ||
                 (std::get<2>(left) == std::get<2>(right) &&
                  std::get<0>(left) < std::get<0>(right))));
    });
    const auto selected = candidates.empty()
                              ? std::optional<std::string>{}
                              : std::optional<std::string>{std::get<0>(candidates.front())};
    const std::string rationale =
        selected.has_value()
            ? "rotated to least-served eligible Inspector; cross-provider independence preferred"
            : "no eligible alternate Inspector; deterministic monitoring must continue";
    std::vector<std::pair<std::string, std::string>> active_inspectors;
    Statement old_inspectors(impl_->db(),
                             "SELECT domain,agent_id FROM active_roles "
                             "WHERE role='inspector'");
    while (old_inspectors.step()) {
        active_inspectors.emplace_back(old_inspectors.text(0),
                                       old_inspectors.text(1));
    }
    for (const auto& [old_domain, old_agent] : active_inspectors) {
        remove_role("inspector", old_domain, old_agent,
                    "Inspector rotated for " + domain);
    }
    Statement election(impl_->db(), R"sql(
        INSERT INTO elections(role,domain,selected_agent_id,challengers_json,rationale,elected_at)
        VALUES('inspector',?,?,'[]',?,?)
    )sql");
    election.bind(1, domain);
    if (selected.has_value()) {
        election.bind(2, *selected);
    } else {
        election.bind_null(2);
    }
    election.bind(3, rationale);
    election.bind(4, utc_now());
    election.run();
    if (selected.has_value()) {
        Statement appoint(impl_->db(), R"sql(
            INSERT INTO active_roles(role,domain,agent_id,election_id)
            VALUES('inspector',?,?,?)
        )sql");
        appoint.bind(1, domain);
        appoint.bind(2, *selected);
        appoint.bind(
            3, static_cast<std::int64_t>(sqlite3_last_insert_rowid(impl_->db())));
        appoint.run();
    }
    transaction.commit();
    return {domain, selected, rationale};
}

std::optional<std::string>
AgentReputationStore::current_inspector(const std::string& domain) {
    require_domain(domain);
    std::lock_guard lock(impl_->mutex());
    Transaction transaction(impl_->db());
    std::optional<std::string> id;
    {
        Statement query(impl_->db(),
                        "SELECT agent_id FROM active_roles "
                        "WHERE role='inspector' AND domain=?");
        query.bind(1, domain);
        if (query.step()) {
            id = query.text(0);
        }
    }
    if (!id.has_value()) {
        transaction.commit();
        return std::nullopt;
    }
    revoke_ineligible_roles(*id, "Inspector qualification no longer passes");
    std::optional<std::string> current_id;
    {
        Statement current(impl_->db(),
                          "SELECT agent_id FROM active_roles "
                          "WHERE role='inspector' AND domain=?");
        current.bind(1, domain);
        if (current.step()) {
            current_id = current.text(0);
        }
    }
    transaction.commit();
    return current_id;
}

int AgentReputationStore::schema_version() const {
    std::lock_guard lock(impl_->mutex());
    Statement query(impl_->db(), "PRAGMA user_version");
    return query.step() ? static_cast<int>(query.integer(0)) : 0;
}

int AgentReputationStore::database_mode() const {
    struct stat info {};
    if (stat(impl_->path().c_str(), &info) != 0) {
        throw ReputationError("cannot stat reputation database");
    }
    return info.st_mode & 0777;
}

SchedulerLease::SchedulerLease(const AgentReputationStore& store) {
    const auto lock_path = store.impl_->path() + ".scheduler.lock";
    descriptor_ = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                       0600);
    if (descriptor_ < 0) {
        throw ReputationError("cannot open scheduler lock");
    }
    struct stat info {};
    if (fstat(descriptor_, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1 || (info.st_mode & 0077) != 0) {
        close(descriptor_);
        descriptor_ = -1;
        throw ReputationError("scheduler lock must be a private regular file");
    }
    if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
        close(descriptor_);
        descriptor_ = -1;
        throw SchedulerAlreadyRunningError(
            "another scheduler already owns this reputation database");
    }
}

SchedulerLease::~SchedulerLease() {
    if (descriptor_ >= 0) {
        flock(descriptor_, LOCK_UN);
        close(descriptor_);
    }
}

} // namespace ailinux::ai
