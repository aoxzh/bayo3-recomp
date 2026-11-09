// nn::fs at the API level, on host files. The game imports these by name (main), so no fsp-srv IPC is involved:
//   rom:   -> private/extract/romfs (the RomFS with the update applied), overlaid by <data root>/mods (scan_mods)
//   save:  -> private/user/save/<mount>
//   cache: -> private/user/cache<index>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <nx/runtime.h>
#include <nx/game.h>

#include "platform.h"
#include "users.h"
#include "movie.h"

namespace fs = std::filesystem;

namespace nx {

namespace {

// NX_TRACE_FS=1: every file the game opens (and the ones it looks for and does not find).
bool trace_fs() {
    static const bool on = g_trace || std::getenv("NX_TRACE_FS") != nullptr;
    return on;
}

// The last opens, newest last: a crash report that says what was being loaded (log_recent_files, read by the crash
// handlers; try_lock there, a handler must not wait for a thread that holds the lock).
constexpr size_t kRecentOps = 24;
std::mutex g_recent_lock;
struct RecentOp {
    int64_t thread = 0;
    uint64_t seq = 0;
    char text[200] = {0};
} g_recent[kRecentOps];
uint64_t g_recent_seq = 0;

void note_open(const char* what, const std::string& guest_path, const char* result) {
    std::lock_guard lk(g_recent_lock);
    RecentOp& r = g_recent[g_recent_seq % kRecentOps];
    r.thread = plat::current_thread_id();
    r.seq = ++g_recent_seq;
    std::snprintf(r.text, sizeof r.text, "%s %s: %s", what, guest_path.c_str(), result);
}

constexpr uint32_t kOk = 0;
constexpr uint32_t kPathNotFound = 0x202;       // fs 1
constexpr uint32_t kPathAlreadyExists = 0x402;  // fs 2
constexpr uint32_t kTargetNotFound = (1002 << 9) | 2;
constexpr uint32_t kMountNotFound = 0x35F202;  // SDK MountTable::Find (sdk+0x1e04f8)
constexpr uint32_t kOutOfRange = 0x177A02;     // SDK ReadFile without out-size: short read
constexpr uint32_t kInvalidOffset = (6061 << 9) | 2;
constexpr uint32_t kInvalidSize = (6062 << 9) | 2;
constexpr uint32_t kNullArgument = (6063 << 9) | 2;
constexpr uint32_t kReadNotPermitted = (6202 << 9) | 2;
constexpr uint32_t kWriteNotPermitted = (6203 << 9) | 2;
constexpr uint32_t kHostIoError = (5000 << 9) | 2;  // Unexpected: host errors have no exact Horizon equivalent

uint32_t host_error(int error) {
    switch (error) {
        case ENOENT: case ENOTDIR: return kPathNotFound;
        case EEXIST: return kPathAlreadyExists;
        case ENOSPC: return (30 << 9) | 2;
        case EACCES: case EPERM: case EROFS: return (6400 << 9) | 2;
        default: return kHostIoError;
    }
}

std::mutex g_fs;
struct Mount { fs::path root; uint64_t id; bool rom = false; };
std::map<std::string, Mount> g_mounts;
uint64_t g_next_mount = 1;
// Open files: shared so a read in progress survives a close from another thread. Reads run outside g_fs (which
// only guards the tables): one global lock made every streaming thread (Bink animations, FMOD banks) wait behind
// any big read, felt as hitches.
struct File {
    FILE* f;
    std::string path;
    uint64_t mount_id;
    int mode;
    File(FILE* file, std::string p, uint64_t mount, int open_mode)
        : f(file), path(std::move(p)), mount_id(mount), mode(open_mode) {}
    File(const File&) = delete;  // owns the FILE: one instance only
    File& operator=(const File&) = delete;
    ~File() { std::fclose(f); }
};
std::map<uint64_t, std::shared_ptr<File>> g_files;

// Files the runtime adds or replaces (mods): key = path under the mount point, '/' separated, lower case.
std::map<std::string, std::string> g_virtual;

std::string virtual_key(const fs::path& p) {
    auto g = p.lexically_normal().generic_u8string();
    std::string s(g.begin(), g.end());
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The virtual file whose key the resolved path ends with, or nullptr.
const std::string* find_virtual(const fs::path& resolved) {
    if (g_virtual.empty() || resolved.empty()) return nullptr;
    std::string k = virtual_key(resolved);
    for (auto& [key, data] : g_virtual)
        if (k.size() >= key.size() + 1 && k.compare(k.size() - key.size(), key.size(), key) == 0 &&
            k[k.size() - key.size() - 1] == '/')
            return &data;
    return nullptr;
}
struct Dir {
    std::vector<fs::directory_entry> entries;
    size_t pos = 0;
};
std::map<uint64_t, Dir> g_dirs;
uint64_t g_next = 1;

fs::path data_root() { return fs::u8path(plat::data_root()); }

std::string utf8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool is_movie_path(const std::string& path) {
    constexpr std::string_view suffix = ".webm";
    if (path.size() < suffix.size()) return false;
    size_t start = path.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(path[start + i])) != suffix[i]) return false;
    return true;
}

std::string cstr(uint64_t p) { return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); }

