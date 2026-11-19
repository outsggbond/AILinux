#include "self_state_store.hpp"

#include <openssl/evp.h>

#include <array>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ailinux {
namespace {

constexpr std::string_view kJournalName = "self-state.journal";
constexpr std::string_view kLockName = ".self-state.lock";
constexpr std::string_view kHeader = "AILINUX-SELF-STATE\t1\n";
constexpr std::size_t kMaxJournalSize = 64U * 1024U * 1024U;
constexpr std::string_view kInitialHash = "-";

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~FileDescriptor() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                close(descriptor_);
            }
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

class DirectoryLock {
public:
    explicit DirectoryLock(int directory_fd) {
        const std::string name(kLockName);
        descriptor_ = FileDescriptor(openat(directory_fd, name.c_str(),
                                            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                                            S_IRUSR | S_IWUSR));
        if (descriptor_.get() < 0) {
            throw std::runtime_error(error_message("open state lock"));
        }
        validate_private_file(descriptor_.get(), "state lock");
        if (flock(descriptor_.get(), LOCK_EX) != 0) {
            throw std::runtime_error(error_message("lock state journal"));
        }
    }

private:
    static void validate_private_file(int fd, std::string_view description) {
        struct stat info {};
        if (fstat(fd, &info) != 0) {
            throw std::runtime_error(error_message("inspect " + std::string(description)));
        }
        if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
            (info.st_mode & (S_IRWXG | S_IRWXO)) != 0 || info.st_nlink != 1) {
            throw std::runtime_error(std::string(description) +
                                     " must be a private, singly-linked file owned by this user");
        }
    }

    FileDescriptor descriptor_;

public:
    static void validate(int fd, std::string_view description) {
        validate_private_file(fd, description);
    }

    static std::string error_message(const std::string& action) {
        return action + ": " + std::strerror(errno);
    }
};

std::string hash_text(std::string_view text) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int digest_size = 0;
    if (EVP_Digest(text.data(), text.size(), digest.data(), &digest_size, EVP_sha256(),
                   nullptr) != 1) {
        throw std::runtime_error("SHA-256 digest failed");
    }

    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index) {
        result << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return result.str();
}

template <typename State>
State parse_state(std::string_view value, std::string_view kind);

template <>
BaseState parse_state<BaseState>(std::string_view value, std::string_view kind) {
    for (const auto state : {BaseState::Bootstrap, BaseState::Ready,
                             BaseState::Recovery, BaseState::Failed}) {
        if (to_string(state) == value) {
            return state;
        }
    }
    throw std::runtime_error("unknown " + std::string(kind) + " state in journal");
}

template <>
RealState parse_state<RealState>(std::string_view value, std::string_view kind) {
    for (const auto state : {RealState::Offline, RealState::Booting, RealState::Active,
                             RealState::Degraded, RealState::Recovery, RealState::Failed}) {
        if (to_string(state) == value) {
            return state;
        }
    }
    throw std::runtime_error("unknown " + std::string(kind) + " state in journal");
}

template <>
SuperState parse_state<SuperState>(std::string_view value, std::string_view kind) {
    for (const auto state : {SuperState::Inactive, SuperState::Active,
                             SuperState::Testing, SuperState::Ready, SuperState::Failed}) {
        if (to_string(state) == value) {
            return state;
        }
    }
    throw std::runtime_error("unknown " + std::string(kind) + " state in journal");
}

template <>
DeploymentState parse_state<DeploymentState>(std::string_view value,
                                             std::string_view kind) {
    for (const auto state : {DeploymentState::Idle, DeploymentState::Deploying,
                             DeploymentState::Rollback}) {
        if (to_string(state) == value) {
            return state;
        }
    }
    throw std::runtime_error("unknown " + std::string(kind) + " state in journal");
}

std::vector<std::string_view> split_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const auto separator = line.find('\t', start);
        fields.push_back(line.substr(start, separator == std::string_view::npos
                                               ? separator
                                               : separator - start));
        if (separator == std::string_view::npos) {
            return fields;
        }
        start = separator + 1;
    }
}

std::uint64_t parse_sequence(std::string_view value) {
    std::uint64_t sequence = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), sequence);
    if (error != std::errc {} || end != value.data() + value.size()) {
        throw std::runtime_error("invalid sequence number in state journal");
    }
    return sequence;
}

bool valid_transition(const SelfStateSnapshot& from, const SelfStateSnapshot& to) {
    SelfStateManager manager(from);
    unsigned int changed_axes = 0;
    StateTransitionResult result {false, {}, {}, {}};

    if (from.base != to.base) {
        ++changed_axes;
        result = manager.transition_base(to.base);
    }
    if (from.real != to.real) {
        ++changed_axes;
        result = manager.transition_real(to.real);
    }
    if (from.super != to.super) {
        ++changed_axes;
        result = manager.transition_super(to.super);
    }
    if (from.deployment != to.deployment) {
        ++changed_axes;
        result = manager.transition_deployment(to.deployment);
    }

    return changed_axes == 1 && result.accepted && manager.snapshot() == to;
}

