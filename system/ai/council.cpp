#include "council.hpp"

#include <sqlite3.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <regex>
#include <set>
#include <utility>

namespace ailinux::ai {
namespace {

constexpr int kCouncilSchemaVersion = 1;
constexpr std::size_t kMaxFieldBytes = 64 * 1024;
constexpr std::size_t kMaxSessionBytes = 256 * 1024;
const std::regex kIdentifier("^[A-Za-z0-9_.-]{1,128}$");
const std::regex kDigest("^[0-9a-f]{64}$");

void require_text(const std::string& value, const char* field,
                  std::size_t limit = kMaxFieldBytes) {
    if (value.empty() || value.size() > limit ||
        value.find('\0') != std::string::npos ||
        value.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::invalid_argument(std::string(field) +
                                    " must be non-empty text within its size limit");
    }
}

void require_identifier(const std::string& value, const char* field) {
    if (!std::regex_match(value, kIdentifier)) {
        throw std::invalid_argument(std::string(field) + " is not a safe identifier");
    }
}

void require_timestamp(const std::string& value) {
    require_text(value, "occurred_at", 64);
}

void validate_size(const std::vector<std::string>& values) {
    std::size_t total = 0;
    for (const auto& value : values) {
        if (value.size() > kMaxFieldBytes ||
            total > kMaxSessionBytes - value.size()) {
            throw std::invalid_argument("Council record exceeds its size limit");
        }
        total += value.size();
    }
}

void exec(sqlite3* database, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message =
            error == nullptr ? sqlite3_errmsg(database) : error;
        sqlite3_free(error);
        throw CouncilError(message);
    }
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) !=
            SQLITE_OK) {
            throw CouncilError(sqlite3_errmsg(database_));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.c_str(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK) {
            throw CouncilError(sqlite3_errmsg(database_));
        }
    }
    void bind_null(int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw CouncilError(sqlite3_errmsg(database_));
        }
    }
    [[nodiscard]] bool step() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw CouncilError(sqlite3_errmsg(database_));
    }
    void run() {
        if (step()) throw CouncilError("Council database mutation returned a row");
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

private:
    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* database) : database_(database) {
        exec(database_, "BEGIN IMMEDIATE");
    }
    ~Transaction() {
        if (!committed_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    void commit() {
        exec(database_, "COMMIT");
        committed_ = true;
    }

private:
    sqlite3* database_;
    bool committed_ = false;
};

} // namespace

