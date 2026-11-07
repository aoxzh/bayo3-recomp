// nvdrv: the NVIDIA driver IPC surface, on the switchbrew command numbering (1=Ioctl, 4=QueryEvent,
// 13=SetGraphicsFirmwareMemoryMarginEnabled, ...). NvMMLite drives the movie decoder (nvdec) through
// it, and a decode fence is a syncpt threshold the decoder signals once the frame is done. With no
// real decoder there are no hardware increments, so SUBMIT bumps the channel's syncpt counter and
// the wait ioctls report it: the player's fences always pass and the stream runs to its end (black
// frames, game-side audio still plays). Guest memory is host memory, so nvmap allocations and the
// address-space ioctls can pass through untouched.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ipc.h"
#include "kernel.h"
#include "nv_os_sem.h"
#include "platform.h"

namespace nx {
namespace {

struct NvState {
    std::mutex m;
    uint32_t next_fd = 1, next_nvmap = 1, next_syncpt = 0xC0;
    std::map<uint32_t, std::string> fds;        // fd -> device name
    std::map<uint32_t, uint32_t> syncpt_of;     // channel fd -> syncpt id
    std::map<uint32_t, uint64_t> syncpt_value;  // syncpt id -> counter
    std::map<uint32_t, uint32_t> nvmap_size;    // nvmap handle -> size
    std::set<uint32_t> seen_ioctls;
    std::map<uint64_t, uint64_t> ioctl_counts;  // (fd << 32 | iocode) -> calls, NX_TRACE_NV_COUNTS
    uint64_t ioctl_total = 0;
};

// A decode fence event: the waiter is woken by the kernel, and CLEAR_EVENT_WAIT re-arms it before
// the next wait, so keep every queried event signaled at display rate.
std::mutex g_events_m;
std::vector<std::weak_ptr<EventState>> g_events;

void pulse_thread() {
    plat::set_thread_name("nv-syncpt");
    for (;;) {
        plat::sleep_ns(16'666'667);
        bool any = false;
        {
            std::lock_guard g(g_events_m);
            for (size_t k = 0; k < g_events.size();) {
                if (auto e = g_events[k].lock()) {
                    e->signaled = true;
                    any = true;
                    ++k;
                } else {
                    g_events.erase(g_events.begin() + k);
                }
            }
        }
        if (any) notify_all();
    }
}

std::shared_ptr<KEvent> sync_event() {
    auto ev = std::make_shared<KEvent>();
    ev->state = std::make_shared<EventState>();
    ev->state->signaled = true;
    static std::once_flag once;
    std::call_once(once, [] { std::thread(pulse_thread).detach(); });
    std::lock_guard g(g_events_m);
    g_events.push_back(ev->state);
    return ev;
}

// Bounds-checked access into an ioctl's in/out buffers (guest memory is host memory).
static uint32_t ioc(uint32_t x) { return x & 0xFFFF; }  // direction/size bits stripped

template <typename T>
T peek(Buffer b, size_t off, T def = {}) {
    if (!b.addr || off + sizeof(T) > b.size) return def;
    T v;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(b.addr) + off, sizeof(T));
    return v;
}

template <typename T>
void poke(Buffer b, size_t off, T v) {
    if (!b.addr || off + sizeof(T) > b.size) return;
    std::memcpy(reinterpret_cast<uint8_t*>(b.addr) + off, &v, sizeof(T));
}

// NVHOST_IOCTL_CHANNEL_SUBMIT: parse the variable struct far enough to find the syncpt increments
// and bump them, then report the reached thresholds. Layout (switchbrew): 4 counts, cmdbufs
// (12 B each), relocs (12 B) + shifts (4 B), syncpt_incrs (20 B each), fence thresholds (4 B each).
void channel_submit(NvState& st, Buffer a, Buffer b) {
    uint32_t ncmd = peek<uint32_t>(a, 0), nrel = peek<uint32_t>(a, 4);
    uint32_t ninc = peek<uint32_t>(a, 8), nfence = peek<uint32_t>(a, 12);
    size_t inc_off = 16 + size_t(ncmd) * 12 + size_t(nrel) * 16;
    std::lock_guard g(st.m);
    for (uint32_t k = 0; k < ninc; ++k) {
        uint32_t id = peek<uint32_t>(a, inc_off + size_t(k) * 20);
        uint32_t incrs = peek<uint32_t>(a, inc_off + size_t(k) * 20 + 4);
        st.syncpt_value[id] += incrs;
    }
    uint64_t value = ninc ? st.syncpt_value[peek<uint32_t>(a, inc_off)] : 0;
    size_t fence_off = inc_off + size_t(ninc) * 20;
    for (uint32_t k = 0; k < nfence; ++k) poke<uint32_t>(b, fence_off + size_t(k) * 4, uint32_t(value));
}

// One ioctl against a device fd. Unhandled ioctls pass their input through untouched, which keeps
// zero-initialized output for queries nobody has needed yet.
uint32_t do_ioctl(NvState& st, IpcCtx& c) {
    uint32_t fd = c.arg<uint32_t>(0), iocode = c.arg<uint32_t>(4);
    Buffer a = c.in_buf(0), b = c.out_buf(0);
    std::string dev;
    {
        std::lock_guard g(st.m);
        dev = st.fds[fd];
        if (st.seen_ioctls.insert(iocode).second && g_trace) log("nvdrv: %s ioctl 0x%08x", dev.c_str(), iocode);
        // NX_TRACE_NV_COUNTS=1: how many ioctls each device actually sees, per 2000 calls. The first-use trace
        // above cannot answer "does the second movie's decoder get any work at all" — a count can: with the
        // engine's movie feed stalling after ev0005, nvdec would show no calls after its open.
        static const bool counts = std::getenv("NX_TRACE_NV_COUNTS") != nullptr;
        if (counts) {
            st.ioctl_counts[uint64_t(fd) << 32 | iocode]++;
            if (++st.ioctl_total % 2000 == 0) {
                std::string s;
                for (const auto& [k, v] : st.ioctl_counts) {
                    const std::string& name = st.fds[uint32_t(k >> 32)];
                    size_t slash = name.find_last_of('/');
                    char line[96];
                    std::snprintf(line, sizeof line, " %s:0x%x=%llu", name.c_str() + (slash == std::string::npos ? 0 : slash + 1),
                                  uint32_t(k), (unsigned long long)v);
                    s += line;
                }
                log("nvdrv: ioctls over %llu calls:%s", (unsigned long long)st.ioctl_total, s.c_str());
            }
        }
    }
    if (dev == "/dev/nvmap") {
        switch (ioc(iocode)) {
        case 0x0101: {  // CREATE: in size, out handle
            std::lock_guard g(st.m);
            uint32_t h = st.next_nvmap++;
            st.nvmap_size[h] = peek<uint32_t>(a, 0);
            poke<uint32_t>(b, 0, peek<uint32_t>(a, 0));
            poke<uint32_t>(b, 4, h);
            break;
        }
        case 0x0103: {  // FROM_ID: id -> handle
            std::lock_guard g(st.m);
            poke<uint32_t>(b, 4, st.next_nvmap++);
            break;
        }
        case 0x0104: {  // ALLOC: record nothing; the address is guest memory already
            break;
        }
        case 0x0109: {  // PARAM
            uint32_t param = peek<uint32_t>(a, 4), v = 0;
            {
                std::lock_guard g(st.m);
                if (param == 1) v = st.nvmap_size[peek<uint32_t>(a, 0)];
            }
            if (param == 2) v = 0x1000;
            if (param == 4) v = 0x40000000;  // HEAP
            poke<uint32_t>(b, 8, v);
            break;
        }
        case 0x010E:  // GET_ID
            poke<uint32_t>(b, 0, peek<uint32_t>(a, 4));
            break;
        }
    } else if (dev == "/dev/nvhost-ctrl") {
        switch (ioc(iocode)) {
        case 0x0014:  // SYNCPT_READ
        case 0x001A: {  // SYNCPT_READ_MAX
            std::lock_guard g(st.m);
            poke<uint32_t>(b, 4, uint32_t(st.syncpt_value[peek<uint32_t>(a, 0)]));
            break;
        }
        case 0x0015: {  // SYNCPT_INCR
            std::lock_guard g(st.m);
            st.syncpt_value[peek<uint32_t>(a, 0)] += 1;
            break;
        }
        case 0x0019:  // SYNCPT_WAITEX: out value
        case 0x001D:  // SYNCPT_WAIT_EVENT
        case 0x001E: {  // SYNCPT_WAIT_EVENT_EX
            std::lock_guard g(st.m);
            uint32_t id = peek<uint32_t>(a, 0);
            poke<uint32_t>(b, 12, uint32_t(st.syncpt_value[id]));
            break;
        }
        case 0x0016:  // SYNCPT_WAIT: waits report success with the fence reached
        case 0x001C:  // SYNCPT_CLEAR_EVENT_WAIT
        case 0x001F:  // SYNCPT_ALLOC_EVENT
        case 0x0020:  // SYNCPT_FREE_EVENT
            break;
        case 0x001B:  // GET_CONFIG: empty value -> driver defaults
            if (b.addr && b.size) std::memset(reinterpret_cast<void*>(b.addr), 0, b.size);
            break;
        }
    } else {  // channel devices (/dev/nvhost-nvdec, -gpu, -vic, ...)
        switch (ioc(iocode)) {
        case 0x0002: {  // GET_SYNCPOINT: hand the channel its own syncpt
            std::lock_guard g(st.m);
            uint32_t id = st.syncpt_of.emplace(fd, st.next_syncpt++).first->second;
            poke<uint32_t>(b, 4, id);
            break;
        }
        case 0x0001:  // SUBMIT / SUBMIT_EX
            channel_submit(st, a, b);
            break;
        case 0x4805:  // ALLOC_GPFIFO
        case 0x4801:  // SET_NVMAP_FD
        case 0x0003:  // GET_WAITBASE: always 0
            poke<uint32_t>(b, 4, 0);
            break;
        }
    }
    c.push<uint32_t>(0);
    return 0u;
}

ServiceRegistrar r_nvdrv("nvdrv", [] {
    auto st = std::make_shared<NvState>();
    auto sp = std::make_shared<Service>("nvdrv");
    Service& s = *sp;
    s.cmds[0] = [st](IpcCtx& c) {  // Open(path)
        Buffer n = c.in_buf(0);
        char name[65] = {};
        if (n.addr && n.size) {
            size_t k = n.size < 64 ? n.size : 64;
            std::memcpy(name, reinterpret_cast<const void*>(n.addr), k);
        }
        uint32_t fd;
        // NX_NO_NVDEC=1: refuse the hardware decoder so the player falls back to its own software VP9
        // decoder. NvMMLite drives nvdec through ioctls we cannot execute, so its frames are never written
        // and the movie shows black; the game ships a full libvpx (subsdk0: V_VP9, vp9.0), and refusing the
        // device is the one signal that can steer it there. Experiment: a rejected open must not be fatal.
        static const bool no_nvdec = std::getenv("NX_NO_NVDEC") != nullptr;
        if (no_nvdec && std::strstr(name, "nvdec")) {
            log("nvdrv: open %s refused (NX_NO_NVDEC): the software decoder must take it", name);
            c.push<uint32_t>(0);
            c.push<uint32_t>(2);  // ENOENT: no such device
            return 2u;
        }
        {
            std::lock_guard g(st->m);
            fd = st->next_fd++;
            st->fds[fd] = name;
        }
        log("nvdrv: open %s -> fd %u", name, fd);
        c.push<uint32_t>(fd);
        c.push<uint32_t>(0);
        return 0u;
    };
    s.cmds[1] = [st](IpcCtx& c) { return do_ioctl(*st, c); };  // Ioctl
    s.cmds[2] = [st](IpcCtx& c) {                              // Close(fd)
        std::lock_guard g(st->m);
        st->fds.erase(c.arg<uint32_t>(0));
        return 0u;
    };
    s.cmds[3] = [](IpcCtx& c) {  // Initialize(process, transfer memory, size)
        c.push<uint32_t>(0);
        return 0u;
    };
    auto query_event = [](IpcCtx& c) {  // QueryEvent(fd, id) -> err, handle
        if (g_trace) log("nvdrv: query event (fd %u id %u)", c.arg<uint32_t>(0), c.arg<uint32_t>(4));
        c.push<uint32_t>(0);
        c.out_move.push_back(handle_add(sync_event()));
        return 0u;
    };
    s.cmds[4] = query_event;   // QueryEvent
    s.cmds[5] = [](IpcCtx&) { return 0u; };  // MapSharedMem: the backing is already guest memory
    s.cmds[6] = [](IpcCtx& c) {  // GetStatus: NvDrvStatus
        Buffer b = c.out_buf(0);
        if (b.addr && b.size) std::memset(reinterpret_cast<void*>(b.addr), 0, b.size);
        c.push<uint32_t>(0);
        return 0u;
    };
    s.cmds[7] = [](IpcCtx&) { return 0u; };  // SetAruidWithoutCheck
    s.cmds[8] = [](IpcCtx&) { return 0u; };  // SetAruid
    s.cmds[9] = [](IpcCtx&) { return 0u; };  // DumpStatus
    s.cmds[10] = [](IpcCtx&) { return 0u; };  // InitializeDevtools
    s.cmds[11] = [st](IpcCtx& c) { return do_ioctl(*st, c); };  // Ioctl2
    s.cmds[12] = [st](IpcCtx& c) { return do_ioctl(*st, c); };  // Ioctl3
    s.cmds[13] = [](IpcCtx&) { return 0u; };  // SetGraphicsFirmwareMemoryMarginEnabled
    return sp;
});

// mm:u: the multimedia power-delivery manager NvMMLite talks to before using a decoder. Its
// policies only matter on hardware power rails, so accept every request and succeed.
ServiceRegistrar r_mm_u("mm:u", [] {
    auto sp = std::make_shared<Service>("mm:u");
    Service& s = *sp;
    for (uint32_t cmd = 0; cmd <= 10; ++cmd) s.cmds[cmd] = [](IpcCtx&) { return 0u; };
    return sp;
});

// ---- the NvOs* layer the faked nvdec is built on --------------------------------------------------------------
//
// The movie library in subsdk0 (NvMMLite, the guest side of the faked nvdec) owns one NvOsSemaphore per decoder
// context: the context init at subsdk0+0x50e20 ends with `NvOsSemaphoreCreate(&ctx[0x4020], 0)` (subsdk0+0x50fc4)
// and the context teardown at subsdk0+0x52e80 destroys that field unconditionally (`ldr x0, [x19, #0x4020]` at
// subsdk0+0x53314 -> the imported NvOsSemaphoreDestroy). The SDK's create (sdk+0x41d780) is
//
//     obj = NvOsAlloc(0x28);                          // sdk+0x41e550 -> the game's graphics allocator, i.e. the
//     if (!obj) return 6;                             // 512 MB nn::mem::StandardAllocator that main installs with
//     nn::os::InitializeSemaphore(obj, count, 0x7fffffff);   // nv::SetGraphicsAllocator (main+0x1322380)
//     *out = obj; return 0;
//
// so the create can fail, and when it does it returns an error *without writing *out*: ctx[0x4020] stays NULL and
// the teardown's FinalizeSemaphore(NULL) faults at data address 0x10. The same NULL teardown happens if the context
// init never ran (the teardown at subsdk0+0x52e80 does not check any state, unlike its callers at subsdk0+0x47090
// and +0x497b0) — both cases land at the ev0000->ev0005 transition, which is why the crash was intermittent.
//
// The runtime owns this layer now rather than leaving it entirely to the guest SDK:
//   * a create the SDK cannot satisfy gets a host semaphore (nv_os_sem.h), so a null handle never reaches the guest
//     and the guest sees success (the decoder keeps working);
//   * Destroy(NULL), and Signal/Wait on a NULL handle, are tolerated (never forwarded to the SDK, which would
//     fault there);
//   * every valid SDK handle still goes to the SDK implementation unchanged, so normal playback is untouched.
// NX_NV_SEM_TRACE=1 logs every create/destroy (one create per decoder, not per frame) and every failed allocation.

const bool g_nv_sem_trace = std::getenv("NX_NV_SEM_TRACE") != nullptr;

NvOsSemaphoreTable g_host_sems;  // the objects behind handles svc_nv handed out itself

void trace_alloc_failure(CPU& c, uint64_t size, uint64_t lr) {
    static std::atomic<int> logged{0};
    if (g_nv_sem_trace || logged.fetch_add(1) < 8) {
        log("nv: NvOsAlloc(%llu) returned NULL (lr %s): the guest graphics allocator is out of memory",
            (unsigned long long)size, describe(lr).c_str());
        if (g_nv_sem_trace) backtrace(c);  // who needed the memory
    }
}

// NvOsAlloc(size) is how the SDK allocates an NvOsSemaphore (0x28 bytes). It is only traced here — the guest
// implementation is still called, so nothing changes for the allocations that succeed.
void hle_nvos_alloc(CPU& c) {
    static const uint64_t real = find_symbol("NvOsAlloc");
    const uint64_t size = c.x[0], lr = c.x[30];
    if (!real) { c.x[0] = 0; return; }
    call(c, real);
    if (!c.x[0]) trace_alloc_failure(c, size, lr);
    c.x[30] = lr;
}

void hle_nvos_free(CPU& c) {
    static const uint64_t real = find_symbol("NvOsFree");
    if (real) call(c, real);
}

void hle_nvos_sem_create(CPU& c) {
    static const uint64_t real = find_symbol("NvOsSemaphoreCreate");
    const uint64_t out = c.x[0], lr = c.x[30];
    const uint32_t initial = uint32_t(c.x[1]);
    if (!real) {  // a build without the SDK export: satisfy the contract ourselves
        if (out) st<uint64_t>(out, g_host_sems.create(initial));
        c.x[0] = 0;
        return;
    }
    call(c, real);
    c.x[30] = lr;
    const uint32_t rc = uint32_t(c.x[0]);
    const uint64_t sem = out ? ld<uint64_t>(out) : 0;
    if (rc != 0 || (out && !sem)) {
        // The SDK create failed (or reported success without a handle) and left *out null. Back it with a host
        // semaphore and report success: the decoder gets a working handle instead of a null one, and the teardown
        // later destroys a valid object.
        const uint64_t h = g_host_sems.create(initial);
        if (out) st<uint64_t>(out, h);
        c.x[0] = 0;
        static std::atomic<int> logged{0};
        if (g_nv_sem_trace || logged.fetch_add(1) < 8) {
            log("nv: NvOsSemaphoreCreate(%u) failed in the SDK (rc %u, sem %llx); host semaphore %llx installed at "
                "%llx (lr %s)", initial, rc, (unsigned long long)sem, (unsigned long long)h,
                (unsigned long long)out, describe(lr).c_str());
            if (g_nv_sem_trace) backtrace(c);  // which decoder init could not get a semaphore
        }
    } else if (g_nv_sem_trace) {
        log("nv: NvOsSemaphoreCreate(%u) -> sem %llx (lr %s)", initial, (unsigned long long)sem,
            describe(lr).c_str());
    }
}

void hle_nvos_sem_destroy(CPU& c) {
    static const uint64_t real = find_symbol("NvOsSemaphoreDestroy");
    const uint64_t sem = c.x[0], lr = c.x[30];
    if (!sem) {
        static std::atomic<int> logged{0};
        if (g_nv_sem_trace || logged.fetch_add(1) < 8) {
            log("nv: NvOsSemaphoreDestroy(NULL) tolerated (a decoder context was torn down without a semaphore; "
                "lr %s)", describe(lr).c_str());
            if (g_nv_sem_trace) backtrace(c);  // which teardown path reached it
        }
        return;
    }
    if (g_host_sems.destroy(sem)) {
        if (g_nv_sem_trace) log("nv: NvOsSemaphoreDestroy(%llx) host semaphore released (lr %s)",
                                (unsigned long long)sem, describe(lr).c_str());
        c.x[0] = 0;
        return;
    }
    if (g_host_sems.was_destroyed(sem)) {  // a second teardown of one of ours: never hand the SDK a host pointer
        if (g_nv_sem_trace) log("nv: NvOsSemaphoreDestroy(%llx) already released (lr %s)",
                                (unsigned long long)sem, describe(lr).c_str());
        c.x[0] = 0;
        return;
    }
    if (g_nv_sem_trace) log("nv: NvOsSemaphoreDestroy(%llx) (lr %s)", (unsigned long long)sem, describe(lr).c_str());
    if (real) call(c, real);  // a valid SDK handle: behaviour unchanged
}

void hle_nvos_sem_signal(CPU& c) {
    static const uint64_t real = find_symbol("NvOsSemaphoreSignal");
    const uint64_t sem = c.x[0];
    if (auto hs = g_host_sems.find(sem)) { NvOsSemaphoreTable::signal(hs); c.x[0] = 0; return; }
    if (!sem || g_host_sems.was_destroyed(sem)) { c.x[0] = 0; return; }  // never the SDK with a host handle
    if (real) call(c, real);
}

void hle_nvos_sem_wait(CPU& c) {
    static const uint64_t real = find_symbol("NvOsSemaphoreWait");
    const uint64_t sem = c.x[0];
    // A host semaphore whose owner was torn down while we waited wakes with a failure (destroy() notifies waiters),
    // so a guest thread can never be left blocked on it.
    if (auto hs = g_host_sems.find(sem)) { c.x[0] = NvOsSemaphoreTable::wait(hs) ? 0 : 5; return; }
    if (!sem) { c.x[0] = 0; return; }
    if (g_host_sems.was_destroyed(sem)) { c.x[0] = 5; return; }
    if (real) call(c, real);
}

void hle_nvos_sem_wait_timeout(CPU& c) {
    static const uint64_t real = find_symbol("NvOsSemaphoreWaitTimeout");
    const uint64_t sem = c.x[0];
    const uint32_t ms = uint32_t(c.x[1]);
    if (auto hs = g_host_sems.find(sem)) { c.x[0] = NvOsSemaphoreTable::wait_timeout(hs, ms); return; }
    if (!sem) { c.x[0] = 5; return; }  // 5 = the SDK's timeout result
    if (g_host_sems.was_destroyed(sem)) { c.x[0] = 5; return; }
    if (real) call(c, real);
}

// NX_NV_SEM_HLE=0: do not register the family at all, so the loader binds subsdk0's imports straight to the guest
// SDK again (exactly the pre-fix behaviour). Bisect switch for anything that appears around a movie transition.
bool nvos_hle_enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("NX_NV_SEM_HLE");
        return !(e && e[0] == '0');
    }();
    return enabled;
}

struct NvOsHleBootstrap {
    NvOsHleBootstrap() {
        if (!nvos_hle_enabled()) {
            log("nv: NX_NV_SEM_HLE=0: NvOs* imports left to the guest SDK (fix disabled)");
            return;
        }
        static HleRegistrar reg_alloc("NvOsAlloc", hle_nvos_alloc);
        static HleRegistrar reg_free("NvOsFree", hle_nvos_free);
        static HleRegistrar reg_create("NvOsSemaphoreCreate", hle_nvos_sem_create);
        static HleRegistrar reg_destroy("NvOsSemaphoreDestroy", hle_nvos_sem_destroy);
        static HleRegistrar reg_signal("NvOsSemaphoreSignal", hle_nvos_sem_signal);
        static HleRegistrar reg_wait("NvOsSemaphoreWait", hle_nvos_sem_wait);
        static HleRegistrar reg_timeout("NvOsSemaphoreWaitTimeout", hle_nvos_sem_wait_timeout);
    }
};

static NvOsHleBootstrap g_nvos_hle_bootstrap;

}  // namespace

}  // namespace nx
