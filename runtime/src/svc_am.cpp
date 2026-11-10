// am services (appletOE): application proxy, focus/operation-mode state, launch parameters.
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <set>

#include "ipc.h"
#include "language.h"
#include "platform.h"
#include "render_scale.h"
#include "users.h"

namespace nx {

namespace {

constexpr uint32_t kAmNoMessage = 0x680;  // am error 3
constexpr uint32_t kMsgFocusStateChanged = 15;
constexpr uint32_t kMsgOperationModeChanged = 30;
constexpr uint32_t kMsgPerformanceModeChanged = 31;

// One application per host process; all service objects share this state. ipc_request
// serializes handlers under g_ipc. The host never launches a second guest process.
struct AppletState {
    std::deque<uint32_t> messages{kMsgFocusStateChanged, kMsgOperationModeChanged, kMsgPerformanceModeChanged};
    std::shared_ptr<EventState> message_event = std::make_shared<EventState>();
    bool request_exit_to_library_applet_at_execute_next_program_enabled = false;
    bool operation_mode_notification = false;
    bool performance_mode_notification = false;
    bool focus_notification = false, background_execution = false, suspend_on_focus_loss = false;
    bool restart_message_enabled = false, out_of_focus_suspending_enabled = false;
    bool foreground_rights = false, exiting = false;
    // This single-application host does not suspend the guest on focus loss.
    uint64_t suspended_ticks = 0;
    std::shared_ptr<EventState> suspended_tick_event = std::make_shared<EventState>();
};

AppletState& applet() {
    static AppletState s;
    return s;
}

uint32_t event_handle(std::shared_ptr<EventState> st) {
    auto e = std::make_shared<KEvent>();
    e->state = std::move(st);
    return handle_add(e);
}

std::shared_ptr<Service> svc(const char* name, std::function<void(Service&)> setup) {
    auto s = std::make_shared<Service>(name);
    setup(*s);
    return s;
}

std::shared_ptr<Service> common_state_getter() {
    return svc("ICommonStateGetter", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) {  // GetEventHandle
            applet().message_event->signaled = !applet().messages.empty();
            c.out_copy.push_back(event_handle(applet().message_event));
            return 0u;
        };
        s.cmds[1] = [](IpcCtx& c) {  // ReceiveMessage
            auto& a = applet();
            if (a.messages.empty()) {
                a.message_event->signaled = false;
                return kAmNoMessage;
            }
            c.push<uint32_t>(a.messages.front());
            a.messages.pop_front();
            if (a.messages.empty()) a.message_event->signaled = false;
            return 0u;
        };
        // NX_HANDHELD=1: handheld mode, so the game renders 1280x720 (a lighter GPU load) instead of docked (1600x900).
        static const bool handheld = nx::plat::handheld_mode();
        s.cmds[5] = [](IpcCtx& c) { c.push<uint8_t>(handheld ? 0 : 1); return 0u; };   // GetOperationMode
        s.cmds[6] = [](IpcCtx& c) { c.push<uint32_t>(handheld ? 0 : 1); return 0u; };  // GetPerformanceMode
        s.cmds[9] = [](IpcCtx& c) { c.push<uint8_t>(1); return 0u; };   // GetCurrentFocusState: in focus
        // SetCpuBoostMode(CpuBoostMode): Wonder asks for the boost around every load. A PC has no clock to raise, so
        // the request is taken and dropped (no output). Same answer as the generic stub, without its log line.
        s.cmds[66] = [](IpcCtx&) { return 0u; };
        // libnx applet.c: CmdNoIO(900), HOS 11+. Citron's common_state_getter.cpp
        // sets the same per-applet flag; this is an enable latch, not an exit request.
        // Actual program replacement is rejected explicitly by cmd120 below.
        s.cmds[900] = [](IpcCtx&) {
            applet().request_exit_to_library_applet_at_execute_next_program_enabled = true;
            return 0u;
        };
        // The size the game renders at: NX_RENDER_RES=<w>x<h> if given, else the console mode's own size scaled by
        // NX_RENDER_SCALE=<0.25..2>. It is independent of the window size (`resolution`): the present step fits the
        // picture into the window keeping its aspect, so a phone or tablet at 20:9, 16:10 or 4:3 shows bars rather
        // than a stretched picture. render_scale.h holds the policy (and its test).
        static const std::pair<uint32_t, uint32_t> res = [] {
            const nx::plat::RenderSize s = nx::plat::render_size(handheld, std::getenv("NX_RENDER_RES"),
                                                                 std::getenv("NX_RENDER_SCALE"));
            return std::pair<uint32_t, uint32_t>{s.w, s.h};
        }();
        s.cmds[60] = [](IpcCtx& c) {  // GetDefaultDisplayResolution
            c.push<uint32_t>(res.first);
            c.push<uint32_t>(res.second);
            return 0u;
        };
        s.cmds[61] = [](IpcCtx& c) {  // GetDefaultDisplayResolutionChangeEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
    });
}

std::shared_ptr<Service> self_controller() {
    return svc("ISelfController", [](Service& s) {
        s.cmds[40] = [](IpcCtx& c) { c.push<uint64_t>(1); return 0u; };  // CreateManagedDisplayLayer
        s.cmds[0] = [](IpcCtx&) -> uint32_t {  // Exit: same host shutdown as svcExitProcess
            applet().exiting = true;
            log("am: ISelfController Exit");
            plat::exit_process(0);
        };
        // libnx applet.c: byte booleans, three packed bytes for cmd13. These
        // policies are retained across controller objects; no focus transition
        // or resume is synthesized while this host runs its sole foreground app.
        s.cmds[11] = [](IpcCtx& c) { applet().operation_mode_notification = c.arg<uint8_t>(0) != 0; return 0u; };
        s.cmds[12] = [](IpcCtx& c) { applet().performance_mode_notification = c.arg<uint8_t>(0) != 0; return 0u; };
        s.cmds[13] = [](IpcCtx& c) {
            applet().focus_notification = c.arg<uint8_t>(0) != 0;
            applet().background_execution = c.arg<uint8_t>(1) != 0;
            applet().suspend_on_focus_loss = c.arg<uint8_t>(2) != 0;
            return 0u;
        };
        s.cmds[14] = [](IpcCtx& c) { applet().restart_message_enabled = c.arg<uint8_t>(0) != 0; return 0u; };
        s.cmds[16] = [](IpcCtx& c) { applet().out_of_focus_suspending_enabled = c.arg<uint8_t>(0) != 0; return 0u; };
        s.cmds[90] = [](IpcCtx& c) { c.push<uint64_t>(applet().suspended_ticks); return 0u; };
        s.cmds[91] = [](IpcCtx& c) {
            c.out_copy.push_back(event_handle(applet().suspended_tick_event));
            return 0u;
        };
        s.cmds[9] = [](IpcCtx& c) {  // GetLibraryAppletLaunchableEvent: signaled, a library applet can be launched
            auto e = std::make_shared<EventState>();
            e->signaled = true;
            c.out_copy.push_back(event_handle(e));
            return 0u;
        };
        s.cmds[62] = [](IpcCtx&) { return 0u; };                            // SetIdleTimeDetectionExtension
        // Skip Cutscene ends here without this: the game calls it the moment the pause menu's
        // skip is confirmed, and nothing in the public ISelfController tables matches the id, so
        // treat it as a setter-shaped no-op and log what it carries.
        s.cmds[130] = [](IpcCtx& c) {
            log("am: ISelfController cmd 130 (skip path) arg0 %u arg1 %u arg2 %u", c.arg<uint32_t>(0),
                c.arg<uint32_t>(4), c.arg<uint32_t>(8));
            return 0u;
        };
        for (uint32_t cmd : {19u, 50u, 71u, 110u})                           // AccumulatedSuspendedTickChangedEvent etc.
            s.cmds[cmd] = [](IpcCtx& c) {
                c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
                return 0u;
            };
    });
}

std::shared_ptr<Service> window_controller() {
    return svc("IWindowController", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) { c.push<uint64_t>(0x51); return 0u; };  // GetAppletResourceUserId
        s.cmds[10] = [](IpcCtx&) {  // AcquireForegroundRights: sole application owns the display
            applet().foreground_rights = true;
            return 0u;
        };
    });
}

