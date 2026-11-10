// Standalone production AM/IPC command tests; no game, GPU, or guest code.
// cmake -S . -B private/build-am-tests -DNX_TESTS=ON -DNX_GEN_DIR=/nonexistent
// cmake --build private/build-am-tests --target am_lifecycle_test
// ctest --test-dir private/build-am-tests -R am_lifecycle --output-on-failure
#include <cstdarg>
#include <string>
#include <cstdio>
#include <stdexcept>

// Include the production handlers so state can be inspected without a runtime
// testing API. Service::handle and the registry are the real strict IPC path.
#include "../runtime/src/ipc.cpp"
#include "../runtime/src/svc_am.cpp"
#include "../runtime/src/svc_misc.cpp"

namespace nx {
bool g_trace = false;
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
UserInfo user_create() { return {1, 2, "Test"}; }
UserInfo user_last_opened() { return {1, 2, "Test"}; }
void user_opened(uint64_t, uint64_t) {}
std::shared_ptr<Service> make_storage(std::vector<uint8_t>) { return std::make_shared<Service>("TestStorage"); }
std::shared_ptr<std::vector<uint8_t>> storage_blob(const std::shared_ptr<Service>&) { return {}; }
std::shared_ptr<Service> preselected_user_storage() { return make_storage(std::vector<uint8_t>{}); }
namespace plat {
struct Exit { int code; };
[[noreturn]] void exit_process(int code) { throw Exit{code}; }
std::string setting(const std::string&) { return {}; }
std::string data_root() { return {}; }
FILE* fopen_utf8(const std::string&, const char*) { return nullptr; }
}
}

