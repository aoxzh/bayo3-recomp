// Standalone production IPC/account/local-user tests; no game or GPU.
// clang++ -std=c++20 -march=x86-64-v3 -pthread -DNX_BAYONETTA3=1 \
//   -Iruntime/include tests/account_service_test.cpp -o /tmp/account_service_test
// /tmp/account_service_test
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>

#include "../runtime/src/ipc.cpp"
#include "../runtime/src/users.cpp"
#include "../runtime/src/svc_account.cpp"

namespace nx {
bool g_trace = true;
bool strict_runtime() { return true; }
void log(const char*, ...) {}
[[noreturn]] void fatal(const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    throw std::runtime_error(text);
}
std::vector<std::shared_ptr<KObject>>& handles() {
    static std::vector<std::shared_ptr<KObject>> objects;
    return objects;
}
uint32_t handle_add(std::shared_ptr<KObject> o) { handles().push_back(std::move(o)); return uint32_t(handles().size()); }
std::shared_ptr<KObject> handle_get(uint32_t h) { return h && h <= handles().size() ? handles()[h - 1] : nullptr; }
void handle_close(uint32_t h) { if (h && h <= handles().size()) handles()[h - 1].reset(); }
uint64_t kernel_alloc(uint64_t, uint64_t) { fatal("unexpected allocation"); }
bool commit(uint64_t, uint64_t) { fatal("unexpected commit"); }
void vm_set(uint64_t, uint64_t, uint32_t, uint32_t) { fatal("unexpected vm_set"); }
namespace plat {
std::string test_root;
std::string data_root() { return test_root; }
}
}

namespace {
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
struct TempRoot {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("nx-account-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempRoot() {
        check(std::filesystem::create_directory(path), "temporary data root");
        nx::plat::test_root = path.string();
        // Two real users ensure listing every profile cannot accidentally pass.
        std::ofstream(path / "users.txt") << "112233445566778899aabbccddeeff00 First\n"
                                           "887766554433221100ffeeddccbbaa99 Second\n";
    }
    ~TempRoot() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
std::vector<uint8_t> uid(const nx::UserInfo& u) {
    std::vector<uint8_t> out(16);
    std::memcpy(out.data(), &u.lo, 8);
    std::memcpy(out.data() + 8, &u.hi, 8);
    return out;
}
nx::IpcCtx request(const std::shared_ptr<nx::Service>& s, uint32_t cmd, uint32_t result = 0,
                   const std::vector<uint8_t>& in = {}, nx::Buffer out = {}) {
    nx::IpcCtx c;
    c.cmd = cmd; c.in = in.data(); c.in_size = in.size();
    if (out.addr) c.c.push_back(out);
    check(s->handle(c) == result, "unexpected account Result");
    // Only context's response data is used after return.
    c.in = nullptr;
    return c;
}
void empty_reply(const nx::IpcCtx& c) {
    check(c.out.empty() && c.out_objects.empty() && c.out_copy.empty() && c.out_move.empty(), "unexpected raw output/handle");
}
bool zero(const uint8_t* p, size_t size) {
    return std::all_of(p, p + size, [](uint8_t b) { return b == 0; });
}
}

int main() {
    try {
        TempRoot root;
        const auto all = nx::users_all();
        check(all.size() == 2, "real local users not loaded");
        nx::user_opened(all[1].lo, all[1].hi);
        auto account = nx::make_service("acc:u0");
        auto launch = nx::storage_blob(nx::preselected_user_storage());
        check(launch && std::memcmp(launch->data() + 8, uid(all[1]).data(), 16) == 0, "preselection disagrees with open user");
        std::array<uint8_t, 146> bytes;
        auto list = [&](const std::shared_ptr<nx::Service>& s, size_t size) {
            bytes.fill(0xAD);
            empty_reply(request(s, 131, 0, {}, {uint64_t(bytes.data() + 1), size}));
            check(bytes.front() == 0xAD && bytes[size + 1] == 0xAD, "list overflowed output buffer");
        };
        list(account, 128);
        check(zero(bytes.data() + 1, 128), "cold stored context must be empty despite preselection");
        auto manager = request(account, 101, 0, uid(all[1])).out_objects.at(0);
        empty_reply(request(manager, 160));
        auto other = nx::make_service("acc:u0");
        list(other, 128);
        check(std::memcmp(bytes.data() + 1, uid(all[1]).data(), 16) == 0 && zero(bytes.data() + 17, 112),
              "stored list must contain only selected user and zero terminators");
        nx::user_opened(all[0].lo, all[0].hi);
        list(other, 17);
        check(std::memcmp(bytes.data() + 1, uid(all[1]).data(), 16) == 0 && bytes[17] == 0,
              "stored context must remain a snapshot; trailing partial slot must be zero");
        list(account, 15);
        check(zero(bytes.data() + 1, 15), "partial Uid must not be written");
        list(account, 0);
        empty_reply(request(manager, 160)); // snapshot current open user, not manager owner
        list(other, 16);
        check(std::memcmp(bytes.data() + 1, uid(all[0]).data(), 16) == 0, "StoreOpenContext failed to replace snapshot");
        empty_reply(request(account, 131, nx::kNullptr));
        nx::IpcCtx map_alias;
        map_alias.cmd = 131; map_alias.b.push_back({uint64_t(bytes.data()), 128});
        bytes.fill(0xAD);
        check(account->handle(map_alias) == nx::kNullptr && bytes.front() == 0xAD, "map alias incorrectly accepted as type0xA");

        // Exercise actual CMIF parsing/C-descriptor response, not just lambdas.
        alignas(16) std::array<uint32_t, 64> message{};
        message[0] = 4; message[1] = 8 | (3 << 10);
        message[4] = 0x49434653; message[6] = 131;
        const uint64_t addr = uint64_t(bytes.data());
        message[10] = uint32_t(addr);
        message[11] = uint32_t(addr >> 32) | (128 << 16);
        uint64_t session;
        check(nx::ipc_connect("acc:u0", &session) == 0, "connect account");
        nx::CPU cpu{};
        check(nx::ipc_request(cpu, uint32_t(session), uint64_t(message.data()), sizeof(message)) == 0, "CMIF transport");
        check(message[4] == 0x4F434653 && message[6] == 0, "CMIF response Result");
        check(std::memcmp(bytes.data(), uid(all[0]).data(), 16) == 0 && zero(bytes.data() + 16, 112), "CMIF C buffer UID layout");

        empty_reply(request(account, 101, nx::kNullptr));
        empty_reply(request(account, 101, nx::kInvalidUserId, std::vector<uint8_t>(16)));
        auto missing = uid({42, 43, "Unknown"});
        empty_reply(request(account, 101, nx::kUserNotExist, missing));
        check(request(account, 51, 0, {0}).out == uid(all[0]), "local selection failed");
        empty_reply(request(account, 51, nx::kNetworkServiceAccountRegistrationRequired, {1}));
        empty_reply(request(manager, 0, 0x1907C));
        empty_reply(request(manager, 1, 0x1907C));
        bool failed = false;
        try { request(manager, 2); } catch (const std::runtime_error& e) {
            failed = std::string(e.what()).find("is not implemented") != std::string::npos;
        }
        check(failed, "unsupported ID token operation must fail strict");
        std::puts("account_service: production IPC/local users/stored context/network failures passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "account_service: %s\n", e.what());
        return 1;
    }
}