class CouncilWorkflow::Impl {
public:
    Impl(AgentReputationStore& reputation, std::string path)
        : reputation_(reputation), path_(std::move(path)) {
        const std::filesystem::path database(path_);
        if (!database.is_absolute() ||
            !std::filesystem::is_directory(database.parent_path())) {
            throw std::invalid_argument("Council database path must be absolute");
        }
        struct stat parent {};
        if (stat(database.parent_path().c_str(), &parent) != 0 ||
            parent.st_uid != geteuid() ||
            (parent.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            throw std::runtime_error(
                "Council database directory must be owned by this user and private");
        }
        int descriptor = open(path_.c_str(), O_CREAT | O_EXCL | O_RDWR |
                                                 O_CLOEXEC | O_NOFOLLOW,
                              0600);
        const bool created = descriptor >= 0;
        if (!created && errno == EEXIST) {
            descriptor = open(path_.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        }
        if (descriptor < 0) {
            throw CouncilError("cannot securely open Council database");
        }
        struct stat info {};
        if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_uid != geteuid() || info.st_nlink != 1 ||
            (!created && (info.st_mode & 0077) != 0)) {
            close(descriptor);
            throw CouncilError("Council database file permissions are unsafe");
        }
        if (fchmod(descriptor, 0600) != 0) {
            close(descriptor);
            throw CouncilError("cannot restrict Council database file permissions");
        }
        close(descriptor);
        if (sqlite3_open_v2(path_.c_str(), &database_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message =
                database_ == nullptr ? "cannot open Council database"
                                     : sqlite3_errmsg(database_);
            if (database_ != nullptr) {
                sqlite3_close(database_);
                database_ = nullptr;
            }
            throw CouncilError(message);
        }
        chmod(path_.c_str(), 0600);
        sqlite3_busy_timeout(database_, 10000);
        exec(database_, "PRAGMA foreign_keys=ON");
        exec(database_, "PRAGMA journal_mode=DELETE");
        initialize_schema();
    }

    ~Impl() {
        if (database_ != nullptr) sqlite3_close(database_);
    }

    void initialize_schema() {
        Statement version(database_, "PRAGMA user_version");
        const int current = version.step() ? std::stoi(version.text(0)) : 0;
        if (current == kCouncilSchemaVersion) return;
        if (current != 0) throw CouncilError("unsupported Council schema version");
        Transaction transaction(database_);
        exec(database_, R"sql(
            CREATE TABLE council_sessions(
                session_id TEXT PRIMARY KEY,
                task_id TEXT NOT NULL UNIQUE,
                task_summary TEXT NOT NULL,
                domain TEXT NOT NULL,
                leader_agent_id TEXT NOT NULL,
                inspector_agent_id TEXT NOT NULL,
                phase TEXT NOT NULL CHECK(phase IN
                    ('intake','independent-analysis','cross-review',
                     'leader-decision','execution','inspection','archived')),
                execution_cycle INTEGER NOT NULL DEFAULT 0
                    CHECK(execution_cycle>=0),
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                CHECK(leader_agent_id<>inspector_agent_id));
            CREATE TABLE council_participants(
                session_id TEXT NOT NULL REFERENCES council_sessions(session_id),
                agent_id TEXT NOT NULL,
                role TEXT NOT NULL CHECK(role IN ('leader','member','inspector')),
                PRIMARY KEY(session_id,agent_id));
            CREATE TABLE council_opinions(
                session_id TEXT NOT NULL REFERENCES council_sessions(session_id),
                agent_id TEXT NOT NULL,
                understanding TEXT NOT NULL,
                technical_analysis TEXT NOT NULL,
                recommendation TEXT NOT NULL,
                risks TEXT NOT NULL,
                implementation_steps TEXT NOT NULL,
                impact TEXT NOT NULL,
                tests TEXT NOT NULL,
                rollback_plan TEXT NOT NULL,
                created_at TEXT NOT NULL,
                PRIMARY KEY(session_id,agent_id),
                FOREIGN KEY(session_id,agent_id)
                    REFERENCES council_participants(session_id,agent_id));
            CREATE TABLE council_cross_reviews(
                session_id TEXT NOT NULL REFERENCES council_sessions(session_id),
                reviewer_agent_id TEXT NOT NULL,
                target_agent_id TEXT NOT NULL,
                verdict TEXT NOT NULL CHECK(verdict IN
                    ('support','concern','reject')),
                findings TEXT NOT NULL,
                created_at TEXT NOT NULL,
                PRIMARY KEY(session_id,reviewer_agent_id,target_agent_id),
                CHECK(reviewer_agent_id<>target_agent_id),
                FOREIGN KEY(session_id,reviewer_agent_id)
                    REFERENCES council_participants(session_id,agent_id),
                FOREIGN KEY(session_id,target_agent_id)
                    REFERENCES council_participants(session_id,agent_id));
            CREATE TABLE council_decisions(
                session_id TEXT PRIMARY KEY
                    REFERENCES council_sessions(session_id),
                final_plan TEXT NOT NULL,
                rationale TEXT NOT NULL,
                rejected_alternatives TEXT NOT NULL,
                risk_level TEXT NOT NULL,
                execution_plan TEXT NOT NULL,
                test_criteria TEXT NOT NULL,
                rollback_conditions TEXT NOT NULL,
                created_at TEXT NOT NULL);
            CREATE TABLE council_execution_reports(
                report_id INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id TEXT NOT NULL REFERENCES council_sessions(session_id),
                execution_cycle INTEGER NOT NULL CHECK(execution_cycle>=0),
                agent_id TEXT NOT NULL,
                outcome TEXT NOT NULL CHECK(outcome IN
                    ('completed','failed','blocked')),
                summary TEXT NOT NULL,
                error_category TEXT,
                created_at TEXT NOT NULL,
                FOREIGN KEY(session_id,agent_id)
                    REFERENCES council_participants(session_id,agent_id));
            CREATE TABLE council_inspections(
                inspection_id INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id TEXT NOT NULL REFERENCES council_sessions(session_id),
                execution_cycle INTEGER NOT NULL CHECK(execution_cycle>=0),
                inspector_agent_id TEXT NOT NULL,
                verdict TEXT NOT NULL CHECK(verdict IN
                    ('verified','changes-requested','rejected')),
                report TEXT NOT NULL,
                evidence_sha256 TEXT NOT NULL CHECK(length(evidence_sha256)=64),
                reviewed_at TEXT NOT NULL,
                FOREIGN KEY(session_id,inspector_agent_id)
                    REFERENCES council_participants(session_id,agent_id));
            CREATE TABLE council_phase_events(
                event_id INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id TEXT NOT NULL REFERENCES council_sessions(session_id),
                from_phase TEXT NOT NULL,
                to_phase TEXT NOT NULL,
                actor_agent_id TEXT,
                occurred_at TEXT NOT NULL);
            PRAGMA user_version=1;
        )sql");
        transaction.commit();
    }

    void require_session(const std::string& id) const {
        Statement query(database_,
                        "SELECT 1 FROM council_sessions WHERE session_id=?");
        query.bind(1, id);
        if (!query.step()) throw std::out_of_range("unknown Council session");
    }

    [[nodiscard]] std::string phase(const std::string& id) const {
        Statement query(database_,
                        "SELECT phase FROM council_sessions WHERE session_id=?");
        query.bind(1, id);
        if (!query.step()) throw std::out_of_range("unknown Council session");
        return query.text(0);
    }

    void require_phase(const std::string& id, const std::string& expected) const {
        if (phase(id) != expected) {
            throw CouncilError("Council session is not in the required phase");
        }
    }

    [[nodiscard]] std::string participant_role(const std::string& session_id,
                                               const std::string& agent_id) const {
        Statement query(database_, R"sql(
            SELECT role FROM council_participants
            WHERE session_id=? AND agent_id=?
        )sql");
        query.bind(1, session_id);
        query.bind(2, agent_id);
        if (!query.step()) throw CouncilError("Agent is not a Council participant");
        return query.text(0);
    }

    void require_active_role(const std::string& session_id,
                             const std::string& agent_id,
                             const std::string& role) const {
        Statement query(database_, R"sql(
            SELECT domain,leader_agent_id,inspector_agent_id
            FROM council_sessions WHERE session_id=?
        )sql");
        query.bind(1, session_id);
        if (!query.step()) throw std::out_of_range("unknown Council session");
        const auto domain = query.text(0);
        const auto elected_id =
            role == "leader" ? query.text(1) : query.text(2);
        if (elected_id != agent_id ||
            reputation_.get_agent(agent_id).status != "ready") {
            throw EligibilityError("Council role holder is no longer ready");
        }
        if (role == "leader" &&
            reputation_.current_domain_leader(domain) != agent_id) {
            throw EligibilityError("Council Leader role has been revoked");
        }
        if (role == "inspector" &&
            reputation_.current_inspector(domain) != agent_id) {
            throw EligibilityError("Council Inspector role has been revoked");
        }
    }

    void require_ready_participant(const std::string& agent_id) const {
        if (reputation_.get_agent(agent_id).status != "ready") {
            throw EligibilityError("Council participant is no longer ready");
        }
    }

    void advance(const std::string& session_id, const std::string& expected,
                 const std::string& next, const std::string& actor,
                 const std::string& occurred_at) {
        Statement update(database_, R"sql(
            UPDATE council_sessions SET phase=?,updated_at=?
                ,execution_cycle=execution_cycle+
                    CASE WHEN phase='inspection' AND ?='execution' THEN 1 ELSE 0 END
            WHERE session_id=? AND phase=?
        )sql");
        update.bind(1, next);
        update.bind(2, occurred_at);
        update.bind(3, next);
        update.bind(4, session_id);
        update.bind(5, expected);
        update.run();
        if (sqlite3_changes(database_) != 1) {
            throw CouncilError("Council phase changed concurrently");
        }
        Statement event(database_, R"sql(
            INSERT INTO council_phase_events(
                session_id,from_phase,to_phase,actor_agent_id,occurred_at)
            VALUES(?,?,?,?,?)
        )sql");
        event.bind(1, session_id);
        event.bind(2, expected);
        event.bind(3, next);
        if (actor.empty()) {
            event.bind_null(4);
        } else {
            event.bind(4, actor);
        }
        event.bind(5, occurred_at);
        event.run();
    }

    AgentReputationStore& reputation_;
    std::string path_;
    sqlite3* database_ = nullptr;
    mutable std::mutex mutex_;
};

CouncilWorkflow::CouncilWorkflow(AgentReputationStore& reputation,
                                 std::string database_path)
    : impl_(std::make_unique<Impl>(reputation, std::move(database_path))) {}

CouncilWorkflow::~CouncilWorkflow() = default;

CouncilSession CouncilWorkflow::create_session(
    const std::string& session_id, const std::string& task_id,
    const std::string& task_summary, const std::string& domain,
    const std::string& leader_agent_id, const std::string& inspector_agent_id,
    const std::vector<std::string>& member_agent_ids,
    const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_identifier(task_id, "task_id");
    require_text(task_summary, "task_summary");
    require_text(domain, "domain", 64);
    require_identifier(leader_agent_id, "leader_agent_id");
    require_identifier(inspector_agent_id, "inspector_agent_id");
    require_timestamp(occurred_at);
    if (leader_agent_id == inspector_agent_id || member_agent_ids.empty() ||
        member_agent_ids.size() > 32) {
        throw CouncilError("Council requires distinct roles and at least one member");
    }
    if (impl_->reputation_.current_domain_leader(domain) != leader_agent_id ||
        impl_->reputation_.current_inspector(domain) != inspector_agent_id) {
        throw EligibilityError(
            "Council Leader and Inspector must hold the active domain roles");
    }
    std::set<std::string> unique{leader_agent_id, inspector_agent_id};
    for (const auto& member : member_agent_ids) {
        require_identifier(member, "member_agent_id");
        if (!unique.insert(member).second) {
            throw CouncilError("Council participants must be distinct");
        }
        if (impl_->reputation_.get_agent(member).status != "ready") {
            throw EligibilityError("Council members must be ready Agents");
        }
    }
    if (impl_->reputation_.get_agent(leader_agent_id).status != "ready" ||
        impl_->reputation_.get_agent(inspector_agent_id).status != "ready") {
        throw EligibilityError("Council roles must be held by ready Agents");
    }

    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    Statement insert(impl_->database_, R"sql(
        INSERT INTO council_sessions(session_id,task_id,task_summary,domain,
            leader_agent_id,inspector_agent_id,phase,created_at,updated_at)
        VALUES(?,?,?,?,?,?,'intake',?,?)
    )sql");
    insert.bind(1, session_id);
    insert.bind(2, task_id);
    insert.bind(3, task_summary);
    insert.bind(4, domain);
    insert.bind(5, leader_agent_id);
    insert.bind(6, inspector_agent_id);
    insert.bind(7, occurred_at);
    insert.bind(8, occurred_at);
    insert.run();
    const auto add_participant = [&](const std::string& agent,
                                     const std::string& role) {
        Statement participant(impl_->database_, R"sql(
            INSERT INTO council_participants(session_id,agent_id,role)
            VALUES(?,?,?)
        )sql");
        participant.bind(1, session_id);
        participant.bind(2, agent);
        participant.bind(3, role);
        participant.run();
    };
    add_participant(leader_agent_id, "leader");
    add_participant(inspector_agent_id, "inspector");
    for (const auto& member : member_agent_ids) add_participant(member, "member");
    Statement event(impl_->database_, R"sql(
        INSERT INTO council_phase_events(
            session_id,from_phase,to_phase,actor_agent_id,occurred_at)
        VALUES(?,'','intake',?,?)
    )sql");
    event.bind(1, session_id);
    event.bind(2, leader_agent_id);
    event.bind(3, occurred_at);
    event.run();
    transaction.commit();
    return {session_id, task_id, task_summary, domain, leader_agent_id,
            inspector_agent_id, "intake", occurred_at, occurred_at};
}

CouncilSession CouncilWorkflow::get_session(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    Statement query(impl_->database_, R"sql(
        SELECT session_id,task_id,task_summary,domain,leader_agent_id,
               inspector_agent_id,phase,created_at,updated_at
        FROM council_sessions WHERE session_id=?
    )sql");
    query.bind(1, session_id);
    if (!query.step()) throw std::out_of_range("unknown Council session");
    return {query.text(0), query.text(1), query.text(2), query.text(3),
            query.text(4), query.text(5), query.text(6), query.text(7),
            query.text(8)};
}

std::vector<CouncilParticipant>
CouncilWorkflow::participants(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    impl_->require_session(session_id);
    Statement query(impl_->database_, R"sql(
        SELECT agent_id,role FROM council_participants
        WHERE session_id=? ORDER BY CASE role WHEN 'leader' THEN 0
        WHEN 'member' THEN 1 ELSE 2 END,agent_id
    )sql");
    query.bind(1, session_id);
    std::vector<CouncilParticipant> result;
    while (query.step()) result.push_back({query.text(0), query.text(1)});
    return result;
}

std::vector<CouncilOpinion>
CouncilWorkflow::opinions(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    impl_->require_session(session_id);
    Statement query(impl_->database_, R"sql(
        SELECT understanding,technical_analysis,recommendation,risks,
               implementation_steps,impact,tests,rollback_plan
        FROM council_opinions WHERE session_id=? ORDER BY agent_id
    )sql");
    query.bind(1, session_id);
    std::vector<CouncilOpinion> result;
    while (query.step()) {
        result.push_back({query.text(0), query.text(1), query.text(2),
                          query.text(3), query.text(4), query.text(5),
                          query.text(6), query.text(7)});
    }
    return result;
}

std::vector<CouncilOpinionRecord>
CouncilWorkflow::opinion_records(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    impl_->require_session(session_id);
    Statement query(impl_->database_, R"sql(
        SELECT agent_id,understanding,technical_analysis,recommendation,risks,
               implementation_steps,impact,tests,rollback_plan
        FROM council_opinions WHERE session_id=? ORDER BY agent_id
    )sql");
    query.bind(1, session_id);
    std::vector<CouncilOpinionRecord> result;
    while (query.step()) {
        result.push_back(
            {query.text(0),
             {query.text(1), query.text(2), query.text(3), query.text(4),
              query.text(5), query.text(6), query.text(7), query.text(8)}});
    }
    return result;
}

std::vector<CouncilCrossReview>
CouncilWorkflow::cross_reviews(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    impl_->require_session(session_id);
    Statement query(impl_->database_, R"sql(
        SELECT reviewer_agent_id,target_agent_id,verdict,findings,created_at
        FROM council_cross_reviews WHERE session_id=?
        ORDER BY reviewer_agent_id,target_agent_id
    )sql");
    query.bind(1, session_id);
    std::vector<CouncilCrossReview> result;
    while (query.step()) {
        result.push_back({query.text(0), query.text(1), query.text(2),
                          query.text(3), query.text(4)});
    }
    return result;
}

std::optional<CouncilDecision>
CouncilWorkflow::decision(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    impl_->require_session(session_id);
    Statement query(impl_->database_, R"sql(
        SELECT final_plan,rationale,rejected_alternatives,risk_level,
               execution_plan,test_criteria,rollback_conditions
        FROM council_decisions WHERE session_id=?
    )sql");
    query.bind(1, session_id);
    if (!query.step()) return std::nullopt;
    return CouncilDecision{query.text(0), query.text(1), query.text(2),
                           query.text(3), query.text(4), query.text(5),
                           query.text(6)};
}

std::vector<CouncilExecutionReport>
CouncilWorkflow::execution_reports(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    impl_->require_session(session_id);
    Statement query(impl_->database_, R"sql(
        SELECT execution_cycle,agent_id,outcome,summary,error_category,created_at
        FROM council_execution_reports WHERE session_id=?
        ORDER BY report_id
    )sql");
    query.bind(1, session_id);
    std::vector<CouncilExecutionReport> result;
    while (query.step()) {
        result.push_back({static_cast<std::uint64_t>(query.integer(0)),
                          query.text(1), query.text(2), query.text(3),
                          query.optional_text(4), query.text(5)});
    }
    return result;
}

std::optional<CouncilInspection>
CouncilWorkflow::inspection(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    impl_->require_session(session_id);
    Statement query(impl_->database_, R"sql(
        SELECT execution_cycle,verdict,report,evidence_sha256,reviewed_at
        FROM council_inspections WHERE session_id=?
        ORDER BY inspection_id DESC LIMIT 1
    )sql");
    query.bind(1, session_id);
    if (!query.step()) return std::nullopt;
    return CouncilInspection{static_cast<std::uint64_t>(query.integer(0)),
                             query.text(1), query.text(2), query.text(3),
                             query.text(4)};
}

std::vector<CouncilInspection>
CouncilWorkflow::inspection_history(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    impl_->require_session(session_id);
    Statement query(impl_->database_, R"sql(
        SELECT execution_cycle,verdict,report,evidence_sha256,reviewed_at
        FROM council_inspections WHERE session_id=?
        ORDER BY inspection_id
    )sql");
    query.bind(1, session_id);
    std::vector<CouncilInspection> result;
    while (query.step()) {
        result.push_back({static_cast<std::uint64_t>(query.integer(0)),
                          query.text(1), query.text(2), query.text(3),
                          query.text(4)});
    }
    return result;
}

std::vector<CouncilPhaseEvent>
CouncilWorkflow::phase_history(const std::string& session_id) const {
    require_identifier(session_id, "session_id");
    std::lock_guard lock(impl_->mutex_);
    impl_->require_session(session_id);
    Statement query(impl_->database_, R"sql(
        SELECT event_id,from_phase,to_phase,actor_agent_id,occurred_at
        FROM council_phase_events WHERE session_id=? ORDER BY event_id
    )sql");
    query.bind(1, session_id);
    std::vector<CouncilPhaseEvent> result;
    while (query.step()) {
        result.push_back({query.integer(0), query.text(1), query.text(2),
                          query.optional_text(3), query.text(4)});
    }
    return result;
}

void CouncilWorkflow::begin_analysis(const std::string& session_id,
                                    const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_timestamp(occurred_at);
    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    impl_->advance(session_id, "intake", "independent-analysis", "",
                   occurred_at);
    transaction.commit();
}

void CouncilWorkflow::submit_opinion(const std::string& session_id,
                                     const std::string& agent_id,
                                     const CouncilOpinion& opinion,
                                     const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_identifier(agent_id, "agent_id");
    require_timestamp(occurred_at);
    const std::vector<std::string> fields = {
        opinion.understanding, opinion.technical_analysis,
        opinion.recommendation, opinion.risks, opinion.implementation_steps,
        opinion.impact, opinion.tests, opinion.rollback_plan};
    for (const auto& field : fields) require_text(field, "opinion field");
    validate_size(fields);
    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    impl_->require_phase(session_id, "independent-analysis");
    const auto role = impl_->participant_role(session_id, agent_id);
    if (role == "inspector") {
        throw CouncilError("Inspector cannot participate in independent analysis");
    }
    impl_->require_ready_participant(agent_id);
    Statement insert(impl_->database_, R"sql(
        INSERT INTO council_opinions(session_id,agent_id,understanding,
            technical_analysis,recommendation,risks,implementation_steps,
            impact,tests,rollback_plan,created_at)
        VALUES(?,?,?,?,?,?,?,?,?,?,?)
    )sql");
    insert.bind(1, session_id);
    insert.bind(2, agent_id);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        insert.bind(static_cast<int>(i + 3), fields[i]);
    }
    insert.bind(11, occurred_at);
    insert.run();
    transaction.commit();
}

void CouncilWorkflow::begin_cross_review(const std::string& session_id,
                                         const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_timestamp(occurred_at);
    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    impl_->require_phase(session_id, "independent-analysis");
    Statement missing(impl_->database_, R"sql(
        SELECT COUNT(*) FROM council_participants p
        WHERE p.session_id=? AND p.role IN ('leader','member')
          AND NOT EXISTS(SELECT 1 FROM council_opinions o
              WHERE o.session_id=p.session_id AND o.agent_id=p.agent_id)
    )sql");
    missing.bind(1, session_id);
    if (!missing.step() || missing.integer(0) != 0) {
        throw CouncilError("all non-Inspector participants must submit analysis");
    }
    impl_->advance(session_id, "independent-analysis", "cross-review", "",
                   occurred_at);
    transaction.commit();
}

void CouncilWorkflow::submit_cross_review(
    const std::string& session_id, const std::string& reviewer_agent_id,
    const std::string& target_agent_id, const std::string& verdict,
    const std::string& findings, const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_identifier(reviewer_agent_id, "reviewer_agent_id");
    require_identifier(target_agent_id, "target_agent_id");
    require_text(findings, "findings");
    require_timestamp(occurred_at);
    if (reviewer_agent_id == target_agent_id ||
        !std::regex_match(verdict, std::regex("^(support|concern|reject)$"))) {
        throw CouncilError("cross-review must be independent and use a valid verdict");
    }
    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    impl_->require_phase(session_id, "cross-review");
    if (impl_->participant_role(session_id, reviewer_agent_id) == "inspector" ||
        impl_->participant_role(session_id, target_agent_id) == "inspector") {
        throw CouncilError("Inspector must remain independent of member cross-review");
    }
    impl_->require_ready_participant(reviewer_agent_id);
    impl_->require_ready_participant(target_agent_id);
    Statement insert(impl_->database_, R"sql(
        INSERT INTO council_cross_reviews(session_id,reviewer_agent_id,
            target_agent_id,verdict,findings,created_at)
        VALUES(?,?,?,?,?,?)
    )sql");
    insert.bind(1, session_id);
    insert.bind(2, reviewer_agent_id);
    insert.bind(3, target_agent_id);
    insert.bind(4, verdict);
    insert.bind(5, findings);
    insert.bind(6, occurred_at);
    insert.run();
    transaction.commit();
}

void CouncilWorkflow::begin_leader_decision(const std::string& session_id,
                                            const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_timestamp(occurred_at);
    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    impl_->require_phase(session_id, "cross-review");
    Statement missing(impl_->database_, R"sql(
        SELECT COUNT(*) FROM council_participants p
        WHERE p.session_id=? AND p.role IN ('leader','member')
          AND NOT EXISTS(SELECT 1 FROM council_cross_reviews r
              WHERE r.session_id=p.session_id
                AND r.reviewer_agent_id=p.agent_id)
    )sql");
    missing.bind(1, session_id);
    if (!missing.step() || missing.integer(0) != 0) {
        throw CouncilError("each non-Inspector participant must cross-review a peer");
    }
    impl_->advance(session_id, "cross-review", "leader-decision", "",
                   occurred_at);
    transaction.commit();
}

void CouncilWorkflow::submit_decision(const std::string& session_id,
                                      const std::string& leader_agent_id,
                                      const CouncilDecision& decision_value,
                                      const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_identifier(leader_agent_id, "leader_agent_id");
    require_timestamp(occurred_at);
    const std::vector<std::string> fields = {
        decision_value.final_plan, decision_value.rationale,
        decision_value.rejected_alternatives, decision_value.risk_level,
        decision_value.execution_plan, decision_value.test_criteria,
        decision_value.rollback_conditions};
    for (const auto& field : fields) require_text(field, "decision field");
    validate_size(fields);
    if (!std::regex_match(decision_value.risk_level,
                          std::regex("^(low|moderate|high|critical)$"))) {
        throw CouncilError("decision risk level is invalid");
    }
    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    impl_->require_phase(session_id, "leader-decision");
    if (impl_->participant_role(session_id, leader_agent_id) != "leader") {
        throw CouncilError("only the Council Leader may submit a decision");
    }
    impl_->require_active_role(session_id, leader_agent_id, "leader");
    Statement insert(impl_->database_, R"sql(
        INSERT INTO council_decisions(session_id,final_plan,rationale,
            rejected_alternatives,risk_level,execution_plan,test_criteria,
            rollback_conditions,created_at)
        VALUES(?,?,?,?,?,?,?,?,?)
    )sql");
    insert.bind(1, session_id);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        insert.bind(static_cast<int>(i + 2), fields[i]);
    }
    insert.bind(9, occurred_at);
    insert.run();
    impl_->advance(session_id, "leader-decision", "execution",
                   leader_agent_id, occurred_at);
    transaction.commit();
}