// NX_MOVIE_READS=1: every read of a cutscene file (path, offset, size, result, thread), first 48 and then every
// 64th per file. The second movie opens its .webm and then never asks its demuxer for a sample; this says whether
// its bytes are still being read at all, and the thread that goes quiet.
void trace_movie_read(const File& file, int64_t off, uint64_t size, uint64_t got, uint32_t rc) {
    static const bool on = std::getenv("NX_MOVIE_READS") != nullptr;
    if (!on || !is_movie_path(file.path)) return;
    static std::mutex m;
    static std::map<std::string, uint64_t> reads;
    std::lock_guard lk(m);
    const uint64_t k = ++reads[file.path];
    if (k <= 48 || k % 64 == 0)
        log("fs: movie read #%llu thread %lld %s at %lld size %llu -> %llu rc %x", (unsigned long long)k,
            (long long)plat::current_thread_id(), file.path.c_str(), (long long)off,
            (unsigned long long)size, (unsigned long long)got, rc);
}

// Mods: every folder under <data root>/mods replaces game files by their RomFS path, from <mod>/romfs/<path> (or
// <mod>/<path> when the mod has no romfs/ folder). Folders apply in name order and a later one wins; a name starting
// with '-' or '.' is switched off. Read once, when the rom mount is first made; the log lists what was found. Mods
// replace files the game opens by name; a file only a mod has does not show up in a directory listing.
std::vector<fs::path> g_mod_roots;  // the winner first

