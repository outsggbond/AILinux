#include "self_state_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/ailinux-state-test-XXXXXX";
        char* path = mkdtemp(pattern.data());
        if (path == nullptr) {
            throw std::runtime_error("could not create temporary test directory");
        }
        path_ = path;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

bool throws_store_error(const std::filesystem::path& directory) {
    try {
        ailinux::SelfStateStore store(directory);
        static_cast<void>(store.load());
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void create_private_directory(const std::filesystem::path& directory) {
    std::filesystem::create_directory(directory);
    std::filesystem::permissions(
        directory, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
}

void verify_persistence_and_deployment(const std::filesystem::path& directory) {
    using namespace ailinux;
    PersistentSelfStateManager manager(directory);
    check(manager.snapshot() == SelfStateSnapshot {}, "new journal has safe initial state");
    check(manager.transition_base(BaseState::Ready).accepted,
          "persist BASE_READY");
    check(manager.transition_real(RealState::Booting).accepted,
          "persist REAL_BOOTING");
    check(manager.transition_real(RealState::Active).accepted,
          "persist REAL_ACTIVE");
    check(manager.transition_super(SuperState::Active).accepted,
          "persist SUPER_ACTIVE concurrently with REAL_ACTIVE");
    check(manager.transition_super(SuperState::Testing).accepted,
          "persist SUPER_TESTING");
    check(manager.transition_super(SuperState::Ready).accepted,
          "persist SUPER_READY");
    check(manager.transition_deployment(DeploymentState::Deploying).accepted,
          "persist deployment after validated Super Self");
    check(manager.transition_deployment(DeploymentState::Rollback).accepted,
          "persist failed deployment entering ROLLBACK");
    check(manager.transition_deployment(DeploymentState::Idle).accepted,
          "persist completed rollback");

    PersistentSelfStateManager reopened(directory);
    const auto restored = reopened.snapshot();
    check(restored.base == BaseState::Ready &&
              restored.real == RealState::Active &&
              restored.super == SuperState::Ready &&
              restored.deployment == DeploymentState::Idle,
          "reopen replays the last complete state");

    struct stat info {};
    const auto journal = directory / "self-state.journal";
    check(stat(journal.c_str(), &info) == 0, "journal exists");
    if (stat(journal.c_str(), &info) == 0) {
        check((info.st_mode & 0777) == 0600, "journal has owner-only permissions");
    }
}

void verify_stale_writer_rejected(const std::filesystem::path& directory) {
    using namespace ailinux;
    PersistentSelfStateManager first(directory);
    PersistentSelfStateManager stale(directory);
    check(first.transition_base(BaseState::Ready).accepted,
          "first writer persists its transition");
    const auto rejected = stale.transition_base(BaseState::Recovery);
    check(!rejected.accepted, "stale writer cannot overwrite a newer journal state");
    check(stale.snapshot().base == BaseState::Bootstrap,
          "failed persistence does not mutate in-memory state");
    check(PersistentSelfStateManager(directory).snapshot().base == BaseState::Ready,
          "newest durable state remains intact");
}

void verify_corruption_fails_closed(const std::filesystem::path& directory) {
    using namespace ailinux;
    {
        PersistentSelfStateManager manager(directory);
        check(manager.transition_base(BaseState::Ready).accepted,
              "create journal record before corruption test");
    }

    const auto journal = directory / "self-state.journal";
    std::fstream file(journal, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(-2, std::ios::end);
    char value = 0;
    file.get(value);
    file.seekp(-2, std::ios::end);
    file.put(value == '0' ? '1' : '0');
    file.close();
    check(throws_store_error(directory),
          "hash-chain corruption is surfaced instead of resetting state");
}

void verify_unsafe_paths_rejected(const std::filesystem::path& root) {
    const auto unsafe_directory = root / "unsafe";
    create_private_directory(unsafe_directory);
    if (chmod(unsafe_directory.c_str(), 0777) != 0) {
        throw std::runtime_error("could not set unsafe directory permissions");
    }
    check(throws_store_error(unsafe_directory),
          "group/world-writable state directory is rejected");

    const auto symlink_directory = root / "symlink";
    create_private_directory(symlink_directory);
    const auto external = root / "external-state";
    {
        std::ofstream file(external);
        file << "not a journal\n";
    }
    std::filesystem::create_symlink(external, symlink_directory / "self-state.journal");
    check(throws_store_error(symlink_directory), "symlink journal is rejected");
}

}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        const auto persistent = temporary.path() / "persistent";
        const auto stale = temporary.path() / "stale";
        const auto corrupt = temporary.path() / "corrupt";
        create_private_directory(persistent);
        create_private_directory(stale);
        create_private_directory(corrupt);

        verify_persistence_and_deployment(persistent);
        verify_stale_writer_rejected(stale);
        verify_corruption_fails_closed(corrupt);
        verify_unsafe_paths_rejected(temporary.path());
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected test error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    if (failures != 0) {
        std::cerr << failures << " state-store test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All self-state store tests passed\n";
    return EXIT_SUCCESS;
}