void CouncilWorkflow::submit_execution_report(
    const std::string& session_id, const std::string& agent_id,
    const std::string& outcome, const std::string& summary,
    std::optional<std::string> error_category,
    const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_identifier(agent_id, "agent_id");
    require_text(summary, "execution summary");
    require_timestamp(occurred_at);
    if (!std::regex_match(outcome,
                          std::regex("^(completed|failed|blocked)$"))) {
        throw CouncilError("execution outcome is invalid");
    }
    if (error_category.has_value()) {
        require_identifier(*error_category, "error_category");
    }
    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    impl_->require_phase(session_id, "execution");
    if (impl_->participant_role(session_id, agent_id) == "inspector") {
        throw CouncilError("Inspector cannot execute the selected plan");
    }
    impl_->require_ready_participant(agent_id);
    Statement cycle_query(impl_->database_,
                          "SELECT execution_cycle FROM council_sessions "
                          "WHERE session_id=?");
    cycle_query.bind(1, session_id);
    if (!cycle_query.step()) throw std::out_of_range("unknown Council session");
    Statement insert(impl_->database_, R"sql(
        INSERT INTO council_execution_reports(session_id,execution_cycle,
            agent_id,outcome,summary,error_category,created_at)
        VALUES(?,?,?,?,?,?,?)
    )sql");
    insert.bind(1, session_id);
    insert.bind(2, std::to_string(cycle_query.integer(0)));
    insert.bind(3, agent_id);
    insert.bind(4, outcome);
    insert.bind(5, summary);
    if (error_category.has_value()) insert.bind(6, *error_category);
    else insert.bind_null(6);
    insert.bind(7, occurred_at);
    insert.run();
    transaction.commit();
}

