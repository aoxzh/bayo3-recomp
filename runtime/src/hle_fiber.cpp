// B3 SDK Fiber context bridge. The guest SDK owns state/TLS/alias-stack semantics;
// this bridge preserves the corresponding host C++ stacks. See BAYONETTA3_FIBER.md.
#include <nx/runtime.h>

#if defined(NX_BAYONETTA3)

// Linux/glibc provides ucontext and the bridge uses it there. macOS ships ucontext too, but marks it deprecated
// ("No longer supported") and it is broken in practice on Apple arm64: a swapcontext to a second makecontext
// context was measured resuming the *first* one, and the trampoline's entry branch sometimes arrived with a
// mangled function pointer (SIGSEGV inside _ctx_start, right after start_context()). The bridge therefore
// switches host stacks itself on Apple arm64 with a few lines of assembly that move only the ABI's callee-saved
// registers; the platform thread pointer (x28) is never touched, so thread_local keeps working exactly as it
// does across a normal call. macOS x86-64 (no PAC, working ucontext) and Linux keep ucontext. Android's bionic
// dropped the ucontext family outright (ucontext.h there defines the type but declares none of get/set/make/
// swapcontext), so arm64 Android takes the same own-switch path as Apple arm64: the code is ABI-identical and
// neither platform allows the thread pointer (x28 on Android, the same register on Darwin) to move.
//
// Windows has neither ucontext nor that asm: it ships the Win32 fiber API, which is the same primitive
// (CreateFiber/SwitchToFiber save and restore a continuation on its own stack) and is what the OS's own
// user-mode scheduling is built on. It is used directly, so nothing here relocates the thread pointer either:
// a Win32 fiber is still the same thread, and TLS -- including C++ thread_local -- is per-thread, so a guest
// fiber sees exactly the thread's TLS as it does on the other hosts.
#if defined(_WIN32)
#define NX_FIBER_WIN32 1
#endif
#if (defined(__APPLE__) || defined(__ANDROID__)) && defined(__aarch64__)
#define NX_FIBER_OWN_SWITCH 1
#endif
// The arm64 asm below spells symbols the way each object format does: Darwin prefixes them with an underscore, ELF
// does not.
#if defined(__APPLE__)
#define NX_HOST_SYM(name) "_" name
#else
#define NX_HOST_SYM(name) name
#endif
#if defined(NX_FIBER_WIN32)
#define NX_FIBER_SUPPORTED 1
#elif (defined(__linux__) && defined(__GLIBC__)) || defined(__APPLE__) || defined(NX_FIBER_OWN_SWITCH)
#define NX_FIBER_SUPPORTED 1
#endif
#if defined(NX_FIBER_WIN32)
#include <windows.h>
#elif defined(NX_FIBER_SUPPORTED)
#if defined(__APPLE__) && !defined(NX_FIBER_OWN_SWITCH) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700  // ucontext.h refuses to define its types without it on Darwin
#endif
#if !defined(NX_FIBER_OWN_SWITCH)
#include <ucontext.h>
#endif
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#if defined(NX_FIBER_OWN_SWITCH)
namespace nx {
// Host continuation in the terms the arm64 ABI defines it: the callee-saved GPRs, the stack pointer and the low
// 64 bits of v8-v15. x28 (the Darwin thread pointer, which is what keeps thread_local working) and the caller-saved
// registers are deliberately absent: a fiber runs on the guest thread and must see that thread's TLS.
struct HostCtx {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30;
    uint64_t sp;
    uint64_t d8, d9, d10, d11, d12, d13, d14, d15;
};
static_assert(sizeof(HostCtx) == 168, "nx_host_switch saves exactly this layout");
} // namespace nx
extern "C" void nx_host_switch(nx::HostCtx* from, nx::HostCtx* to);
extern "C" void nx_host_bootstrap();
extern "C" void nx_fiber_enter(void* context);
__asm__(
    ".text\n"
    ".p2align 2\n"
    // void nx_host_switch(HostCtx* from, HostCtx* to): save the live continuation into `from`, load `to`.
    ".globl " NX_HOST_SYM("nx_host_switch") "\n"
    NX_HOST_SYM("nx_host_switch") ":\n"
    ".cfi_startproc\n"
    "  stp x19, x20, [x0, #0]\n"
    "  stp x21, x22, [x0, #16]\n"
    "  stp x23, x24, [x0, #32]\n"
    "  stp x25, x26, [x0, #48]\n"
    "  stp x27, x28, [x0, #64]\n"
    "  stp x29, x30, [x0, #80]\n"
    "  mov x9, sp\n"
    "  str x9, [x0, #96]\n"
    "  stp d8, d9, [x0, #104]\n"
    "  stp d10, d11, [x0, #120]\n"
    "  stp d12, d13, [x0, #136]\n"
    "  stp d14, d15, [x0, #152]\n"
    "  mov x10, x1\n"
    "  ldp x19, x20, [x10, #0]\n"
    "  ldp x21, x22, [x10, #16]\n"
    "  ldp x23, x24, [x10, #32]\n"
    "  ldp x25, x26, [x10, #48]\n"
    "  ldp x27, x28, [x10, #64]\n"
    "  ldp x29, x30, [x10, #80]\n"
    "  ldr x9, [x10, #96]\n"
    "  ldp d8, d9, [x10, #104]\n"
    "  ldp d10, d11, [x10, #120]\n"
    "  ldp d12, d13, [x10, #136]\n"
    "  ldp d14, d15, [x10, #152]\n"
    "  mov sp, x9\n"
    "  ret\n"
    ".cfi_endproc\n"
    // First frame of a new continuation: x19 holds its Context*, nothing to return to.
    ".globl " NX_HOST_SYM("nx_host_bootstrap") "\n"
    NX_HOST_SYM("nx_host_bootstrap") ":\n"
    ".cfi_startproc\n"
    ".cfi_undefined lr\n"
    "  mov x0, x19\n"
    "  bl " NX_HOST_SYM("nx_fiber_enter") "\n"
    "  brk #0\n"
    ".cfi_endproc\n");