void scan_mods() {
    g_mod_roots.clear();
    std::error_code ec;
    const fs::path mods = data_root() / "mods";
    if (!fs::is_directory(mods, ec)) return;
    std::vector<fs::path> found;
    for (auto& e : fs::directory_iterator(mods, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string name = utf8(e.path().filename());
        if (name.empty() || name[0] == '-' || name[0] == '.') {
            log("fs: mod %s is off (its name starts with '%c')", name.c_str(), name.empty() ? '?' : name[0]);
            continue;
        }
        found.push_back(fs::is_directory(e.path() / "romfs", ec) ? e.path() / "romfs" : e.path());
    }
    std::sort(found.begin(), found.end());
    for (auto& m : found) log("fs: mod %s", utf8(m).c_str());
    g_mod_roots.assign(found.rbegin(), found.rend());
}

// The mod file that replaces `rest` of the RomFS, or empty.
fs::path mod_file(const std::string& rest) {
    std::error_code ec;
    for (auto& root : g_mod_roots) {
        fs::path p = root / fs::u8path(rest);
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

// "mount:/a/b" -> host path; empty when the mount is unknown.
fs::path resolve(const std::string& guest) {
    auto colon = guest.find(':');
    if (colon == std::string::npos) return {};
    auto it = g_mounts.find(guest.substr(0, colon));
    if (it == g_mounts.end()) return {};
    std::string rest = guest.substr(colon + 1);
    while (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) rest.erase(0, 1);
    if (it->second.rom && !g_mod_roots.empty() && !rest.empty())
        if (fs::path m = mod_file(rest); !m.empty()) {
            if (trace_fs()) log("fs: %s from a mod: %s", guest.c_str(), utf8(m).c_str());
            return m;
        }
    return it->second.root / fs::u8path(rest);
}

uint32_t mount(const std::string& name, const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) return host_error(ec.value());
    g_mounts[name] = {p, g_next_mount++};
    log("fs: mounted %s: -> %s", name.c_str(), p.string().c_str());
    return kOk;
}

// Caller holds g_fs: serialize with writes, resizing and close. fflush alone only
// transfers libc buffers to the kernel; commit/FlushFile must also request durability.
uint32_t flush_file(File& file) {
    if (std::fflush(file.f) != 0) return host_error(errno);
#if defined(_WIN32)
    if (_commit(_fileno(file.f)) != 0) return host_error(errno);
#else
    int rc;
    do { rc = fsync(fileno(file.f)); } while (rc != 0 && errno == EINTR);
    if (rc != 0) return host_error(errno);
#endif
    return kOk;
}

// ---- mounts ---------------------------------------------------------------------------------------------------

void QueryMountRomCacheSize(CPU& c) {
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = kOk;
}

void MountRom(CPU& c) {
    std::lock_guard lk(g_fs);
    const std::string name = cstr(c.x[0]);
    c.x[0] = mount(name, data_root() / "extract" / "romfs");
    if (c.x[0] == kOk) {
        g_mounts[name].rom = true;
        static bool scanned = false;
        if (!scanned) scanned = true, scan_mods();
    }
}

// The save folder of the account::Uid at a guest address: user/save for the first user, user/save_<uid> for the others.
fs::path save_dir_of(uint64_t uid_addr) {
    uint64_t lo = ld<uint64_t>(uid_addr), hi = ld<uint64_t>(uid_addr + 8);
    // Pure path mapping. In particular, existence queries must not initialize the
    // account database or change the last-opened user through user_opened().
    return data_root() / "user" / user_save_dir(lo, hi);
}

void EnsureSaveData(CPU& c) {
    std::error_code ec;
    fs::path p = save_dir_of(c.x[0]);
    fs::create_directories(p, ec);
    log("fs: EnsureSaveData -> %s", p.string().c_str());
    c.x[0] = ec ? host_error(ec.value()) : kOk;
}

void MountSaveData(CPU& c) {
    std::lock_guard lk(g_fs);
    std::string name = cstr(c.x[0]);
    uint64_t uid = c.x[1];
    c.x[0] = mount(name, save_dir_of(uid));
    if (c.x[0] == kOk) user_opened(ld<uint64_t>(uid), ld<uint64_t>(uid + 8));
}

void Unmount(CPU& c) {
    std::lock_guard lk(g_fs);
    g_mounts.erase(cstr(c.x[0]));
    c.x[0] = kOk;
}

// CommitSaveData and Commit use the same SDK helper (sdk+0x1e4150).
void Commit(CPU& c) {
    std::lock_guard lk(g_fs);
    if (!c.x[0]) { c.x[0] = kNullArgument; return; }
    auto mount = g_mounts.find(cstr(c.x[0]));
    if (mount == g_mounts.end()) { c.x[0] = kMountNotFound; return; }
    uint32_t result = kOk;
    for (auto& [handle, file] : g_files) {
        if (file->mount_id != mount->second.id || !(file->mode & 2)) continue;
        uint32_t rc = flush_file(*file);
        if (result == kOk) result = rc;
    }
    c.x[0] = result;
}
void EnableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }
void DisableGlobalFileDataCache(CPU& c) { c.x[0] = kOk; }

// ---- files ----------------------------------------------------------------------------------------------------

void CreateFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    int64_t size = int64_t(c.x[1]);
    if (size < 0) { c.x[0] = kInvalidSize; return; }
    if (p.empty() || !fs::exists(p.parent_path())) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), "wb");
    if (!f) { c.x[0] = host_error(errno); return; }
    uint32_t result = kOk;
    if (size > 0 && (plat::fseek64(f, size - 1, SEEK_SET) != 0 || fputc(0, f) == EOF))
        result = host_error(errno);
    if (fclose(f) != 0 && result == kOk) result = host_error(errno);
    c.x[0] = result;
}

void DeleteFile(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    c.x[0] = !p.empty() && fs::remove(p, ec) ? kOk : kPathNotFound;
}

void CreateDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    if (p.empty()) {
        c.x[0] = kPathNotFound;
        return;
    }
    if (fs::exists(p)) {
        c.x[0] = kPathAlreadyExists;
        return;
    }
    std::error_code ec;
    c.x[0] = fs::create_directory(p, ec) ? kOk : kPathNotFound;
}