struct JournalEntry {
    std::uint64_t sequence;
    SelfStateSnapshot state;
    std::string previous_hash;
    std::string hash;
};

std::string entry_prefix(std::uint64_t sequence,
                         const SelfStateSnapshot& state,
                         std::string_view previous_hash) {
    return std::to_string(sequence) + "\t" + std::string(to_string(state.base)) + "\t" +
           std::string(to_string(state.real)) + "\t" +
           std::string(to_string(state.super)) + "\t" +
           std::string(to_string(state.deployment)) + "\t" +
           std::string(previous_hash);
}

std::string serialize_entry(std::uint64_t sequence,
                            const SelfStateSnapshot& state,
                            std::string_view previous_hash) {
    const auto prefix = entry_prefix(sequence, state, previous_hash);
    return prefix + "\t" + hash_text(prefix) + "\n";
}

std::vector<JournalEntry> parse_journal(std::string_view contents) {
    if (!contents.starts_with(kHeader) || contents.back() != '\n') {
        throw std::runtime_error("state journal header or final record is incomplete");
    }

    std::vector<JournalEntry> entries;
    auto remaining = contents.substr(kHeader.size());
    std::string expected_previous(kInitialHash);
    std::uint64_t expected_sequence = 0;
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        if (newline == std::string_view::npos || newline == 0) {
            throw std::runtime_error("incomplete or empty state journal record");
        }
        const auto line = remaining.substr(0, newline);
        remaining.remove_prefix(newline + 1);
        const auto fields = split_fields(line);
        if (fields.size() != 7) {
            throw std::runtime_error("invalid field count in state journal record");
        }

        const auto sequence = parse_sequence(fields[0]);
        const auto expected_prefix = entry_prefix(
            sequence,
            SelfStateSnapshot {
                parse_state<BaseState>(fields[1], "Base Self"),
                parse_state<RealState>(fields[2], "Real Self"),
                parse_state<SuperState>(fields[3], "Super Self"),
                parse_state<DeploymentState>(fields[4], "deployment"),
            },
            fields[5]);
        const auto digest = hash_text(expected_prefix);
        if (sequence != expected_sequence || fields[5] != expected_previous ||
            fields[6] != digest) {
            throw std::runtime_error("state journal sequence or hash chain is invalid");
        }

        JournalEntry entry {
            sequence,
            SelfStateSnapshot {
                parse_state<BaseState>(fields[1], "Base Self"),
                parse_state<RealState>(fields[2], "Real Self"),
                parse_state<SuperState>(fields[3], "Super Self"),
                parse_state<DeploymentState>(fields[4], "deployment"),
            },
            std::string(fields[5]),
            std::string(fields[6]),
        };
        if (sequence == 0) {
            if (entry.state != SelfStateSnapshot {} || fields[5] != kInitialHash) {
                throw std::runtime_error("state journal has an invalid initial state");
            }
        } else if (!valid_transition(entries.back().state, entry.state)) {
            throw std::runtime_error("state journal contains an invalid state transition");
        }

        expected_previous = entry.hash;
        ++expected_sequence;
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        throw std::runtime_error("state journal contains no initial state");
    }
    return entries;
}

std::string read_all(int fd) {
    struct stat info {};
    if (fstat(fd, &info) != 0) {
        throw std::runtime_error(DirectoryLock::error_message("inspect state journal"));
    }
    DirectoryLock::validate(fd, "state journal");
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > kMaxJournalSize) {
        throw std::runtime_error("state journal exceeds the maximum supported size");
    }

    std::string contents;
    contents.resize(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = read(fd, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            throw std::runtime_error(DirectoryLock::error_message("read state journal"));
        }
        if (count == 0) {
            throw std::runtime_error("state journal was truncated while reading");
        }
        offset += static_cast<std::size_t>(count);
    }
    return contents;
}

