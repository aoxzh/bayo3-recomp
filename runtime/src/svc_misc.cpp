// Small host services: parental controls, add-on content, logging, power and error context.
#include <cstdlib>
#include "ipc.h"
#include "platform.h"

namespace nx {

namespace {

std::shared_ptr<Service> make(const char* name, std::function<void(Service&)> setup = {}) {
    auto s = std::make_shared<Service>(name);
    if (setup) setup(*s);
    return s;
}

struct ParentalControlState {
    // Local host policy: no PIN/restrictions are configured.
    bool initialized = false;
    bool free_communication = false;
};

std::shared_ptr<Service> parental_control(bool initialize) {
    auto state = std::make_shared<ParentalControlState>();
    state->initialized = initialize;
    return make("IParentalControlService", [state](Service& s) {
        // libnx pctl.c: CreateServiceWithoutInitialize then CmdNoIO(1).
        s.cmds[1] = [state](IpcCtx&) {
            state->initialized = true;
            state->free_communication = false;
            return 0u;
        };
        s.cmds[1031] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsRestrictionEnabled (libnx)
        s.cmds[1403] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsPairingActive (no paired parental-control app)
        s.cmds[1001] = [state](IpcCtx&) {  // CheckFreeCommunicationPermission
            if (!state->initialized) fatal("IParentalControlService CheckFreeCommunicationPermission before Initialize");
            state->free_communication = true;
            return 0u;
        };
    });
}

void pctl(Service& s) {
    s.cmds[0] = [](IpcCtx& c) { c.push_object(parental_control(true)); return 0u; };  // CreateService
    s.cmds[1] = [](IpcCtx& c) { c.push_object(parental_control(false)); return 0u; };  // CreateServiceWithoutInitialize
}

ServiceRegistrar r_pctl("pctl", [] { return make("pctl", pctl); });
ServiceRegistrar r_pctl_s("pctl:s", [] { return make("pctl:s", pctl); });
ServiceRegistrar r_pctl_a("pctl:a", [] { return make("pctl:a", pctl); });
ServiceRegistrar r_pctl_r("pctl:r", [] { return make("pctl:r", pctl); });

ServiceRegistrar r_lm("lm", [] {
    return make("lm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(make("ILogger")); return 0u; };  // OpenLogger
    });
});

// libnx apm.c cmd0/1: (u32 mode, u32 configuration) / (u32 mode)->u32.
// Configuration is shared across sessions. It is a guest power-policy request;
// host scheduling/clocks remain managed by the OS, as in emulator APM backends.
uint32_t* performance_configurations() {
    static uint32_t configs[2] = {0x20003, 0x20003};  // Citron APM default Config7
    return configs;
}

std::shared_ptr<Service> power_session() {
    return make("ISession", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) {
            if (c.in_size < 8 || c.arg<uint32_t>(0) > 1)
                fatal("apm ISession SetPerformanceConfiguration: invalid mode/payload");
            performance_configurations()[c.arg<uint32_t>(0)] = c.arg<uint32_t>(4);
            return 0u;
        };
        s.cmds[1] = [](IpcCtx& c) {
            if (c.in_size < 4 || c.arg<uint32_t>(0) > 1)
                fatal("apm ISession GetPerformanceConfiguration: invalid mode/payload");
            c.push<uint32_t>(performance_configurations()[c.arg<uint32_t>(0)]);
            return 0u;
        };
    });
}

// Ryujinx Services/Ectx/IWriterForApplication.cs registers this HOS 11+ endpoint.
// Connecting creates a real IPC session; no writer commands are implemented yet,
// so any attempted submission still reaches Service::handle's strict failure.
ServiceRegistrar r_ectx_aw("ectx:aw", [] { return make("IWriterForApplication"); });

// Original B3 SDK's prepo shim connects directly to IPrepoService (observed: SaveReportWithUser[Old3] 10105 at
// chapter start, right after EnsureSaveData). The service only uploads Nintendo telemetry (SwitchBrew BCAT_services);
// a local session accepts every report and drops it. The other families stay strict until observed.
ServiceRegistrar r_prepo_u("prepo:u", [] {
    return make("IPrepoService", [](Service& s) {
        auto save_report = [](IpcCtx& c) {
            if (const Buffer report = c.in_buf(0); report.addr && g_trace)
                log("prepo: SaveReport dropped (%llu bytes)", (unsigned long long)report.size);
            return 0u;
        };
        for (uint32_t cmd = 10100; cmd <= 10107; ++cmd) s.cmds[cmd] = save_report;
    });
});

ServiceRegistrar r_apm("apm", [] {
    return make("apm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(power_session()); return 0u; };  // OpenSession
        s.cmds[1] = [](IpcCtx& c) {  // GetPerformanceMode: boost (docked), normal with NX_HANDHELD (svc_am.cpp)
            c.push<uint32_t>(nx::plat::handheld_mode() ? 0 : 1);
            return 0u;
        };
    });
});

// Application-scoped content state. The loader mounts only extract/romfs;
// there is no separately installed or mounted DLC and no hot-install path.
// Keep mount validation separate from enumeration: a future DLC backend must
// populate both and implement validation before enabling nonempty inventories.
struct AddOnContentState {
    std::vector<int32_t> installed;
    std::vector<uint64_t> mounted;
    std::shared_ptr<EventState> changed = std::make_shared<EventState>();
};
AddOnContentState& add_on_content() {
    static AddOnContentState state;
    return state;
}
void require_empty_add_on_content() {
    const auto& state = add_on_content();
    if (!state.installed.empty() || !state.mounted.empty())
        fatal("aoc:u: nonempty add-on-content inventory requires a DLC mount/validation backend");
}

ServiceRegistrar r_aoc("aoc:u", [] {
    return make("aoc:u", [](Service& s) {
        s.cmds[50] = [](IpcCtx& c) {  // CheckAddOnContentMountStatus (13.0.0+)
            // Original B3 SDK +0x35618 -> +0x362a0: u64 PID placeholder
            // with send-PID, no buffers and no raw output (Result only).
            if (c.in_size < 8) fatal("aoc:u CheckAddOnContentMountStatus: truncated PID placeholder");
            require_empty_add_on_content();
            // No DLC mounts can have disappeared or become invalid. This is
            // the empty-set validation case, not a claim that DLC is mounted.
            if (g_trace) log("aoc:u CheckAddOnContentMountStatus: installed=0 mounted=0 valid");
            return 0u;
        };
        s.cmds[2] = [](IpcCtx& c) {  // CountAddOnContent
            require_empty_add_on_content();
            c.push<int32_t>(int32_t(add_on_content().installed.size()));
            return 0u;
        };
        s.cmds[3] = [](IpcCtx& c) {  // ListAddOnContent: no elements written, count only
            require_empty_add_on_content();
            c.push<int32_t>(0);
            return 0u;
        };
        s.cmds[7] = [](IpcCtx&) -> uint32_t {  // PrepareAddOnContent
            fatal("aoc:u PrepareAddOnContent: requested DLC is not installed in this base-RomFS application");
        };
        auto changed_event = [](IpcCtx& c) {
            auto e = std::make_shared<KEvent>();
            e->state = add_on_content().changed;
            c.out_copy.push_back(handle_add(e));
            return 0u;
        };
        s.cmds[8] = changed_event;   // GetAddOnContentListChangedEvent
        s.cmds[10] = changed_event;  // GetAddOnContentListChangedEventWithProcessId (SDK +0x35598)
    });
});

}  // namespace

}  // namespace nx