void GetEntryType(CPU& c) {  // (DirectoryEntryType* out, path)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    if (find_virtual(p)) {
        st<uint32_t>(c.x[0], 1);
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::exists(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    st<uint32_t>(c.x[0], fs::is_directory(p) ? 0 : 1);
    c.x[0] = kOk;
}

void OpenFile(CPU& c) {  // (FileHandle* out, path, int mode: 1 read, 2 write, 4 append)
    std::lock_guard lk(g_fs);
    std::string guest = cstr(c.x[1]);
    fs::path p = resolve(guest);
    int mode = int(c.x[2]);
    if (const std::string* v = find_virtual(p)) {  // served from memory, through a temporary file
        FILE* f = std::tmpfile();
        if (!f) {
            c.x[0] = kPathNotFound;
            return;
        }
        std::fwrite(v->data(), 1, v->size(), f);
        std::fflush(f);
        uint64_t h = g_next++;
        g_files[h] = std::make_shared<File>(f, guest, g_mounts.at(guest.substr(0, guest.find(':'))).id, mode);
        st<uint64_t>(c.x[0], h);
        note_open("OpenFile", guest, "virtual");
        if (trace_fs()) log("fs: OpenFile(%s) -> virtual, %zu bytes", guest.c_str(), v->size());
        c.x[0] = kOk;
        return;
    }
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) {
        note_open("OpenFile", guest, p.empty() ? "unknown mount" : "not found");
        if (trace_fs()) log("fs: OpenFile(%s) not found", guest.c_str());
        c.x[0] = kPathNotFound;
        return;
    }
    FILE* f = plat::fopen_utf8(utf8(p), (mode & 2) ? "r+b" : "rb");
    if (!f) {
        note_open("OpenFile", guest, "the host could not open it");
        if (trace_fs()) log("fs: OpenFile(%s) failed to open on the host (errno %d)", guest.c_str(), errno);
        c.x[0] = kPathNotFound;
        return;
    }
    uint64_t h = g_next++;
    g_files[h] = std::make_shared<File>(f, guest, g_mounts.at(guest.substr(0, guest.find(':'))).id, mode);
    st<uint64_t>(c.x[0], h);
    note_open("OpenFile", guest, "ok");
    if (is_movie_path(guest)) {
        // romfs cutscene/title video: host-side decode feeds its nvdec planes, and reads below are traceable. The
        // caller is the game's cutscene manager, which is the layer that decides *whether* to play a movie at all.
        log("fs: OpenFile(%s) -> handle %llu (caller %s)", guest.c_str(), (unsigned long long)h,
            describe(c.x[30]).c_str());
        movie_on_open(guest, utf8(p), h);
    }
    if (trace_fs()) log("fs: OpenFile(%s, %d) -> %llu", guest.c_str(), mode, (unsigned long long)h);
    c.x[0] = kOk;
}

std::shared_ptr<File> open_file(uint64_t h) {
    std::lock_guard lk(g_fs);
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second;
}

void CloseFile(CPU& c) {
    std::lock_guard lk(g_fs);
    if (auto it = g_files.find(c.x[0]); it != g_files.end()) {
        const std::string& gp = it->second->path;
        if (is_movie_path(gp)) {
            log("fs: CloseFile(%s) handle %llu", gp.c_str(), (unsigned long long)c.x[0]);
            movie_on_close(gp, c.x[0]);
        }
    }
    g_files.erase(c.x[0]);
}

void GetFileSize(CPU& c) {  // (s64* out, FileHandle)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    std::lock_guard lk(g_fs);  // the FILE position is shared with writers
    FILE* f = file->f;
    int64_t cur = plat::ftell64(f);
    if (cur < 0 || plat::fseek64(f, 0, SEEK_END) != 0) { c.x[0] = host_error(errno); return; }
    int64_t size = plat::ftell64(f);
    int error = errno;
    if (plat::fseek64(f, cur, SEEK_SET) != 0) { c.x[0] = host_error(errno); return; }
    if (size < 0) { c.x[0] = host_error(error); return; }
    st<int64_t>(c.x[0], size);
    c.x[0] = kOk;
}