std::string hex_dump(const std::vector<uint8_t>& d, size_t max = 0x40) {
    std::string out;
    char b[4];
    for (size_t i = 0; i < d.size() && i < max; i++) {
        std::snprintf(b, sizeof b, "%02x", d[i]);
        out += b;
    }
    return out;
}

// ILibraryAppletAccessor of one library applet. Only the account applets (playerSelect, id 0x10) are emulated, with no
// UI: a "create user" request makes a new local user, a "select user" request answers with the last opened one.
// Switchbrew: Applet Manager services (ILibraryAppletAccessor) and the playerSelect applet (PselUiMode).
std::shared_ptr<Service> library_applet(uint32_t applet_id, uint32_t mode) {
    struct State {
        uint32_t id;
        std::shared_ptr<EventState> done = std::make_shared<EventState>();
        std::shared_ptr<EventState> out_ready = std::make_shared<EventState>();
        std::shared_ptr<EventState> inter_ready = std::make_shared<EventState>();
        std::vector<std::vector<uint8_t>> in;
        std::deque<std::shared_ptr<Service>> out;
        bool started = false, exit_requested = false;
        uint32_t result = 0;
    };
    auto st = std::make_shared<State>();
    st->id = applet_id;
    log("am: CreateLibraryApplet(id 0x%x, mode %u)", applet_id, mode);
    return svc("ILibraryAppletAccessor", [st](Service& s) {
        s.cmds[0] = [st](IpcCtx& c) {  // GetAppletStateChangedEvent
            c.out_copy.push_back(event_handle(st->done));
            return 0u;
        };
        s.cmds[1] = [st](IpcCtx& c) { c.push<uint8_t>(st->started); return 0u; };  // IsCompleted
        s.cmds[10] = [st](IpcCtx&) {                                                // Start
            for (size_t i = 0; i < st->in.size(); i++)
                log("am: applet 0x%x input storage #%zu (%zu bytes): %s", st->id, i, st->in[i].size(), hex_dump(st->in[i]).c_str());
            st->started = true;
            std::vector<uint8_t> out;
            if (st->id == 0x10 && !st->in.empty()) {  // playerSelect: the last input is PselUiSettings (u32 UiMode first)
                const auto& in = st->in.back();
                uint32_t ui = in.size() >= 4 ? uint32_t(in[0] | in[1] << 8 | in[2] << 16 | uint32_t(in[3]) << 24) : 0;
                UserInfo u;
                if (ui == 1 || ui == 5) {  // UserCreator / UserCreatorForStarter
                    u = user_create();
                    log("am: playerSelect UserCreator -> %s", u.valid() ? u.nickname.c_str() : "no room (8 users)");
                } else if (ui == 0) {  // UserSelector
                    u = user_last_opened();
                    log("am: playerSelect UserSelector -> %s", u.nickname.c_str());
                } else {
                    log("am: playerSelect UiMode %u not handled", ui);
                }
                out.assign(0x18, 0);  // PselUiReturnArg {u64 result; Uid}
                if (u.valid()) {
                    std::memcpy(out.data() + 8, &u.lo, 8);
                    std::memcpy(out.data() + 16, &u.hi, 8);
                    user_opened(u.lo, u.hi);
                } else {
                    uint64_t cancelled = 0x2;  // nothing chosen
                    std::memcpy(out.data(), &cancelled, 8);
                }
            } else {
                log("am: library applet 0x%x is not emulated; answering with an empty output", st->id);
            }
            st->out.push_back(make_storage(std::move(out)));
            st->out_ready->signaled = true;
            st->done->signaled = true;
            return 0u;
        };
        s.cmds[20] = [st](IpcCtx&) {  // RequestExit
            st->exit_requested = true;
            // Our account applet runs synchronously in Start. There is no live
            // guest applet process to notify once its completion event is set.
            if (!st->done->signaled)
                fatal("ILibraryAppletAccessor RequestExit: unfinished applet 0x%x has no host exit handler", st->id);
            return 0u;
        };
        s.cmds[25] = [st](IpcCtx&) {  // Terminate
            if (!st->done->signaled)
                fatal("ILibraryAppletAccessor Terminate: unfinished applet 0x%x has no host termination handler", st->id);
            return 0u;
        };
        s.cmds[30] = [st](IpcCtx&) { return st->result; };  // GetResult
        s.cmds[50] = [](IpcCtx&) { return 0u; };  // SetOutOfFocusApplicationSuspendingEnabled
        s.cmds[100] = [st](IpcCtx& c) {           // PushInData(IStorage)
            if (!c.in_services.empty())
                if (auto b = storage_blob(c.in_services[0])) st->in.push_back(*b);
            return 0u;
        };
        s.cmds[101] = [st](IpcCtx& c) {  // PopOutData -> IStorage
            if (st->out.empty()) return 0x2A80u;
            c.push_object(st->out.front());
            st->out.pop_front();
            if (st->out.empty()) st->out_ready->signaled = false;
            return 0u;
        };
        s.cmds[102] = [](IpcCtx&) { return 0u; };  // PushExtraStorage
        s.cmds[103] = [](IpcCtx&) { return 0u; };  // PushInteractiveInData
        s.cmds[104] = [](IpcCtx&) { return 0x2A80u; };  // PopInteractiveOutData: none
        s.cmds[105] = [st](IpcCtx& c) { c.out_copy.push_back(event_handle(st->out_ready)); return 0u; };  // GetPopOutDataEvent
        s.cmds[106] = [st](IpcCtx& c) { c.out_copy.push_back(event_handle(st->inter_ready)); return 0u; };
        s.cmds[110] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // NeedsToExitProcess
        s.cmds[150] = [](IpcCtx&) { return 0u; };                         // RequestForAppletToGetForeground
    });
}