namespace {
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
nx::IpcCtx command(const std::shared_ptr<nx::Service>& s, uint32_t id, const std::vector<uint8_t>& in = {}) {
    nx::IpcCtx c;
    c.cmd = id; c.in = in.data(); c.in_size = in.size();
    check(s->handle(c) == 0, "command failed");
    return c;
}
// The diagnostic alone is ambiguous (several checks share one string), so the line number goes into
// the failure: a red test must name the check, not just the family of check.
template<class F> void fails_at(F f, const char* diagnostic, int line) {
    try { f(); } catch (const std::runtime_error& e) {
        check(std::string(e.what()).find(diagnostic) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error(std::string("expected explicit failure: ") + diagnostic +
                             " (tests/am_lifecycle_test.cpp:" + std::to_string(line) + ")");
}

#define fails(f, diagnostic) fails_at((f), (diagnostic), __LINE__)
std::shared_ptr<nx::Service> object(const std::shared_ptr<nx::Service>& s, uint32_t id) {
    auto c = command(s, id);
    check(c.out_objects.size() == 1, "missing object");
    return c.out_objects[0];
}
}

int main() {
    try {
        auto proxy = object(nx::make_service("appletOE"), 0);
        auto common = object(proxy, 0), self = object(proxy, 1), window = object(proxy, 2), app = object(proxy, 20);
        auto& state = nx::applet();
        check(!state.request_exit_to_library_applet_at_execute_next_program_enabled, "flag must default false");
        const auto messages = state.messages;
        std::vector<uint8_t> execute(16);
        execute[8] = 7;
        // ExecuteProgram (IApplicationFunctions 120) is not a refusal: the host ends this guest process
        // with code 86 and the launcher restarts it with the requested program
        // (scripts/run_linux.sh reads relaunch.txt). Only the launch parameter decides which program;
        // the kinds the console accepts all take the same road here.
        auto expect_relaunch = [&](const std::vector<uint8_t>& in, const char* what) {
            try { command(app, 120, in); }
            catch (const nx::plat::Exit& e) { check(e.code == 86, what); return; }
            throw std::runtime_error(std::string("ExecuteProgram did not end the process: ") + what);
        };
        expect_relaunch(execute, "ExecuteProgram(kind 0) must exit 86");
        auto enabled = command(common, 900);
        check(enabled.out.empty() && enabled.out_copy.empty() && enabled.out_objects.empty(), "cmd900 must have no output");
        check(state.request_exit_to_library_applet_at_execute_next_program_enabled, "cmd900 did not latch");
        command(object(proxy, 0), 900); // a second interface must share state; repeated enables are idempotent
        check(state.messages == messages && !state.exiting, "cmd900 must not request an exit or enqueue messages");
        expect_relaunch(execute, "kind=0, value=7, request_exit_to_library_applet=1 must exit 86");
        execute[0] = 2; execute[8] = 0;
        expect_relaunch(execute, "kind=2, value=0 must exit 86");
        fails([&] { command(app, 120); }, "truncated arguments");
        check(state.request_exit_to_library_applet_at_execute_next_program_enabled && state.messages == messages, "failed handoff mutated state");
        fails([&] { command(common, 0xFFFFFF); }, "is not implemented"); // production strict fallback stays fatal

        command(self, 11, {1}); command(self, 12, {1}); command(self, 13, {1, 0, 1});
        command(self, 14, {1}); command(self, 16, {1}); command(window, 10);
        check(state.operation_mode_notification && state.performance_mode_notification && state.focus_notification &&
              !state.background_execution && state.suspend_on_focus_loss && state.restart_message_enabled &&
              state.out_of_focus_suspending_enabled && state.foreground_rights, "AM policies not retained");
        command(object(proxy, 1), 11, {0});
        check(!state.operation_mode_notification, "setter did not clear shared state");
        auto tick = command(self, 90);
        check(tick.out == std::vector<uint8_t>(8, 0), "foreground app accrued suspended ticks");
        auto event = command(self, 91), event2 = command(object(proxy, 1), 91);
        auto e1 = std::dynamic_pointer_cast<nx::KEvent>(nx::handle_get(event.out_copy.at(0)));
        auto e2 = std::dynamic_pointer_cast<nx::KEvent>(nx::handle_get(event2.out_copy.at(0)));
        check(e1 && e2 && e1->state == e2->state && !e1->signaled(), "suspended tick event must be shared and unsignaled");

        auto power = nx::make_service("apm");
        auto session = object(power, 0), session2 = object(power, 0);
        command(session, 0, {0, 0, 0, 0, 1, 0, 1, 0});
        command(session, 0, {1, 0, 0, 0, 4, 0, 2, 0});
        check(command(session2, 1, {0, 0, 0, 0}).out == std::vector<uint8_t>({1, 0, 1, 0}), "normal power config not retained");
        check(command(session2, 1, {1, 0, 0, 0}).out == std::vector<uint8_t>({4, 0, 2, 0}), "boost power config not retained");
        fails([&] { command(session, 0, {2, 0, 0, 0, 0, 0, 0, 0}); }, "invalid mode/payload");
        auto pctl = nx::make_service("pctl:a");
        auto parental = object(pctl, 1);
        fails([&] { command(parental, 1001); }, "before Initialize");
        command(parental, 1); command(parental, 1); command(parental, 1001);
        check(command(parental, 1031).out == std::vector<uint8_t>{0}, "unexpected parental restriction");
        command(object(pctl, 0), 1001); // CreateService initializes, unlike CreateServiceWithoutInitialize
        fails([&] { command(object(pctl, 1), 1001); }, "before Initialize"); // independent session state
        auto writer = nx::make_service("ectx:aw");
        check(writer->name == "IWriterForApplication", "wrong ectx endpoint");
        fails([&] { command(writer, 0); }, "is not implemented");
        fails([&] { nx::make_service("unknown:test"); }, "unimplemented service");
        auto prepo = nx::make_service("prepo:u");
        check(prepo->name == "IPrepoService", "prepo must create a direct telemetry session");
        // prepo:u accepts and drops the observed telemetry range (10100-10107, svc_misc.cpp): there is
        // no local destination for a report, and the game sends SaveReport at chapter start. Strictness
        // stays for anything outside that range.
        check(command(prepo, 10104).out.empty(), "prepo report must be accepted and dropped");
        fails([&] { command(prepo, 10108); }, "is not implemented");
        auto lm = nx::make_service("lm");
        auto logger = object(lm, 0);
        check(logger->name == "ILogger", "lm OpenLogger must return an ILogger");
        // Retail stubs ILogger (SwitchBrew Log_services): a log line is accepted and dropped, and the
        // destination/hashed-log commands succeed without configuring anything. Commands outside the
        // implemented 0-3 range stay strict.
        check(command(logger, 0, std::vector<uint8_t>(16)).out.empty(), "ILogger Log must accept and drop the message");
        check(command(logger, 1, std::vector<uint8_t>(4)).out.empty(), "ILogger SetDestination must succeed without a local destination");
        check(command(logger, 2, std::vector<uint8_t>(4)).out.empty(), "ILogger TransmitHashedLog must be accepted and dropped");
        check(command(logger, 3, std::vector<uint8_t>(4)).out.empty(), "ILogger DevNotify must be accepted and dropped");
        fails([&] { command(logger, 11); }, "is not implemented");
        auto aoc = nx::make_service("aoc:u");
        auto mount_status = command(aoc, 50, std::vector<uint8_t>(8));
        check(mount_status.out.empty() && mount_status.out_objects.empty() && mount_status.out_copy.empty(),
              "AOC mount validation must have Result-only reply");
        fails([&] { command(aoc, 50); }, "truncated PID placeholder");
        check(command(aoc, 2).out == std::vector<uint8_t>(4), "base application must have no DLC");
        check(command(aoc, 3).out == std::vector<uint8_t>(4), "empty DLC list count");
        fails([&] { command(aoc, 7); }, "requested DLC is not installed");
        auto aoc_event = command(aoc, 8), aoc_event2 = command(nx::make_service("aoc:u"), 10);
        auto ae1 = std::dynamic_pointer_cast<nx::KEvent>(nx::handle_get(aoc_event.out_copy.at(0)));
        auto ae2 = std::dynamic_pointer_cast<nx::KEvent>(nx::handle_get(aoc_event2.out_copy.at(0)));
        check(ae1 && ae2 && ae1->state == ae2->state && !ae1->signaled(), "AOC event must be shared and unsignaled");
        nx::add_on_content().mounted.push_back(123);
        fails([&] { command(aoc, 50, std::vector<uint8_t>(8)); }, "requires a DLC mount/validation backend");
        nx::add_on_content().mounted.clear();
        nx::add_on_content().installed.push_back(1);
        fails([&] { command(aoc, 2); }, "requires a DLC mount/validation backend");
        nx::add_on_content().installed.clear();

        auto holder = nx::library_applet(0x10, 0);
        fails([&] { command(holder, 20); }, "unfinished applet");
        fails([&] { command(holder, 25); }, "unfinished applet");
        command(holder, 10); // synchronous applet completion
        check(command(holder, 1).out == std::vector<uint8_t>{1}, "applet did not complete");
        command(holder, 20); command(holder, 20); command(holder, 25);
        check(command(holder, 110).out == std::vector<uint8_t>{0}, "completed local applet must not exit application");
        try { command(self, 0); throw std::runtime_error("Exit returned success without exiting"); }
        catch (const nx::plat::Exit& e) { check(e.code == 0 && state.exiting, "wrong host exit"); }
        std::puts("am_lifecycle: production command/state tests passed (strict enabled)");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "am_lifecycle: %s\n", e.what());
        return 1;
    }
}