uint32_t read_at(const File& file, int64_t off, uint64_t buf, uint64_t size, uint64_t& n) {
    if (!(file.mode & 1)) return kReadNotPermitted;
    if (off < 0) return kInvalidOffset;
    if (size > uint64_t(std::numeric_limits<int64_t>::max()) - uint64_t(off)) return kInvalidSize;
    uint64_t t0 = plat::monotonic_ns();
    n = 0;
#if defined(_WIN32)
    errno = 0;
    n = plat::read_at(file.f, off, reinterpret_cast<void*>(buf), size);
    if (n < size && errno) {
        trace_movie_read(file, off, size, n, host_error(errno));
        return host_error(errno);
    }
#else
    // platform::read_at returns only a byte count and discards pread failures.
    // Preserve EOF versus I/O error here without serializing streaming reads.
    while (n < size) {
        ssize_t got = pread(fileno(file.f), reinterpret_cast<uint8_t*>(buf) + n, size_t(size - n), off + int64_t(n));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            trace_movie_read(file, off, size, n, host_error(errno));
            return host_error(errno);
        }
        if (got == 0) break;
        n += uint64_t(got);
    }
#endif
    trace_movie_read(file, off, size, n, kOk);
    uint64_t ms = (plat::monotonic_ns() - t0) / 1'000'000;
    if (g_trace) log("fs: read %s at %lld size %llu -> %llu", file.path.c_str(), (long long)off, (unsigned long long)size,
                     (unsigned long long)n);
    if (trace_fs() && file.path.find(".bwav") != std::string::npos) {  // streamed audio: is it being consumed?
        static std::mutex m;
        static std::map<std::string, uint64_t> reads;
        std::lock_guard lk(m);
        uint64_t k = ++reads[file.path];
        if (k <= 3 || k % 50 == 0) log("fs: stream read #%llu %s at %lld", (unsigned long long)k, file.path.c_str(), (long long)off);
    }
    if (ms >= 30)
        log("fs: slow read %s: %llu bytes at %lld took %llu ms", file.path.c_str(), (unsigned long long)size,
            (long long)off, (unsigned long long)ms);
    return kOk;
}

void ReadFileOut(CPU& c) {  // (size_t* out, FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[1]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    uint64_t n = 0;
    uint32_t rc = read_at(*file, int64_t(c.x[2]), c.x[3], c.x[4], n);
    if (rc == kOk) st<uint64_t>(c.x[0], n);
    c.x[0] = rc;
}

void ReadFile(CPU& c) {  // (FileHandle, s64 off, void* buf, size_t size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    uint64_t n = 0;
    uint32_t rc = read_at(*file, int64_t(c.x[1]), c.x[2], c.x[3], n);
    c.x[0] = rc != kOk ? rc : n == c.x[3] ? kOk : kOutOfRange;
}

void WriteFile(CPU& c) {  // (FileHandle, s64 off, const void* buf, size_t size, const WriteOption&)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    if (!(file->mode & 2)) { c.x[0] = kWriteNotPermitted; return; }
    if (int64_t(c.x[1]) < 0) { c.x[0] = kInvalidOffset; return; }
    if (c.x[3] > uint64_t(std::numeric_limits<int64_t>::max()) - c.x[1]) { c.x[0] = kInvalidSize; return; }
    std::lock_guard lk(g_fs);
    if (plat::fseek64(file->f, int64_t(c.x[1]), SEEK_SET) != 0 ||
        fwrite(reinterpret_cast<const void*>(c.x[2]), 1, c.x[3], file->f) != c.x[3] ||
        fflush(file->f) != 0) { c.x[0] = host_error(errno); return; }
    // WriteOption is a const reference to a 32-bit flag; bit 0 requests durable flush.
    c.x[0] = c.x[4] && (ld<uint32_t>(c.x[4]) & 1) ? flush_file(*file) : kOk;
}

void FlushFile(CPU& c) {
    auto file = open_file(c.x[0]);
    if (!file) { c.x[0] = kTargetNotFound; return; }
    if (!(file->mode & 2)) { c.x[0] = kWriteNotPermitted; return; }
    std::lock_guard lk(g_fs);
    c.x[0] = flush_file(*file);
}

// ---- directories ----------------------------------------------------------------------------------------------