std::shared_ptr<Service> library_applet_creator() {
    return svc("ILibraryAppletCreator", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(library_applet(c.arg<uint32_t>(0), c.arg<uint32_t>(4))); return 0u; };
        s.cmds[1] = [](IpcCtx&) { return 0u; };                              // TerminateAllLibraryApplets
        s.cmds[2] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };       // AreAnyLibraryAppletsLeft
        s.cmds[10] = [](IpcCtx& c) {                                         // CreateStorage(s64 size)
            int64_t n = c.arg<int64_t>(0);
            c.push_object(make_storage(std::vector<uint8_t>(n > 0 && n < (1 << 24) ? size_t(n) : 0, 0)));
            return 0u;
        };
        s.cmds[11] = [](IpcCtx& c) {  // CreateTransferMemoryStorage(bool writable, s64 size, handle): not backed
            log("am: CreateTransferMemoryStorage is not supported");
            c.push_object(make_storage(std::vector<uint8_t>()));
            return 0u;
        };
    });
}

std::shared_ptr<Service> application_functions() {
    return svc("IApplicationFunctions", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) {  // PopLaunchParameter(kind): the preselected user, once
            // kind 2 is the preselected user (libnx appletPopLaunchParameter). Anything else is a
            // launch parameter the applet manager would have handed the *new* process after
            // IApplicationFunctions::ExecuteProgram (the hub's picture book does that to start program 1,
            // the Bayonetta Origins prologue, and program 1 does it to return). This host keeps what
            // ExecuteProgram recorded in launch_parameter.txt when the launcher restarts the game, serves it
            // once for the first non-user kind, and logs every kind so the request sequence is visible.
            //
            // An empty channel is 2-128-2 (0x480), not 0x2A80: the SDK's
            // nn::applet::TryPopFromApplicationParameterChannel (sdk+0x3c5d0) compares the result with 0x480
            // and aborts on anything else, so program 1, whose start calls nn::oe::TryPopLaunchParameter,
            // stopped on its first frame with "abort with Result 0x2a80" (B3 itself never pops kind 1).
            constexpr uint32_t kNoDataInChannel = 0x480u;
            const uint32_t kind = c.arg<uint32_t>(0);
            static std::set<uint32_t> seen;
            static bool served_parameter = false;
            static bool popped = false;
            if (seen.insert(kind).second) log("am: PopLaunchParameter(kind %u)", kind);
            if (kind == 2) {
                if (popped) return kNoDataInChannel;
                popped = true;
                c.push_object(preselected_user_storage());
                return 0u;
            }
            if (served_parameter) return kNoDataInChannel;
            FILE* f = plat::fopen_utf8(plat::data_root() + "/launch_parameter.txt", "rb");
            if (!f) return kNoDataInChannel;
            std::vector<uint8_t> blob;
            uint8_t buf[256];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) blob.insert(blob.end(), buf, buf + n);
            std::fclose(f);
            std::remove((plat::data_root() + "/launch_parameter.txt").c_str());
            served_parameter = true;
            log("am: served the relaunch parameter (%zu bytes) for PopLaunchParameter(kind %u): %s", blob.size(), kind,
                hex_dump(blob).c_str());
            c.push_object(make_storage(std::move(blob)));
            return 0u;
        };
        s.cmds[20] = [](IpcCtx& c) { c.push<uint64_t>(0); return 0u; };  // EnsureSaveData
        s.cmds[21] = [](IpcCtx& c) {                                      // GetDesiredLanguage
            uint64_t code = desired_language();
            char tag[9] = {0};
            std::memcpy(tag, &code, 8);
            log("am: desired language = %s", tag);
            c.push<uint64_t>(code);
            return 0u;
        };
        s.cmds[22] = [](IpcCtx& c) {  // SetTerminateResult (SwitchBrew AM): the Result to report if we terminate.
            // The game sets one at chapter start; logged so an upstream error stays visible, then accepted.
            log("am: SetTerminateResult(%#010x)", c.arg<uint32_t>(0));
            return 0u;
        };
        s.cmds[40] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // NotifyRunning
        s.cmds[50] = [](IpcCtx& c) {                                     // GetPseudoDeviceId
            c.push<uint64_t>(0x1234);
            c.push<uint64_t>(0x5678);
            return 0u;
        };
        s.cmds[120] = [](IpcCtx& c) -> uint32_t {  // ExecuteProgram(u32 kind, padding, u64 value)
            // libnx's ExecuteProgram/JumpToSubApplication/RestartProgram all use this command, and the
            // console's answer is always the same: this guest process ends and the applet manager starts a
            // fresh one (with the new launch parameter). The bar hub's "Records of Time" book uses it to
            // start a chapter, which is the first place the game reaches it in a normal session.
            if (c.in_size < 16) fatal("IApplicationFunctions ExecuteProgram: truncated arguments (%zu bytes)", c.in_size);
            const uint32_t kind = c.arg<uint32_t>(0);
            const uint64_t value = c.arg<uint64_t>(8);
            static const bool relaunch = [] {
                const char* e = std::getenv("NX_RELAUNCH");
                return !(e && e[0] == '0');
            }();
            if (!relaunch) {
                fatal("IApplicationFunctions ExecuteProgram: single-application host cannot replace/restart the "
                      "guest (kind=%u, value=%llu, request_exit_to_library_applet=%u; NX_RELAUNCH=0)",
                      kind, static_cast<unsigned long long>(value),
                      unsigned(applet().request_exit_to_library_applet_at_execute_next_program_enabled));
            }
            // The launcher (scripts/b3_play.sh, and the experiment loop) restarts the game with the same data
            // root when it sees this exit code. The marker carries what was asked for, for the log; the game
            // itself finds the selected chapter in its save data, as on the console.
            FILE* f = plat::fopen_utf8(plat::data_root() + "/relaunch.txt", "wb");
            if (f) {
                std::fprintf(f, "%u %llu\n", kind, static_cast<unsigned long long>(value));
                std::fclose(f);
            }
            log("am: ExecuteProgram(kind %u, value %llu): the guest process ends here; the launcher restarts it "
                "(exit code 86, relaunch.txt written)", kind, static_cast<unsigned long long>(value));
            plat::exit_process(86);
            return 0u;  // not reached
        };
        s.cmds[121] = [](IpcCtx& c) {  // ClearUserChannel
            // The bar hub's "Records of Time" book calls this just before it starts a chapter
            // (IApplicationFunctions 121, no in/out data). It drops whatever the player channel
            // storage held; this host does not model that storage, so success is the honest answer
            // and the game continues instead of the strict runtime aborting here.
            static bool logged = false;
            if (!logged) {
                logged = true;
                log("am: ClearUserChannel (121) -> success (player channel storage is not modelled)");
            }
            return 0u;
        };
        s.cmds[122] = [](IpcCtx& c) {  // UnpopToUserChannel (switchbrew's name for command 122)
            // The hub's records book calls this right after ClearUserChannel (121) and immediately
            // before ExecuteProgram (120): the application is about to be replaced, and this is where
            // a player-channel object would be handed to the *next* process. Keep whatever arrives
            // (an IStorage in the request) so the new process can be given it, and log it once.
            static bool logged = false;
            const uint32_t arg = c.arg<uint32_t>(0);
            std::vector<uint8_t> blob;
            for (const auto& svc_obj : c.in_services)
                if (auto b = storage_blob(svc_obj)) blob = *b;
            if (!logged) {
                logged = true;
                log("am: UnpopToUserChannel (122, arg %u): %zu storage byte(s) %s", arg, blob.size(),
                    blob.empty() ? "(none)" : hex_dump(blob).c_str());
            }
            if (!blob.empty()) {
                if (FILE* f = plat::fopen_utf8(plat::data_root() + "/user_channel.bin", "wb")) {
                    std::fwrite(blob.data(), 1, blob.size(), f);
                    std::fclose(f);
                }
            }
            return 0u;
        };
        s.cmds[130] = [](IpcCtx& c) {  // GetGpuErrorDetectedSystemEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
        s.cmds[210] = [](IpcCtx&) { return 0u; };  // UnsetGameCardEject... (stub)
    });
}

std::shared_ptr<Service> application_proxy() {
    return svc("IApplicationProxy", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(common_state_getter()); return 0u; };
        s.cmds[1] = [](IpcCtx& c) { c.push_object(self_controller()); return 0u; };
        s.cmds[2] = [](IpcCtx& c) { c.push_object(window_controller()); return 0u; };
        s.cmds[3] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IAudioController")); return 0u; };
        s.cmds[4] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDisplayController")); return 0u; };
        s.cmds[10] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IProcessWindingController")); return 0u; };
        s.cmds[11] = [](IpcCtx& c) { c.push_object(library_applet_creator()); return 0u; };
        s.cmds[20] = [](IpcCtx& c) { c.push_object(application_functions()); return 0u; };
        s.cmds[1000] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDebugFunctions")); return 0u; };
    });
}

ServiceRegistrar reg_applet_oe("appletOE", [] {
    return svc("appletOE", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(application_proxy()); return 0u; };  // OpenApplicationProxy
    });
});

}  // namespace

}  // namespace nx
