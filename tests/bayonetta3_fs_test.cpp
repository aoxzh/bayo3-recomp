// Standalone Linux runner, no game launch or CMake changes:
// clang++ -std=c++20 -pthread -DNX_BAYONETTA3 -I runtime/include \
//   tests/bayonetta3_fs_test.cpp runtime/src/users.cpp -Wl,--wrap=fsync -o /tmp/bayonetta3_fs_test
// /tmp/bayonetta3_fs_test
#include <nx/runtime.h>
#include <nx/game.h>
#include "../runtime/src/platform.h"
#include <array>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <fcntl.h>
#include <unistd.h>

namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
std::map<std::string, nx::HleFn>& registry() {
    static std::map<std::string, nx::HleFn> r;
    return r;
}
std::string root;
std::set<int> synced;
int failed_sync_fd = -1;
bool fail_seek = false;
template<class T> uint64_t ptr(T* p) { return reinterpret_cast<uint64_t>(p); }
uint64_t invoke(const char* suffix, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0) {
    auto it = registry().find(std::string("_ZN2nn2fs") + suffix);
    check(it != registry().end(), suffix);
    nx::CPU cpu{};
    cpu.x[0] = a; cpu.x[1] = b; cpu.x[2] = c; cpu.x[3] = d; cpu.x[4] = e;
    it->second(cpu);
    return cpu.x[0];
}
struct Output {
    std::array<uint64_t, 3> words{0xa5a5a5a5a5a5a5a5ull, 0xccccccccccccccccull, 0xa5a5a5a5a5a5a5a5ull};
    uint64_t address() { return ptr(&words[1]); }
    void guards() { check(words[0] == 0xa5a5a5a5a5a5a5a5ull && words[2] == words[0], "8-byte output sentinels"); }
};
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    synced.insert(fd);
    if (fd == failed_sync_fd) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