void OpenDirectory(CPU& c) {  // (DirectoryHandle* out, path, int mode: 1 dirs, 2 files)
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[1]));
    int mode = int(c.x[2]);
    std::error_code ec;
    bool found = !p.empty() && fs::is_directory(p, ec);
    note_open("OpenDirectory", cstr(c.x[1]), found ? "ok" : "not found");
    if (trace_fs()) log("fs: OpenDirectory(%s, %d)%s", cstr(c.x[1]).c_str(), mode, found ? "" : " not found");
    if (!found) {
        c.x[0] = kPathNotFound;
        return;
    }
    Dir d;
    for (auto& e : fs::directory_iterator(p, ec)) {
        bool dir = e.is_directory();
        if ((dir && (mode & 1)) || (!dir && (mode & 2))) d.entries.push_back(e);
    }
    uint64_t h = g_next++;
    g_dirs[h] = std::move(d);
    st<uint64_t>(c.x[0], h);
    c.x[0] = kOk;
}

void ReadDirectory(CPU& c) {  // (s64* out_count, DirectoryEntry* entries, DirectoryHandle, s64 max)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[2]);
    if (it == g_dirs.end()) {
        c.x[0] = kTargetNotFound;
        return;
    }
    Dir& d = it->second;
    int64_t n = 0, max = int64_t(c.x[3]);
    for (; n < max && d.pos < d.entries.size(); n++, d.pos++) {
        const auto& e = d.entries[d.pos];
        uint64_t out = c.x[1] + uint64_t(n) * 0x310;
        std::memset(reinterpret_cast<void*>(out), 0, 0x310);
        auto u8 = e.path().filename().u8string();
        std::string name(u8.begin(), u8.end());
        std::memcpy(reinterpret_cast<void*>(out), name.c_str(), std::min<size_t>(name.size(), 0x300));
        bool dir = e.is_directory();
        st<int8_t>(out + 0x304, dir ? 0 : 1);
        st<int64_t>(out + 0x308, dir ? 0 : int64_t(e.file_size()));
    }
    st<int64_t>(c.x[0], n);
    c.x[0] = kOk;
}

void GetDirectoryEntryCount(CPU& c) {  // (s64* out, DirectoryHandle)
    std::lock_guard lk(g_fs);
    auto it = g_dirs.find(c.x[1]);
    st<int64_t>(c.x[0], it == g_dirs.end() ? 0 : int64_t(it->second.entries.size()));
    c.x[0] = kOk;
}

void CloseDirectory(CPU& c) {
    std::lock_guard lk(g_fs);
    g_dirs.erase(c.x[0]);
}

// ---- cache storage --------------------------------------------------------------------------------------------

void OpenCacheStorageList(CPU& c) {
    st<uint64_t>(c.x[0], 1);
    c.x[0] = kOk;
}
void ReadCacheStorageList(CPU& c) {  // (int* out, CacheStorageInfo*, handle, int)
    st<int32_t>(c.x[0], 0);
    c.x[0] = kOk;
}
void CloseCacheStorageList(CPU& c) {}
void CreateCacheStorage(CPU& c) { c.x[0] = kOk; }
void DeleteCacheStorage(CPU& c) { c.x[0] = kOk; }
void MountCacheStorage(CPU& c) {  // (name, int index)
    std::lock_guard lk(g_fs);
    c.x[0] = mount(cstr(c.x[0]), data_root() / "user" / ("cache" + std::to_string(int(c.x[1]))));
}

// ---- SDK 16 additions (Super Mario Bros. Wonder) ---------------------------------------------------------------
// MountRom(name) without a cache buffer, and ReadFile(size_t* out, FileHandle, s64, void*, size_t, const ReadOption&)
// are the older calls with one argument less / more: the same functions serve them.

bool save_exists(uint64_t uid) {
    if (!uid || !(ld<uint64_t>(uid) || ld<uint64_t>(uid + 8))) return false;
    std::error_code ec;
    return fs::is_directory(save_dir_of(uid), ec) && !ec;
}

void IsSaveDataExisting(CPU& c) { c.x[0] = save_exists(c.x[0]); }

// SDK sdk+0x2393d0: ApplicationId is passed by value in x0, Uid by reference
// in x1; the result is bool, not Result/out-bool. This data root belongs to one title.
void IsSaveDataExistingForApplication(CPU& c) {
    c.x[0] = c.x[0] == game::kInfo.program_id && save_exists(c.x[1]);
}

