// Requires private generated_fiber_test.inc, produced by bayonetta3_fiber_test.py.
// Uses the production bridge, not a second implementation of context switching.
#include <nx/runtime.h>
#include <sys/mman.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <array>

constexpr uint64_t NXB = 0x4010000000ull;
constexpr const char* init_name = "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi";
constexpr const char* fini_name = "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE";
constexpr const char* dispatch_name = "_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_";
namespace {
auto& registry() { static std::unordered_map<std::string,nx::HleFn> r; return r; }
thread_local nx::CPU cpu{};
}
namespace nx {
HleRegistrar::HleRegistrar(const char* s,HleFn f) { registry()[s]=f; }
CPU& current_cpu() { return cpu; }
uint64_t find_symbol(const char* s) {
    if (!std::strcmp(s,init_name)) return NXB+0x379220;
    if (!std::strcmp(s,fini_name)) return NXB+0x3792f0;
    return 0;
}
[[noreturn]] void fatal(const char* fmt,...) {
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);fputc('\n',stderr);std::exit(90);
}
void log(const char*,...) {}
}
#include "../runtime/src/hle_fiber.cpp"
#include "generated_fiber_decls.inc"
#include "generated_fiber_test.inc"
void sj_00000200(nx::CPU&);
void sj_00000300(nx::CPU&);
#include "generated_fiber_setjmp.inc"

uint64_t address(void* p) { return reinterpret_cast<uint64_t>(p); }
alignas(16) unsigned char tls[0x1000]{}, thread_obj[0x200]{}, sdk_state[0x1000]{};
alignas(16) unsigned char fiber_a[0x58]{}, fiber_b[0x58]{};
alignas(4096) unsigned char a_stack[0x10000]{}, b_stack[0x10000]{}, root_stack[0x10000]{};
void* alias_area;
int map_calls=0,unmap_calls=0,callback_runs=0;
bool no_address=false, mapping_failure=false, rejection=false;
std::string events;

void invoke(const char* name) { registry().at(name)(cpu); }
void switch_to(void* fiber) { cpu.x[0]=address(fiber);sdk_00379330(cpu); }
void initialize(void* fiber,void* stack,uint64_t callback,uint64_t arg,int flags=1) {
    cpu.x[0]=address(fiber);cpu.x[1]=callback;cpu.x[2]=arg;cpu.x[3]=address(stack);cpu.x[4]=0x10000;cpu.x[5]=flags;invoke(init_name);
}
void finalize(void* fiber) { cpu.x[0]=address(fiber);invoke(fini_name); }

// Extracted production MapMemory/UnmapMemory switch cases use these host boundaries.
namespace nx {
constexpr uint64_t kTestStackBase = 0x700000000000ull, kTestStackSize=0x100000;
constexpr int kTestMemStack=1,kTestMemAlias=2,kTestMemFree=0;
bool fiber_test_commit(uint64_t p,uint64_t n) { return mprotect(reinterpret_cast<void*>(p),n,PROT_READ|PROT_WRITE)==0; }
void fiber_test_decommit(uint64_t p,uint64_t n) { assert(mprotect(reinterpret_cast<void*>(p),n,PROT_NONE)==0); }
void fiber_test_vm_set(uint64_t,uint64_t,int,int) {}
#include "generated_fiber_map_cases.inc"
void svc(CPU& c,uint32_t imm,uint64_t) {
    if (imm==4) { ++map_calls; if (mapping_failure) {c.x[0]=0xd401;return;} }
    else if(imm==5) ++unmap_calls;
    else std::abort();
    fiber_test_memory_svc(c,imm);
}
}

void sdk_005C9A90(nx::CPU& c) { sdk_0037D29C(c); }
void sdk_005C9AA0(nx::CPU& c) { sdk_0037D284(c); }
void sdk_005C9AB0(nx::CPU& c) { sdk_0037D3B8(c); }
void sdk_005C9AC0(nx::CPU& c) { sdk_0037945C(c); }
void sdk_005C9AD0(nx::CPU& c) { sdk_0037D304(c); }
void sdk_005C9AE0(nx::CPU& c) { registry().at(dispatch_name)(c); }
void sdk_005B22B0(nx::CPU&) { nx::fatal("SDK Fiber AbortImpl"); }
void sdk_005C9DA0(nx::CPU& c) { sdk_0037AA34(c); }
// Deterministic replacements for random virtual-address selection and guard-range
// validation; actual StackGuardManager retry/map/unmap flow above is generated SDK.
void sdk_005C9CE0(nx::CPU& c) { c.x[0]=no_address?0:address(alias_area); }
void sdk_005C9D00(nx::CPU& c) { c.x[0]=rejection?0:1; }