void CouncilWorkflow::begin_inspection(const std::string& session_id,
                                       const std::string& leader_agent_id,
                                       const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_identifier(leader_agent_id, "leader_agent_id");
    require_timestamp(occurred_at);
    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    impl_->require_phase(session_id, "execution");
    if (impl_->participant_role(session_id, leader_agent_id) != "leader") {
        throw CouncilError("only the Council Leader may submit execution for inspection");
    }
    impl_->require_active_role(session_id, leader_agent_id, "leader");
    Statement report(impl_->database_, R"sql(
        SELECT r.outcome FROM council_execution_reports r
        JOIN council_sessions s ON s.session_id=r.session_id
        WHERE r.session_id=? AND r.agent_id=?
          AND r.execution_cycle=s.execution_cycle
        ORDER BY r.report_id DESC LIMIT 1
    )sql");
    report.bind(1, session_id);
    report.bind(2, leader_agent_id);
    if (!report.step()) {
        throw CouncilError("Leader must record an execution report before inspection");
    }
    impl_->advance(session_id, "execution", "inspection", leader_agent_id,
                   occurred_at);
    transaction.commit();
}

void CouncilWorkflow::submit_inspection(
    const std::string& session_id, const std::string& inspector_agent_id,
    const std::string& verdict, const std::string& report,
    const std::string& evidence_sha256, const std::string& occurred_at) {
    require_identifier(session_id, "session_id");
    require_identifier(inspector_agent_id, "inspector_agent_id");
    require_text(report, "inspection report");
    require_timestamp(occurred_at);
    if (!std::regex_match(verdict,
                          std::regex("^(verified|changes-requested|rejected)$")) ||
        !std::regex_match(evidence_sha256, kDigest)) {
        throw CouncilError("inspection verdict or evidence digest is invalid");
    }
    std::lock_guard lock(impl_->mutex_);
    Transaction transaction(impl_->database_);
    impl_->require_phase(session_id, "inspection");
    if (impl_->participant_role(session_id, inspector_agent_id) != "inspector") {
        throw CouncilError("only the independent Inspector may submit inspection");
    }
    impl_->require_active_role(session_id, inspector_agent_id, "inspector");
    Statement insert(impl_->database_, R"sql(
        INSERT INTO council_inspections(session_id,execution_cycle,
            inspector_agent_id,verdict,report,evidence_sha256,reviewed_at)
        SELECT ?,execution_cycle,?,?,?,?,? FROM council_sessions
        WHERE session_id=?
    )sql");
    insert.bind(1, session_id);
    insert.bind(2, inspector_agent_id);
    insert.bind(3, verdict);
    insert.bind(4, report);
    insert.bind(5, evidence_sha256);
    insert.bind(6, occurred_at);
    insert.bind(7, session_id);
    insert.run();
    if (verdict == "changes-requested") {
        impl_->advance(session_id, "inspection", "execution",
                       inspector_agent_id, occurred_at);
    } else {
        impl_->advance(session_id, "inspection", "archived",
                       inspector_agent_id, occurred_at);
    }
    transaction.commit();
}

} // namespace ailinux::ai