void SetFileSize(CPU& c) {  // (FileHandle, s64 size)
    auto file = open_file(c.x[0]);
    if (!file) {
        c.x[0] = kTargetNotFound;
        return;
    }
    if (!(file->mode & 2)) { c.x[0] = kWriteNotPermitted; return; }
    if (int64_t(c.x[1]) < 0) { c.x[0] = kInvalidSize; return; }
    std::lock_guard lk(g_fs);
    if (std::fflush(file->f) != 0) { c.x[0] = host_error(errno); return; }
#if defined(_WIN32)
    int error = int(_chsize_s(_fileno(file->f), int64_t(c.x[1])));
#else
    int error = ftruncate(fileno(file->f), off_t(c.x[1])) == 0 ? 0 : errno;
#endif
    c.x[0] = error ? host_error(error) : kOk;
}

void DeleteDirectoryRecursively(CPU& c) {
    std::lock_guard lk(g_fs);
    fs::path p = resolve(cstr(c.x[0]));
    std::error_code ec;
    if (p.empty() || !fs::is_directory(p, ec)) {
        c.x[0] = kPathNotFound;
        return;
    }
    fs::remove_all(p, ec);
    c.x[0] = ec ? kPathNotFound : kOk;
}

}  // namespace

NX_HLE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", QueryMountRomCacheSize);
NX_HLE("_ZN2nn2fs8MountRomEPKcPvm", MountRom);
NX_HLE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", EnsureSaveData);
NX_HLE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", MountSaveData);
NX_HLE("_ZN2nn2fs7UnmountEPKc", Unmount);
NX_HLE("_ZN2nn2fs10DeleteFileEPKc", DeleteFile);
NX_HLE("_ZN2nn2fs10CreateFileEPKcl", CreateFile);
NX_HLE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", OpenFile);
NX_HLE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", GetFileSize);
NX_HLE("_ZN2nn2fs15CreateDirectoryEPKc", CreateDirectory);
NX_HLE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", GetEntryType);
NX_HLE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", CloseFile);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", ReadFileOut);
NX_HLE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", ReadFile);
NX_HLE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", WriteFile);
NX_HLE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", FlushFile);
NX_HLE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", EnableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs26DisableGlobalFileDataCacheEv", DisableGlobalFileDataCache);
NX_HLE("_ZN2nn2fs6CommitEPKc", Commit);
NX_HLE("_ZN2nn2fs14CommitSaveDataEPKc", Commit);
NX_HLE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", OpenDirectory);
NX_HLE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", ReadDirectory);
NX_HLE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", CloseDirectory);
NX_HLE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", GetDirectoryEntryCount);
NX_HLE("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", OpenCacheStorageList);
NX_HLE("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", ReadCacheStorageList);
NX_HLE("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", CloseCacheStorageList);
NX_HLE("_ZN2nn2fs18CreateCacheStorageEill", CreateCacheStorage);
NX_HLE("_ZN2nn2fs18DeleteCacheStorageEi", DeleteCacheStorage);
NX_HLE("_ZN2nn2fs17MountCacheStorageEPKci", MountCacheStorage);
NX_HLE("_ZN2nn2fs8MountRomEPKc", MountRom);
NX_HLE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", ReadFileOut);
NX_HLE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", IsSaveDataExisting);
NX_HLE("_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", IsSaveDataExistingForApplication);
// Get/SetCurrentThreadFsContext intentionally remain SDK TLS operations. SDK
// GetTlsValue/SetTlsValue use [TPIDRRO+0x1f8]->ThreadType+0x80+slot*8; SDK main
// initialization and thread entry install that pointer. No mount table is involved.
NX_HLE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", SetFileSize);
NX_HLE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", DeleteDirectoryRecursively);

void log_recent_files() {
    std::unique_lock lk(g_recent_lock, std::try_to_lock);
    if (!lk.owns_lock()) return;
    log("  last file opens (newest last; thread = host thread id):");
    for (uint64_t k = 0; k < kRecentOps; k++) {
        const RecentOp& r = g_recent[(g_recent_seq + k) % kRecentOps];
        if (r.seq) log("    #%llu thread %lld  %s", (unsigned long long)r.seq, (long long)r.thread, r.text);
    }
}

void add_virtual_file(const std::string& path, std::string data) {
    std::lock_guard lk(g_fs);
    g_virtual[virtual_key(fs::u8path(path))] = std::move(data);
}

std::string read_game_file(const std::string& relative_to_romfs) {
    FILE* f = plat::fopen_utf8(utf8(data_root() / "extract" / "romfs" / fs::u8path(relative_to_romfs)), "rb");
    if (!f) return {};
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

}  // namespace nx