namespace nx {
bool g_trace = false;
HleRegistrar::HleRegistrar(const char* name, HleFn fn) { check(registry().emplace(name, fn).second, "unique registration"); }
void log(const char*, ...) {}
namespace game {
const Info kInfo{"bayonetta3", "Bayonetta 3", 0x01004a4010fea000ull, "test", "unused"};
}
namespace plat {
std::string data_root() { return root; }
int64_t current_thread_id() { return getpid(); }
uint64_t monotonic_ns() { return 0; }
FILE* fopen_utf8(const std::string& p, const char* mode) { return std::fopen(p.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) {
    if (fail_seek) { errno = EIO; return -1; }
    return fseeko(f, off, origin);
}
}
}
// Include the production implementation to inject genuinely buffered data and
// descriptor failures; all guest calls still go through its actual HLE registry.
#include "../runtime/src/hle_fs.cpp"

int main() {
    char temp[] = "/tmp/bayonetta3-fs-XXXXXX";
    char* dir = mkdtemp(temp); check(dir, "temporary root"); root = dir;
    uint64_t uid[] = {1, 0x4E58485553455231ull};
    uint64_t second[] = {2, uid[1]};
    uint64_t invalid[] = {0, 0};
    constexpr auto exists = "18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE";
    auto title = nx::game::kInfo.program_id;
    check(invoke(exists, title, ptr(uid)) == 0, "missing current-title save");
    check(invoke(exists, title, ptr(second)) == 0, "missing secondary save");
    check(invoke(exists, title + 1, ptr(uid)) == 0, "foreign title false");
    check(invoke("18IsSaveDataExistingERKNS_7account3UidE", ptr(uid)) == 0, "legacy query truthful");
    check(fs::is_empty(root), "existence queries create neither directories nor users.txt");
    check(invoke("14EnsureSaveDataERKNS_7account3UidE", ptr(uid)) == 0, "ensure creates save");
    check(invoke(exists, title, ptr(uid)) == 1, "ensured save exists");
    check(invoke(exists, 0, ptr(uid)) == 0, "zero is not current title");
    check(invoke(exists, title, ptr(invalid)) == 0, "invalid uid false");
    check(invoke(exists, title, ptr(second)) == 0, "users isolated");
    check(!fs::exists(fs::path(root) / "users.txt"), "ensure path mapping has no account side effect");
    auto user = nx::user_create();
    nx::user_opened(user.lo, user.hi);
    check(invoke(exists, title, ptr(uid)) == 1, "query existing first user");
    check(nx::user_last_opened().lo == user.lo, "query does not change last opened user");
    auto second_path = fs::path(root) / "user" / nx::user_save_dir(second[0], second[1]);
    std::ofstream(second_path) << "not a save directory";
    check(invoke(exists, title, ptr(second)) == 0, "regular file is not a save");
    check(invoke("14EnsureSaveDataERKNS_7account3UidE", ptr(second)) != 0, "ensure reports directory creation failure");
    fs::remove(second_path);
    check(invoke("14EnsureSaveDataERKNS_7account3UidE", ptr(second)) == 0, "secondary ensure");
    check(invoke(exists, title, ptr(second)) == 1, "secondary save path");
    check(invoke("13MountSaveDataEPKcRKNS_7account3UidE", ptr("save"), ptr(uid)) == 0, "mount save");
    check(invoke("13MountSaveDataEPKcRKNS_7account3UidE", ptr("other"), ptr(second)) == 0, "mount other");
    check(invoke("14CommitSaveDataEPKc", ptr("missing")) == 0x35f202, "SDK missing mount result");
    check(invoke("14CommitSaveDataEPKc", 0) == 0x2f5e02, "SDK null mount result");
    check(invoke("14CommitSaveDataEPKc", ptr("save:")) == 0x35f202, "commit takes bare mount name");
    check(invoke("14CommitSaveDataEPKc", ptr("save")) == 0, "empty mount commit");
    check(invoke("10CreateFileEPKcl", ptr("save:/negative"), uint64_t(-1)) == nx::kInvalidSize, "reject negative create size");
    check(!fs::exists(fs::path(root) / "user/save/negative"), "negative create leaves no file");
    check(invoke("10CreateFileEPKcl", ptr("save:/data"), 8) == 0, "create save file");
    check(invoke("10CreateFileEPKcl", ptr("other:/data"), 8) == 0, "create other file");
    auto open = [](const char* path, int mode) {
        Output out;
        check(invoke("8OpenFileEPNS0_10FileHandleEPKci", out.address(), ptr(path), mode) == 0, "open");
        out.guards(); return out.words[1];
    };
    auto h = open("save:/data", 3), other = open("other:/data", 3), ro = open("save:/data", 1);
    auto file = nx::g_files.at(h), other_file = nx::g_files.at(other);
    int fd = fileno(file->f), other_fd = fileno(other_file->f);
    // Bypass WriteFile's mandatory visibility flush to prove Commit drains libc buffers.
    check(fwrite("persist!", 1, 8, file->f) == 8, "buffered host write");
    synced.clear();
    check(invoke("14CommitSaveDataEPKc", ptr("save")) == 0, "commit buffered data");
    check(synced == std::set<int>{fd}, "commit only syncs this mount's writable handle");
    char bytes[16]{};
    int fresh = ::open((fs::path(root) / "user/save/data").c_str(), O_RDONLY);
    check(fresh >= 0 && pread(fresh, bytes, 8, 0) == 8 && std::string(bytes, 8) == "persist!", "fresh descriptor reads committed bytes");
    close(fresh);
    failed_sync_fd = fd;
    check(invoke("14CommitSaveDataEPKc", ptr("save")) == nx::kHostIoError, "commit propagates fsync EIO");
    check(invoke("9FlushFileENS0_10FileHandleE", h) == nx::kHostIoError, "flush propagates fsync EIO");
    uint32_t option = 1;
    check(invoke("9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", h, 0, ptr("durable!"), 8, ptr(&option)) == nx::kHostIoError, "WriteOption flush is honored");
    failed_sync_fd = -1;
    synced.clear();
    check(invoke("6CommitEPKc", ptr("save")) == 0 && synced.count(fd), "Commit alias durable");
    check(invoke("9FlushFileENS0_10FileHandleE", 0xdead) != 0, "invalid flush handle fails");
    check(invoke("9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", ro, 0, ptr("bad"), 3) == nx::kWriteNotPermitted, "readonly write rejected");
    check(invoke("11SetFileSizeENS0_10FileHandleEl", ro, 1) == nx::kWriteNotPermitted, "readonly resize rejected");
    check(invoke("11SetFileSizeENS0_10FileHandleEl", h, uint64_t(-1)) == nx::kInvalidSize, "negative resize rejected");
    check(invoke("11SetFileSizeENS0_10FileHandleEl", h, 4) == 0, "resize");
    Output size;
    check(invoke("11GetFileSizeEPlNS0_10FileHandleE", size.address(), h) == 0 && size.words[1] == 4, "file size is s64"); size.guards();
    Output amount;
    check(invoke("8ReadFileEPmNS0_10FileHandleElPvm", amount.address(), h, 0, ptr(bytes), 8) == 0 && amount.words[1] == 4, "counting overload reports EOF short read"); amount.guards();
    check(invoke("8ReadFileENS0_10FileHandleElPvm", h, 0, ptr(bytes), 8) == 0x177a02, "exact overload rejects short read");
    check(invoke("8ReadFileENS0_10FileHandleElPvm", h, uint64_t(-1), ptr(bytes), 1) == nx::kInvalidOffset, "negative read offset");
    fail_seek = true;
    Output failed_size;
    check(invoke("11GetFileSizeEPlNS0_10FileHandleE", failed_size.address(), h) != 0 && failed_size.words[1] == 0xccccccccccccccccull, "failed size query preserves output"); failed_size.guards();
    check(invoke("9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", h, 0, ptr("bad"), 3) != 0, "seek failure is not success");
    fail_seek = false;
    // An actual kernel failure: replace this file descriptor with write-only /dev/full.
    int backup = dup(fd), full = ::open("/dev/full", O_WRONLY);
    check(backup >= 0 && full >= 0 && dup2(full, fd) == fd, "inject /dev/full"); close(full);
    check(invoke("9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", h, 0, ptr("bad"), 3) == ((30 << 9) | 2), "write/fflush ENOSPC propagated");
    Output failed_read;
    check(invoke("8ReadFileEPmNS0_10FileHandleElPvm", failed_read.address(), h, 0, ptr(bytes), 1) != 0 && failed_read.words[1] == 0xccccccccccccccccull, "pread EBADF is not EOF success"); failed_read.guards();
    check(dup2(backup, fd) == fd, "restore descriptor"); close(backup); clearerr(file->f);
    invoke("7UnmountEPKc", ptr("save"));
    check(invoke("14CommitSaveDataEPKc", ptr("save")) == nx::kMountNotFound, "unmounted commit fails");
    check(invoke("13MountSaveDataEPKcRKNS_7account3UidE", ptr("save"), ptr(second)) == 0, "reuse mount name");
    synced.clear();
    check(invoke("14CommitSaveDataEPKc", ptr("save")) == 0 && synced.empty(), "remount does not flush stale handles");
    synced.clear();
    check(invoke("14CommitSaveDataEPKc", ptr("other")) == 0 && synced == std::set<int>{other_fd}, "other mount independent");
    for (auto handle : {h, other, ro}) invoke("9CloseFileENS0_10FileHandleE", handle);
    file.reset(); other_file.reset();
    check(!registry().count("_ZN2nn2fs25GetCurrentThreadFsContextEv") && !registry().count("_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE"), "FsContext remains SDK TLS");
    fs::remove_all(root);
    std::puts("bayonetta3_fs_test: save existence, commit durability/isolation, ABI sentinels and host error checks passed");
}