#endif

#if defined(NX_FIBER_SUPPORTED)
namespace nx {
namespace {
constexpr const char* kInitialize = "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi";
constexpr const char* kFinalize = "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE";
constexpr const char* kDispatch = "_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_";
constexpr uint64_t kFrameSize = 0xf0;
// 2 MiB was enough on Linux/glibc. Darwin's ucontext pushes larger trampoline frames, and the guest code a fiber
// runs recurses as deeply as a thread would (the guest threads themselves get 16 MiB, kernel.cpp
// create_main_thread), so macOS matches that. The stack is mmap'd and committed on demand, so only the touched pages
// occupy memory on either host.
#if defined(__APPLE__) || defined(_WIN32)
// Darwin's ucontext pushes larger trampoline frames, and the guest code a fiber runs recurses as deeply as a thread
// would (the guest threads themselves get 16 MiB, kernel.cpp create_main_thread), so macOS matches that. Windows
// fibers create their stack up front (CreateFiber), so the same 16 MiB is what the guest expects; only the pages
// actually touched are committed. On the POSIX hosts the stack is mmap'd and committed on demand too.
constexpr size_t kHostStackSize = 16 * 1024 * 1024;
#else
constexpr size_t kHostStackSize = 2 * 1024 * 1024;
#endif

struct Context {
#if defined(NX_FIBER_WIN32)
    void* host = nullptr;  // LPVOID from CreateFiber: the host continuation on its own stack
#elif defined(NX_FIBER_OWN_SWITCH)
    HostCtx host{};
#else
    ucontext_t host{};
#endif
    uint64_t slot = 0;
    uint64_t initial_frame = 0;
    uint64_t initial_entry = 0;
    CPU* cpu = nullptr;
    std::thread::id owner{};
    uint64_t tpidr = 0, tpidrro = 0;
    bool root = false;
    bool started = false;
#if defined(NX_FIBER_WIN32)
    // The Win32 fiber API owns its stack, so there is no separate mapping to track: DeleteFiber frees the stack it
    // was created with (the fiber must not be running, which the caller guarantees by dispatching away from it).
    // The root fiber is the thread itself, converted on the first dispatch (ConvertThreadToFiber); it must be turned
    // back into a thread with ConvertFiberToThread, not deleted -- DeleteFiber would free the thread's own stack.
    ~Context() {
        if (!host) return;
        if (root)
            ConvertFiberToThread();
        else
            DeleteFiber(host);
    }
#else
    void* allocation = MAP_FAILED;
    size_t allocation_size = 0;
    ~Context() {
        if (allocation != MAP_FAILED) munmap(allocation, allocation_size);
    }
#endif
};

std::mutex contexts_lock;
std::unordered_map<uint64_t, std::unique_ptr<Context>> contexts;
thread_local Context* active = nullptr;
thread_local bool logged_first_dispatch = false;

void original(CPU& c, const char* symbol) {
    // find_symbol is the original module export, not its HLE relocation thunk.
    uint64_t entry = find_symbol(symbol);
    if (!entry) fatal("Fiber: original SDK export missing: %s", symbol);
    call(c, entry);
}

void check_owner(const Context& fiber, CPU& c) {
    if (fiber.started && (fiber.owner != std::this_thread::get_id() || fiber.cpu != &c ||
                          fiber.tpidr != c.tpidr || fiber.tpidrro != c.tpidrro))
        fatal("Fiber: cross-thread/context migration unsupported (slot %llx): owner %s cpu %s (%p vs %p) "
              "tpidr %s tpidrro %s",
              (unsigned long long)fiber.slot,
              fiber.owner == std::this_thread::get_id() ? "same" : "DIFFERENT",
              fiber.cpu == &c ? "same" : "DIFFERENT", (void*)fiber.cpu, (void*)&c,
              fiber.tpidr == c.tpidr ? "same" : "DIFFERENT",
              fiber.tpidrro == c.tpidrro ? "same" : "DIFFERENT");
}

void load_frame(CPU& c, uint64_t frame) {
    c.x[2] = frame;
    c.x[0] = ld<uint64_t>(frame);
    for (int k = 19; k <= 30; ++k) c.x[k] = ld<uint64_t>(frame + 8 * (k - 18));
    for (int k = 8; k <= 15; ++k) c.v[k] = ld<V128>(frame + 0x70 + 16 * (k - 8));
    c.x[31] = frame + kFrameSize;
}

void save_frame(CPU& c, uint64_t slot) {
    uint64_t frame = c.x[31] - kFrameSize;
    st<uint64_t>(frame, 0);
    for (int k = 19; k <= 30; ++k) st<uint64_t>(frame + 8 * (k - 18), c.x[k]);
    st<uint64_t>(frame + 0x68, 0);
    for (int k = 8; k <= 15; ++k) st<V128>(frame + 0x70 + 16 * (k - 8), c.v[k]);
    st<uint64_t>(slot, frame);
}

void fiber_entry(Context& fiber) {
    // All fibers on a guest thread use its one stable CPU object, including references already held by generated
    // code. On Linux this runs through makecontext; on Apple arm64 nx_host_bootstrap loads this Context* from x19.
    CPU& c = *fiber.cpu;
    try {
        call(c, fiber.initial_entry);
    } catch (const LongJmp& e) {
        fatal("Fiber: longjmp escaped fiber (buffer %llx)", (unsigned long long)e.buf);
    } catch (const std::exception& e) {
        fatal("Fiber: host exception escaped fiber: %s", e.what());
    } catch (...) {
        fatal("Fiber: unknown exception escaped fiber");
    }
    fatal("Fiber: SDK entry returned instead of dispatching to its next fiber");
}

void start_context(Context& fiber, CPU& c) {
#if defined(NX_FIBER_WIN32)
    // CreateFiber takes the reservation size up front and commits pages as they are touched, which is exactly the
    // mmap(PROT_NONE)+mprotect behaviour of the POSIX hosts. The parameter is a void*, so the Context pointer is
    // handed over directly; the trampoline runs on the new stack and needs nothing else to find its fiber.
    fiber.host = CreateFiber(kHostStackSize, [](void* parameter) { fiber_entry(*static_cast<Context*>(parameter)); },
                             &fiber);
    if (!fiber.host) fatal("Fiber: CreateFiber failed (%lu)", (unsigned long)GetLastError());
#else
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) fatal("Fiber: cannot determine host page size");
    fiber.allocation_size = kHostStackSize + 2 * size_t(page);
    fiber.allocation = mmap(nullptr, fiber.allocation_size, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fiber.allocation == MAP_FAILED) fatal("Fiber: host stack mmap failed (%d)", errno);
    auto* stack = static_cast<char*>(fiber.allocation) + page;
    if (mprotect(stack, kHostStackSize, PROT_READ | PROT_WRITE))
        fatal("Fiber: host stack mprotect failed (%d)", errno);
#if defined(NX_FIBER_OWN_SWITCH)
    std::memset(&fiber.host, 0, sizeof(fiber.host));
    fiber.host.sp = (reinterpret_cast<uint64_t>(stack) + kHostStackSize) & ~uint64_t(15);
    fiber.host.x30 = reinterpret_cast<uint64_t>(&nx_host_bootstrap);
    fiber.host.x19 = reinterpret_cast<uint64_t>(&fiber);  // read by nx_host_bootstrap
#else
    if (getcontext(&fiber.host)) fatal("Fiber: getcontext failed (%d)", errno);
    fiber.host.uc_stack.ss_sp = stack;
    fiber.host.uc_stack.ss_size = kHostStackSize;
    fiber.host.uc_stack.ss_flags = 0;
    fiber.host.uc_link = nullptr;
    makecontext(&fiber.host, [] { fiber_entry(*active); }, 0);
#endif
#endif
    fiber.owner = std::this_thread::get_id();
    fiber.cpu = &c;
    fiber.tpidr = c.tpidr;
    fiber.tpidrro = c.tpidrro;
    fiber.started = true;
}

void initialize(CPU& c) {
    const uint64_t object = c.x[0], slot = object + 0x50;
    {
        std::lock_guard lock(contexts_lock);
        auto it = contexts.find(slot);
        if (it != contexts.end()) {
            check_owner(*it->second, c);
            if (it->second.get() == active || it->second->root)
                fatal("Fiber: cannot initialize active/root fiber %llx", (unsigned long long)object);
            // Reinitialization cancels the old suspended continuation. No host RAII
            // objects may require unwinding across a guest cooperative yield.
            contexts.erase(it);
        }
    }
    original(c, kInitialize);
    auto fiber = std::make_unique<Context>();
    fiber->slot = slot;
    fiber->initial_frame = ld<uint64_t>(slot);
    fiber->initial_entry = ld<uint64_t>(fiber->initial_frame + 0x60);
    if (!fiber->initial_entry || (fiber->initial_frame & 15))
        fatal("Fiber: invalid initialized frame for %llx", (unsigned long long)object);
    // Ownership is recorded here, at initialize time, not at the first dispatch: the SDK initializes a fiber on the
    // thread that will run it, and every later check_owner() compares against these. Leaving them unset (cpu nullptr,
    // started false) made the first dispatch of a fiber skip the ownership check on Linux by luck of the `started`
    // guard, but fail as "cpu DIFFERENT (0x0 vs ...)" as soon as a platform actually got that far.
    fiber->cpu = &c;
    fiber->owner = std::this_thread::get_id();
    fiber->tpidr = c.tpidr;
    fiber->tpidrro = c.tpidrro;
    std::lock_guard lock(contexts_lock);
    if (!contexts.emplace(slot, std::move(fiber)).second)
        fatal("Fiber: concurrent initialization of %llx", (unsigned long long)object);
}

void finalize(CPU& c) {
    const uint64_t slot = c.x[0] + 0x50;
    {
        std::lock_guard lock(contexts_lock);
        auto it = contexts.find(slot);
        if (it != contexts.end()) {
            check_owner(*it->second, c);
            if (it->second.get() == active || it->second->root)
                fatal("Fiber: cannot finalize active/root fiber (slot %llx)", (unsigned long long)slot);
            contexts.erase(it);
        }
    }
    original(c, kFinalize); // Includes the SDK's alias unmap and guest state reset.
}

void dispatch(CPU& c) {
    if (std::uncaught_exceptions()) fatal("Fiber: switch during host exception unwinding unsupported");
    const uint64_t from = c.x[0], to = c.x[1];
    if (!from || !to || from == to || &c != &current_cpu())
        fatal("Fiber: invalid dispatch slots or CPU identity");
    Context* previous;
    Context* next;
    {
        std::lock_guard lock(contexts_lock);
        if (!active) {
            // The first SDK SwitchToFiber creates a temporary root Fiber on the
            // guest stack. Its C++ continuation is our existing host stack.
            uint64_t thread = ld<uint64_t>(c.tpidrro + 0x1f8);
            if (!thread || ld<uint64_t>(thread + 0x78) + 0x50 != from)
                fatal("Fiber: dispatch source is not the SDK root");
            auto root = std::make_unique<Context>();
            root->slot = from;
            root->cpu = &c;
            root->owner = std::this_thread::get_id();
            root->tpidr = c.tpidr;
            root->tpidrro = c.tpidrro;
            root->root = root->started = true;
#if defined(NX_FIBER_WIN32)
            // The guest's root fiber runs on the host thread's own stack, so the thread has to become a fiber before
            // anything can switch back to it. NULL means "this thread already is a fiber", which is not an error here:
            // the same continuation is then the one GetCurrentFiber() reports.
            root->host = ConvertThreadToFiber(nullptr);
            if (!root->host) {
                if (GetLastError() != ERROR_ALREADY_FIBER)
                    fatal("Fiber: ConvertThreadToFiber failed (%lu)", (unsigned long)GetLastError());
                root->host = GetCurrentFiber();
            }
#endif
            active = root.get();
            if (!contexts.emplace(from, std::move(root)).second)
                fatal("Fiber: stale root context (slot %llx)", (unsigned long long)from);
        }
        previous = active;
        check_owner(*previous, c);
        if (previous->slot != from) fatal("Fiber: source does not match active host continuation");
        auto it = contexts.find(to);
        if (it == contexts.end())
            fatal("Fiber: target was not initialized through Fiber HLE (slot %llx)", (unsigned long long)to);
        next = it->second.get();
        check_owner(*next, c);
        if (!next->started) {
            if (ld<uint64_t>(to) != next->initial_frame ||
                ld<uint64_t>(next->initial_frame + 0x60) != next->initial_entry)
                fatal("Fiber: initial guest frame changed before first dispatch");
            start_context(*next, c);
        }
    } // Never keep a lock or owning smart pointer on a suspended host stack.
    static const bool trace = std::getenv("NX_FIBER_TRACE") != nullptr;
    if (trace)
        log("Fiber: dispatch %llx -> %llx (host %p -> %p), sp %llx, lr %llx",
            (unsigned long long)(from - 0x50), (unsigned long long)(to - 0x50), (void*)previous,
            (void*)next, (unsigned long long)c.x[31], (unsigned long long)c.x[30]);
    if (!logged_first_dispatch) {
        log("Fiber: host context dispatch from %llx to %llx, tls %llx, sp %llx, lr %llx",
            (unsigned long long)(from - 0x50), (unsigned long long)(to - 0x50),
            (unsigned long long)c.tpidrro, (unsigned long long)c.x[31], (unsigned long long)c.x[30]);
        logged_first_dispatch = true;
    }
    save_frame(c, from);
    load_frame(c, ld<uint64_t>(to));
    active = next;
#if defined(NX_FIBER_WIN32)
    SwitchToFiber(next->host);  // returns here when some other fiber switches back to us
#elif defined(NX_FIBER_OWN_SWITCH)
    nx_host_switch(&previous->host, &next->host);
#else
    if (swapcontext(&previous->host, &next->host)) fatal("Fiber: swapcontext failed (%d)", errno);
#endif
    if (active != previous) fatal("Fiber: resumed wrong host continuation");
    // The incoming dispatch restored guest registers before resuming us. Do not
    // overwrite them with a stale whole-CPU copy (TLS and FP state are thread state).
    if (previous->root) {
        active = nullptr;
        std::lock_guard lock(contexts_lock);
        contexts.erase(from); // SDK now clears thread+0x70/+0x78 and returns.
    }
}

NX_HLE(kInitialize, initialize);
NX_HLE(kFinalize, finalize);
NX_HLE(kDispatch, dispatch);
} // namespace

#if defined(NX_FIBER_OWN_SWITCH)
// nx_host_bootstrap branches here with x19 = Context* (see the assembler block above).
extern "C" void nx_fiber_enter(void* context) { fiber_entry(*static_cast<Context*>(context)); }
#endif
} // namespace nx

#else
namespace nx {
namespace {
void unsupported_fiber(CPU&) { fatal("B3 Fiber host contexts need ucontext (Linux glibc, macOS, Win32 fibers) on this platform"); }
NX_HLE("_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi", unsupported_fiber);
NX_HLE("_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE", unsupported_fiber);
NX_HLE("_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_", unsupported_fiber);
} // namespace
} // namespace nx
#endif
#endif