struct CalleeSaved {
    std::array<uint64_t,12> x;
    std::array<nx::V128,8> q;
    CalleeSaved() { for(int k=0;k<12;++k)x[k]=cpu.x[k+19];for(int k=0;k<8;++k)q[k]=cpu.v[k+8]; }
    void restore() { for(int k=0;k<12;++k)cpu.x[k+19]=x[k];for(int k=0;k<8;++k)cpu.v[k+8]=q[k]; }
};
void callback_a() {
    CalleeSaved saved;
    assert(cpu.x[0]==0xaabb);events+='A';sdk_00379470(cpu);assert(cpu.x[0]==address(fiber_a));
    for(int k=19;k<=29;++k)cpu.x[k]=0x12340000+k;
    for(int k=8;k<=15;++k){cpu.v[k].u64[0]=0x1000+k;cpu.v[k].u64[1]=0x2000+k;}
    int local=73; switch_to(nullptr);
    assert(local==73);
    for(int k=19;k<=29;++k)assert(cpu.x[k]==0x12340000+unsigned(k));
    for(int k=8;k<=15;++k){assert(cpu.v[k].u64[0]==0x1000+unsigned(k));assert(cpu.v[k].u64[1]==0x2000+unsigned(k));}
    events+='a';switch_to(fiber_b);events+='C';saved.restore();cpu.x[0]=0;
}
void callback_b() { CalleeSaved saved;assert(cpu.x[0]==0xbbcc);events+='B';saved.restore();cpu.x[0]=address(fiber_a); }
void callback_suspend() { CalleeSaved saved;++callback_runs;switch_to(nullptr);nx::fatal("cancelled continuation resumed"); }
void callback_complete() { ++callback_runs;cpu.x[0]=0; }
void sj_00000200(nx::CPU&) { switch_to(nullptr); }
void sj_00000300(nx::CPU& c) { throw nx::LongJmp{c.x[0],uint32_t(c.x[1])}; }
void callback_setjmp() {
    sj_00000000(cpu);assert(cpu.x[0]==7);cpu.x[0]=0;
}
void callback_fp() {
    // Guest dispatch does not save FPCR/FPSR, NZCV, TPIDR or caller-saved regs.
    // Those fields remain on the stable CPU and reflect the most recent writer.
    CalleeSaved saved;cpu.fpcr=0x400000;cpu.fpsr=0x10;switch_to(nullptr);
    assert(cpu.fpcr==0x800000 && cpu.fpsr==0x20);saved.restore();cpu.x[0]=0;
}
namespace nx {
void call(CPU& c,uint64_t target) {
    switch(target-NXB) {
    case 0x379220:sdk_00379220(c);return;
    case 0x3792f0:sdk_003792F0(c);return;
    case 0x379290:sdk_00379290(c);return;
    case 0x37d320:sdk_0037D320(c);return;
    case 0x37d33c:sdk_0037D33C(c);return;
    }
    if(target==0x11110000)callback_a();else if(target==0x22220000)callback_b();
    else if(target==0x33330000)callback_suspend();else if(target==0x44440000)callback_complete();
    else if(target==0x55550000)callback_fp();else if(target==0x66660000)callback_setjmp();
    else fatal("test unknown target %llx",(unsigned long long)target);
}
}
void setup() {
    cpu.tpidrro=address(tls);cpu.tpidr=0xabc000;
    nx::st<uint64_t>(address(tls)+0x1f8,address(thread_obj));
    cpu.x[31]=address(root_stack)+sizeof(root_stack);cpu.x[30]=0xfeed0000;
    void* got=mmap(reinterpret_cast<void*>(NXB+0xce7000),4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(got!=MAP_FAILED);nx::st<uint64_t>(NXB+0xce7368,address(sdk_state));
    alias_area=mmap(nullptr,0x10000,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(alias_area!=MAP_FAILED);
}
void root_clean() {
    assert(nx::active==nullptr);assert(nx::ld<uint64_t>(address(thread_obj)+0x70)==0);assert(nx::ld<uint64_t>(address(thread_obj)+0x78)==0);
    for(auto& [slot,ctx]:nx::contexts) assert(!ctx->root);
    assert(cpu.x[31]==address(root_stack)+sizeof(root_stack));
}
int main(int argc,char** argv) {
    setup();
    initialize(fiber_a,a_stack,0x11110000,0xaabb);initialize(fiber_b,b_stack,0x22220000,0xbbcc);
    switch_to(fiber_a);root_clean();events+='R';switch_to(fiber_a);root_clean();assert(events=="ARaBC");
    assert(fiber_a[0x20]==4 && fiber_b[0x20]==4);finalize(fiber_a);finalize(fiber_b);assert(nx::contexts.empty());
    puts("PASS production HLE: generated SDK root/A/B, callback next/null, all x19-x29/q8-q15, root cleanup");
    for(int round=0;round<32;++round) {
        initialize(fiber_a,a_stack,0x33330000,0);switch_to(fiber_a);root_clean();
        if(argc>1 && std::string(argv[1])=="cross-thread") {
            std::thread other([] {cpu.x[0]=address(fiber_a);invoke(fini_name);});other.join();std::abort();
        }
        if(argc>1 && std::string(argv[1])=="cross-resume") {
            nx::CPU snapshot=cpu;
            std::thread other([snapshot] {cpu=snapshot;switch_to(fiber_a);});other.join();std::abort();
        }
        if(round%2==0)finalize(fiber_a); // Other rounds exercise direct same-address Initialize cancellation.
        initialize(fiber_a,a_stack,0x44440000,0);switch_to(fiber_a);root_clean();finalize(fiber_a);
        assert(nx::contexts.empty());
    }
    assert(callback_runs==64);puts("PASS production HLE: suspended finalize/reinitialize and same-address reuse, 64 callbacks, no sidecars remain");
    initialize(fiber_a,a_stack,0x55550000,0);switch_to(fiber_a);root_clean();assert(cpu.fpcr==0x400000 && cpu.fpsr==0x10);
    cpu.fpcr=0x800000;cpu.fpsr=0x20;switch_to(fiber_a);root_clean();finalize(fiber_a);
    puts("PASS guest FPCR/FPSR remain thread state across SDK dispatch");
    initialize(fiber_a,a_stack,0x66660000,0);switch_to(fiber_a);root_clean();switch_to(fiber_a);root_clean();finalize(fiber_a);
    puts("PASS production emitter setjmp frame survives yield and same-fiber LongJmp resumes with value 7");
    a_stack[3]=0x77;initialize(fiber_a,a_stack,0x44440000,0,0);
    assert(map_calls==1 && fiber_a[0x21]==1);assert(nx::ld<uint64_t>(address(fiber_a)+0x40)==address(alias_area));
    assert(static_cast<unsigned char*>(alias_area)[3]==0x77);switch_to(fiber_a);root_clean();
    static_cast<unsigned char*>(alias_area)[3]=0x55;finalize(fiber_a);
    assert(unmap_calls==1 && a_stack[3]==0x55 && fiber_a[0x21]==0);assert(nx::contexts.empty());
    puts("PASS flags=0: real generated MapAliasStack/StackGuardManager/SVC wrappers + extracted production map/unmap cases, copyback");
    no_address=true;initialize(fiber_a,a_stack,0x44440000,0,0);assert(!fiber_a[0x21]);switch_to(fiber_a);root_clean();finalize(fiber_a);no_address=false;
    mapping_failure=true;int before=map_calls;initialize(fiber_a,a_stack,0x44440000,0,0);assert(map_calls-before==64 && !fiber_a[0x21]);finalize(fiber_a);mapping_failure=false;
    rejection=true;before=unmap_calls;initialize(fiber_a,a_stack,0x44440000,0,0);assert(unmap_calls-before==64 && !fiber_a[0x21]);finalize(fiber_a);rejection=false;
    assert(nx::contexts.empty());puts("PASS alias no-address, injected map failure (64 retries), rejected range unmap/retry (64), fallback and finalize");
}