FileDescriptor open_directory(const std::filesystem::path& path) {
    const auto encoded_path = path.string();
    FileDescriptor directory(open(encoded_path.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0) {
        throw std::runtime_error(DirectoryLock::error_message("open state directory"));
    }
    struct stat info {};
    if (fstat(directory.get(), &info) != 0) {
        throw std::runtime_error(DirectoryLock::error_message("inspect state directory"));
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        throw std::runtime_error(
            "state directory must be owned by this user and not group/world writable");
    }
    return directory;
}

std::string read_journal(int directory_fd) {
    const std::string name(kJournalName);
    FileDescriptor journal(openat(directory_fd, name.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (journal.get() < 0) {
        throw std::runtime_error(DirectoryLock::error_message("open state journal"));
    }
    return read_all(journal.get());
}

void write_all(int fd, std::string_view contents) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count =
            write(fd, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error(DirectoryLock::error_message("write state journal"));
        }
        offset += static_cast<std::size_t>(count);
    }
}

void initialize_journal(int directory_fd) {
    const std::string name(kJournalName);
    FileDescriptor journal(openat(directory_fd, name.c_str(),
                                  O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                                  S_IRUSR | S_IWUSR));
    if (journal.get() < 0) {
        throw std::runtime_error(DirectoryLock::error_message("create state journal"));
    }
    if (fchmod(journal.get(), S_IRUSR | S_IWUSR) != 0) {
        throw std::runtime_error(DirectoryLock::error_message("secure initial state journal"));
    }
    write_all(journal.get(), kHeader);
    write_all(journal.get(), serialize_entry(0, SelfStateSnapshot {}, kInitialHash));
    if (fsync(journal.get()) != 0 || fsync(directory_fd) != 0) {
        throw std::runtime_error(DirectoryLock::error_message("sync initial state journal"));
    }
}

std::vector<JournalEntry> load_entries(int directory_fd, bool initialize) {
    const std::string name(kJournalName);
    FileDescriptor journal(openat(directory_fd, name.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (journal.get() < 0 && errno == ENOENT && initialize) {
        initialize_journal(directory_fd);
        return parse_journal(read_journal(directory_fd));
    }
    if (journal.get() < 0) {
        throw std::runtime_error(DirectoryLock::error_message("open state journal"));
    }
    const auto contents = read_all(journal.get());
    return parse_journal(contents);
}

}  // namespace

SelfStateStore::SelfStateStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {
    if (directory_.empty()) {
        throw std::invalid_argument("state directory must not be empty");
    }
}

SelfStateSnapshot SelfStateStore::load() {
    const auto directory = open_directory(directory_);
    DirectoryLock lock(directory.get());
    const auto entries = load_entries(directory.get(), true);
    return entries.back().state;
}

void SelfStateStore::append(const SelfStateSnapshot& expected,
                            const SelfStateSnapshot& next) {
    const auto directory = open_directory(directory_);
    DirectoryLock lock(directory.get());
    const auto entries = load_entries(directory.get(), true);
    const auto& last = entries.back();
    if (last.state != expected) {
        throw std::runtime_error("state journal changed; reload before retrying transition");
    }
    if (!valid_transition(expected, next)) {
        throw std::invalid_argument("refusing to persist an invalid state transition");
    }
    if (last.sequence == UINT64_MAX) {
        throw std::runtime_error("state journal sequence is exhausted");
    }

    const std::string name(kJournalName);
    FileDescriptor journal(openat(directory.get(), name.c_str(),
                                  O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW));
    if (journal.get() < 0) {
        throw std::runtime_error(DirectoryLock::error_message("open state journal for append"));
    }
    DirectoryLock::validate(journal.get(), "state journal");
    const auto record = serialize_entry(last.sequence + 1, next, last.hash);
    write_all(journal.get(), record);
    if (fsync(journal.get()) != 0) {
        throw std::runtime_error(DirectoryLock::error_message("sync state transition"));
    }
}

PersistentSelfStateManager::PersistentSelfStateManager(
    std::filesystem::path directory)
    : store_(std::move(directory)), manager_(store_.load()) {}

SelfStateSnapshot PersistentSelfStateManager::snapshot() const noexcept {
    return manager_.snapshot();
}

StateTransitionResult PersistentSelfStateManager::persist_candidate(
    SelfStateManager candidate,
    StateTransitionResult transition) {
    if (!transition.accepted) {
        return transition;
    }
    try {
        store_.append(manager_.snapshot(), candidate.snapshot());
    } catch (const std::exception& error) {
        transition.accepted = false;
        transition.reason = error.what();
        return transition;
    }
    manager_ = std::move(candidate);
    return transition;
}

StateTransitionResult PersistentSelfStateManager::transition_base(BaseState next) {
    auto candidate = manager_;
    auto transition = candidate.transition_base(next);
    return persist_candidate(std::move(candidate), std::move(transition));
}

StateTransitionResult PersistentSelfStateManager::transition_real(RealState next) {
    auto candidate = manager_;
    auto transition = candidate.transition_real(next);
    return persist_candidate(std::move(candidate), std::move(transition));
}

StateTransitionResult PersistentSelfStateManager::transition_super(SuperState next) {
    auto candidate = manager_;
    auto transition = candidate.transition_super(next);
    return persist_candidate(std::move(candidate), std::move(transition));
}

StateTransitionResult PersistentSelfStateManager::transition_deployment(
    DeploymentState next) {
    auto candidate = manager_;
    auto transition = candidate.transition_deployment(next);
    return persist_candidate(std::move(candidate), std::move(transition));
}

}  // namespace ailinux
