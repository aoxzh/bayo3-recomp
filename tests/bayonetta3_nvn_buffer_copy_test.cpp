// Real HLE recorder and shared written tracking; fake Vulkan queues operations
// until run_gpu(), making premature CPU synchronization observable.
#include "../runtime/src/nvn/nvn_buffer_copy.h"
#include "../runtime/src/nvn/nvn_draw_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>

using namespace nx;
using namespace nx::nvn;
namespace {
std::map<std::string, HleFn>& registry() { static std::map<std::string, HleFn> r; return r; }
std::vector<std::function<void()>> gpu;
std::vector<std::unique_ptr<std::vector<uint8_t>>> allocations;
bool allow_kept_allocations = false;  // enabled only by the dirty-learning regression
unsigned kept_allocations = 0;
uint8_t* pending_kept = nullptr;
uint64_t pending_kept_size = 0;
std::vector<char> events;
struct Barrier { VkPipelineStageFlags src, dst; VkAccessFlags read, write; };
std::vector<Barrier> barriers;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
uint64_t invoke(const char* name, uint64_t a=0, uint64_t b=0, uint64_t c=0, uint64_t d=0, uint64_t e=0) {
    CPU cpu{}; cpu.x[0]=a;cpu.x[1]=b;cpu.x[2]=c;cpu.x[3]=d;cpu.x[4]=e;
    registry().at(name)(cpu); return cpu.x[0];
}
void run_gpu() { for (auto& fn:gpu) fn(); gpu.clear(); }
void VKAPI_PTR copy(VkCommandBuffer,VkBuffer src,VkBuffer dst,uint32_t count,const VkBufferCopy* ranges) {
    events.push_back('C');
    for (uint32_t i=0;i<count;++i) {
        auto r=ranges[i];
        check(r.size && (src!=dst || r.srcOffset+r.size<=r.dstOffset || r.dstOffset+r.size<=r.srcOffset),"Vulkan copy must not overlap");
        gpu.emplace_back([=]{std::memcpy(reinterpret_cast<uint8_t*>(dst)+r.dstOffset,reinterpret_cast<uint8_t*>(src)+r.srcOffset,r.size);});
    }
}
void VKAPI_PTR barrier(VkCommandBuffer,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags,uint32_t n,const VkMemoryBarrier* b,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*) {
    events.push_back('B');
    check(src && dst,"barrier stages");
    barriers.push_back({src,dst,n?b[0].srcAccessMask:0,n?b[0].dstAccessMask:0});
}
void VKAPI_PTR properties(VkPhysicalDevice,VkPhysicalDeviceProperties* p) {
    p->limits.minStorageBufferOffsetAlignment=256;p->limits.minUniformBufferOffsetAlignment=256;
}
template<class F> void rejects(F fn) {
    auto n=gpu.size(), e=events.size(); auto binds=replay::written_bind_count();
    bool threw=false;try{fn();}catch(const std::runtime_error&){threw=true;}
    check(threw,"invalid input rejected");check(n==gpu.size() && e==events.size() && binds==replay::written_bind_count(),"reject is atomic before GPU work");
}
Cmd command(uint64_t src,uint64_t dst,uint64_t bytes,uint32_t flags=0) {
    Cmd c{Cmd::CopyBufferToBuffer};c.a[0]=src;c.a[1]=dst;c.a[2]=bytes;c.n=flags;return c;
}
void replay_copy(uint64_t s,uint64_t d,uint64_t n) { replay_buffer_copy(VK_NULL_HANDLE,command(s,d,n)); }
// Production's write-back entry point takes the batch (write_backs_apply); one range is a batch of one.
void apply_write_back(WriteBack w) { std::vector<WriteBack> v{std::move(w)}; write_backs_apply(v); }
void finish() { auto writes=replay::written_take();run_gpu();for(auto& w:writes)apply_write_back(w); }
MemoryPool& pool(uint64_t key,uint64_t gpu_address,std::vector<uint8_t>& memory) {
    auto& p=make<MemoryPool>(key);p.gpu=gpu_address;p.size=memory.size();p.memory=reinterpret_cast<uint64_t>(memory.data());return p;
}
void queued_write(const uint8_t* p,uint64_t n,uint8_t value) {
    auto u=replay::written_bind(VK_NULL_HANDLE,p,n,n);
    gpu.emplace_back([=]{std::memset(u.ptr,value,n);});
}
}
namespace nx::plat {
uint64_t monotonic_ns(){return 0;}
bool late_guard_on(){return false;} uint64_t late_guard_add(uint64_t,uint64_t){return 0;} uint32_t late_guard_open(LateGuardEvent*,uint32_t){return 0;}
void set_thread_name(const char*){}
}
namespace nx {
HleRegistrar::HleRegistrar(const char* n,HleFn f){check(registry().emplace(n,f).second,"duplicate HLE");}
void log(const char*,...) {}
[[noreturn]] void fatal(const char*,...){throw std::runtime_error("production fatal");}
std::string describe(uint64_t){return "test";}
}
namespace nx::vk {
PFN_vkCmdCopyBuffer p_vkCmdCopyBuffer=copy;
PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier=::barrier;
PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties=properties;
// A real host allocation makes a falsely learned whole range observable in
// the regression. Other checks keep the original null-allocation fallback.
VkResult VKAPI_PTR create_buffer(VkDevice,VkBufferCreateInfo const* info,VkAllocationCallbacks const*,VkBuffer* b){
    *b=VK_NULL_HANDLE;
    if(allow_kept_allocations){
        ++kept_allocations;
        auto data=std::make_unique<std::vector<uint8_t>>(info->size);
        pending_kept=data->data();pending_kept_size=info->size;
        *b=reinterpret_cast<VkBuffer>(pending_kept);allocations.push_back(std::move(data));
    }
    return VK_SUCCESS;
}
void VKAPI_PTR buffer_requirements(VkDevice,VkBuffer,VkMemoryRequirements* r){*r={};r->size=pending_kept_size;r->memoryTypeBits=1;}
VkResult VKAPI_PTR alloc_memory(VkDevice,VkMemoryAllocateInfo const*,VkAllocationCallbacks const*,VkDeviceMemory* m){*m=reinterpret_cast<VkDeviceMemory>(pending_kept);return VK_SUCCESS;}
VkResult VKAPI_PTR bind_memory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize){return VK_SUCCESS;}
VkResult VKAPI_PTR map_memory(VkDevice,VkDeviceMemory memory,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void** p){*p=reinterpret_cast<void*>(memory);return VK_SUCCESS;}
void VKAPI_PTR destroy_buffer(VkDevice,VkBuffer,VkAllocationCallbacks const*){}
void VKAPI_PTR free_memory(VkDevice,VkDeviceMemory,VkAllocationCallbacks const*){}
void VKAPI_PTR unmap_memory(VkDevice,VkDeviceMemory){}
void VKAPI_PTR memory_properties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties* props){
    *props={};props->memoryTypeCount=1;
    props->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
PFN_vkCreateBuffer p_vkCreateBuffer=create_buffer;
PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements=buffer_requirements;
PFN_vkAllocateMemory p_vkAllocateMemory=alloc_memory;
PFN_vkBindBufferMemory p_vkBindBufferMemory=bind_memory;
PFN_vkMapMemory p_vkMapMemory=map_memory;
PFN_vkDestroyBuffer p_vkDestroyBuffer=destroy_buffer;
PFN_vkFreeMemory p_vkFreeMemory=free_memory;
PFN_vkUnmapMemory p_vkUnmapMemory=unmap_memory;
PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties=memory_properties;
}
namespace nx::nvn {
ObjectsLock& objects_lock(){static ObjectsLock l;return l;}
std::unordered_map<uint64_t,std::unique_ptr<Object>>& objects(){static std::unordered_map<uint64_t,std::unique_ptr<Object>> o;return o;}
bool resolve_cache_on(){return false;}
Object* find_object(uint64_t g,uint64_t){auto i=objects().find(g);return i==objects().end()?nullptr:i->second.get();}
void defer_object(std::unique_ptr<Object>){}
uint64_t current_epoch(){return 1;}
const std::vector<const ShaderInfo*>* program_shaders(uint64_t){return nullptr;}
ViewInfo read_view(CPU&,uint64_t){return {};}
MemoryPool* pool_at_gpu(uint64_t g,uint64_t* off){for(auto& [_,o]:objects())if(auto* p=dynamic_cast<MemoryPool*>(o.get());p && g>=p->gpu && g-p->gpu<p->size){*off=g-p->gpu;return p;}return nullptr;}
Upload upload_alloc(uint64_t n,uint64_t){auto a=std::make_unique<std::vector<uint8_t>>(n);auto* p=a->data();allocations.push_back(std::move(a));return {reinterpret_cast<VkBuffer>(p),0,p};}
VkPhysicalDevice vk_physical(){return VK_NULL_HANDLE;}
VkDevice vk_device(){return VK_NULL_HANDLE;}
uint32_t memory_type(uint32_t,VkMemoryPropertyFlags){return 0;}
Upload host_import_upload(const void*,uint64_t,uint64_t){return {};}
bool vk_host_import(){return false;}
bool async_gpu(){return allow_kept_allocations;}
void frame_wait_all(){run_gpu();}
void frame_carry_wait(){}
void draw_end_rendering(VkCommandBuffer,int){events.push_back('E');}
void clears_flush(VkCommandBuffer){events.push_back('F');}
// Retained by the production recorder's other registered handlers, unused here.
uint64_t checked_scratch_address(uint64_t,uint64_t,uint64_t){return 0;}
namespace replay {
void dedup_invalidate(const uint8_t*,uint64_t){}
Upload dedup_upload(const uint8_t* p,uint64_t n,uint64_t padded,uint64_t align, bool){auto a=upload_alloc(padded,align);std::memcpy(a.ptr,p,n);return a;}
// No dispatch is replayed here, so production's NX_LAZY_COMPUTE_BARRIER function is this no-op.
void compute_write_barrier(VkCommandBuffer){}
void trace_nan_write_back(const uint8_t*,const uint8_t*,uint64_t){}
}
}

// NX_REPLAY_CUTS (nvn_written.cpp): a read that is not contained in a written range is "composed" -- a copy of its own
// with the written parts copied in on the GPU -- and every such copy used to end with a full TRANSFER -> ALL_COMMANDS
// barrier, one per copy (~2,530 per frame in the Prologue battle state). The barrier cut defers that barrier to the next
// consumer; the cache cut reuses the earlier copy for a repeat of the same (address, size) while no write reaches the
// ranges it composes. Both are on by default now, so this check runs with no NX_REPLAY_CUTS at all, and per-cut with a
// named value. Neither may change the bytes a reader sees, so this checks the composition itself as well as the
// copies/barriers it takes. The switch is read once per process: each mode is one run of this binary.
void composed_read_cuts() {
    // The same value parse as replay_cuts() (nvn_written.cpp): unset means the default pair, a name selects exactly the
    // cuts it lists, "1"/"all" enables every cut, "0" is handled by main (it runs the pre-cut checks instead).
    const char* e = std::getenv("NX_REPLAY_CUTS");
    const bool named = e && e[0];
    const bool names_barrier = named && std::strstr(e, "barrier") != nullptr;
    const bool names_cache = named && std::strstr(e, "cache") != nullptr;
    const bool names_split = named && (std::strstr(e, "pipeline") != nullptr || std::strstr(e, "split") != nullptr);
    const bool cut_barrier = named ? (names_barrier || !(names_cache || names_split)) : true;
    const bool cut_cache = named ? (names_cache || !(names_barrier || names_split)) : true;
    std::vector<uint8_t> guest(0x400, 0x5a);
    replay::written_reset();  // a new submit: no ranges, no cached copies
    // The dispatch fills [0x100, 0x200): the copy holds the GPU's bytes as soon as it runs.
    Upload written = replay::written_bind(VK_NULL_HANDLE, guest.data() + 0x100, 0x100, 0x100);
    std::memset(written.ptr, 0x77, 0x100);
    gpu.clear();
    const auto copies = [&] { return size_t(std::count(events.begin(), events.end(), 'C')); };
    size_t c0 = copies(), b0 = barriers.size();
    Upload first, second;
    // [0x180, 0x280) starts inside the written range and runs past it: not bindable in place, so composed.
    check(replay::written_read(VK_NULL_HANDLE, guest.data() + 0x180, 0x100, 0x100, 4, first), "read past the written range composes");
    size_t c1 = copies(), b1 = barriers.size();
    check(replay::written_read(VK_NULL_HANDLE, guest.data() + 0x180, 0x100, 0x100, 4, second), "the same read composes again");
    size_t c2 = copies(), b2 = barriers.size();
    run_gpu();
    check(first.ptr[0] == 0x77 && first.ptr[0x7f] == 0x77 && first.ptr[0x80] == 0x5a && first.ptr[0xff] == 0x5a,
          "composition: the written part from the GPU copy, the rest from guest memory");
    check(second.buffer == first.buffer && second.offset == first.offset ? cut_cache : !cut_cache,
          "a repeated read reuses the earlier copy exactly when the cache is on");
    check(std::memcmp(first.ptr, second.ptr, 0x100) == 0, "both readers see the same bytes");
    check(c1 == c0 + 1, "a composed read records one buffer copy");
    check(c2 == c1 + (cut_cache ? 0 : 1), "a repeated read copies again unless the cache answered it");
    check(b1 == b0 + (cut_barrier ? 0 : 1), "one visibility barrier per copy unless the cut defers it");
    check(b2 == b1 + (cut_cache || cut_barrier ? 0 : 1), "the deferred or cached path records no barrier of its own");
    if (cut_barrier) {
        replay::copy_write_barrier(VK_NULL_HANDLE);
        check(barriers.size() == b2 + 1, "one deferred barrier, at the consumer");
        check(barriers.back().src == VK_PIPELINE_STAGE_TRANSFER_BIT &&
                  barriers.back().dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
                  (barriers.back().read & VK_ACCESS_TRANSFER_WRITE_BIT) && (barriers.back().write & VK_ACCESS_MEMORY_READ_BIT),
              "the deferred barrier is the transfer write -> all commands one");
        replay::copy_write_barrier(VK_NULL_HANDLE);
        check(barriers.size() == b2 + 1, "nothing is recorded without pending copies");
    } else {
        replay::copy_write_barrier(VK_NULL_HANDLE);
        check(barriers.size() == b2, "nothing is deferred when the cut is off");
    }
    // A write to the composed range invalidates it: the next read must compose the new GPU bytes, not reuse the copy.
    Upload rewrite = replay::written_bind(VK_NULL_HANDLE, guest.data() + 0x100, 0x100, 0x100);
    std::memset(rewrite.ptr, 0x33, 0x100);
    gpu.clear();
    Upload third;
    check(replay::written_read(VK_NULL_HANDLE, guest.data() + 0x180, 0x100, 0x100, 4, third), "the read after a write composes");
    check(!(third.buffer == first.buffer && third.offset == first.offset), "a write to the composed range invalidates the cached copy");
    run_gpu();
    check(third.ptr[0] == 0x33 && third.ptr[0x80] == 0x5a, "the new read composes the new GPU bytes and the guest's");
}

// Run in a dedicated process: the switches are cached on first use. In the old
// aggregation loop, the whole range left the result cursor at job zero, so the
// following dirty merge was falsely learned clean twice and became whole on
// the third bind. Its later CPU write was then overwritten by stale GPU bytes.
void whole_before_dirty_merge() {
    unsetenv("NX_COMPUTE_SEPARATE");
    unsetenv("NX_WRITE_BACK_MERGE");
    unsetenv("NX_WRITE_BACK_WHOLE");
    setenv("NX_WRITE_BACK_FAST_DIRTY", "1", 1);  // check every batch, no sampling
    setenv("NX_P2_KEPT_COPY", "1", 1);
    static std::vector<uint8_t> guest(128, 0x31);
    allow_kept_allocations = true;
    for (uint32_t round = 0; round < 4; ++round) {
        replay::written_reset();
        const auto whole = replay::written_bind(VK_NULL_HANDLE, guest.data(), 64, 64, true);
        const auto merge = replay::written_bind(VK_NULL_HANDLE, guest.data() + 64, 64, 64, false);
        std::memset(whole.ptr, int(0x40 + round), 64);
        const uint32_t gpu_word = 0x12340000 + round;
        const uint32_t cpu_word = 0xabcd0000 + round;
        std::memcpy(merge.ptr, &gpu_word, 4);  // only the first word belongs to the GPU
        std::memcpy(guest.data() + 68, &cpu_word, 4);  // CPU changes the untouched second word
        auto batch = replay::written_take();
        check(batch.size() == 2 && batch[0].whole, "regression requires whole before merge");
        const bool kept_snapshot = !batch[1].whole && bool(batch[1].before);
        write_backs_apply(batch);
        check(std::memcmp(guest.data() + 68, &cpu_word, 4) == 0,
              "whole-before-merge must preserve the CPU's untouched word on every batch");
        check(std::memcmp(guest.data() + 64, &gpu_word, 4) == 0, "GPU changed word written back");
        check(kept_snapshot, "dirty merge must not be falsely learned whole");
    }
    check(kept_allocations == 2, "two retained buffers were allocated and reused");
    allow_kept_allocations = false;
    puts("bayonetta3_nvn_buffer_copy_test: whole-before-dirty-merge learning passed");
}

int main(int argc,char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "merge-learning") == 0) {
            whole_before_dirty_merge();
            return 0;
        }
        const char* cuts_env = std::getenv("NX_REPLAY_CUTS");
        const bool cuts_off = cuts_env && cuts_env[0] == '0' && !cuts_env[1];
        if (argc == 1 && !cuts_off) {  // the cuts are on by default (nvn_written.cpp): their own checks
            composed_read_cuts();
            puts("bayonetta3_nvn_buffer_copy_test: replay cuts (NX_REPLAY_CUTS) passed");
            return 0;
        }
        // The checks below describe the behaviour before the replay cuts (one barrier and one copy per composed read),
        // so this path pins them off before anything reads the switch (replay_cuts() caches its value on first call).
        setenv("NX_REPLAY_CUTS", "0", 1);
        if(argc==1)unsetenv("NX_COMPUTE_SEPARATE");
        std::vector<uint8_t> a(512,0x11),b(512,0x22),c(512,0x33);
        auto& pa=pool(0x1000,0x100000,a);auto& pb=pool(0x2000,0x200000,b);auto& pc=pool(0x3000,0x300000,c);
        if(argc>1){rejects([&]{replay_copy(pa.gpu,pb.gpu,32);});puts("separate tracking rejected");return 0;}
        constexpr uint64_t cb=0x4000;
        invoke("nvnCommandBufferInitialize",cb);invoke("nvnCommandBufferBeginRecording",cb);
        invoke("nvnCommandBufferCopyBufferToBuffer",cb,pa.gpu+3,pb.gpu+7,33,0);
        auto list=command_list(invoke("nvnCommandBufferEndRecording",cb));
        check(list && list->cmds.size()==1,"production recorder appends command");
        const auto& cmd=list->cmds[0];
        check(cmd.op==Cmd::CopyBufferToBuffer && cmd.a[0]==pa.gpu+3 && cmd.a[1]==pb.gpu+7 && cmd.a[2]==33 && cmd.n==0,"HLE x1..x4 ABI");
        check(gpu.empty() && b[7]==0x22,"recording is deferred");
        replay_buffer_copy(VK_NULL_HANDLE,cmd);
        check(events==std::vector<char>({'E','F','B','C','B'}),"end/flush/pre-barrier/copy/post-barrier order");
        check(barriers[0].src==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && barriers[0].dst==VK_PIPELINE_STAGE_TRANSFER_BIT &&
              (barriers[0].read&VK_ACCESS_MEMORY_WRITE_BIT) && (barriers[0].write&VK_ACCESS_TRANSFER_READ_BIT) &&
              (barriers[0].write&VK_ACCESS_TRANSFER_WRITE_BIT),"transfer pre dependency");
        check(barriers.back().src==VK_PIPELINE_STAGE_TRANSFER_BIT && barriers.back().dst==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
              (barriers.back().read&VK_ACCESS_TRANSFER_WRITE_BIT) && (barriers.back().write&VK_ACCESS_MEMORY_READ_BIT),"transfer post dependency");
        check(b[7]==0x22,"replay does not change host");finish();
        check(b[6]==0x22 && b[7]==0x11 && b[39]==0x11 && b[40]==0x22,"unaligned byte copy and sentinels");
        // Earlier GPU writes + CPU-only gaps must compose into one source, and a
        // chain must consume GPU output even though no guest bytes changed yet.
        queued_write(a.data()+8,16,0x7a);
        replay_copy(pa.gpu,pb.gpu,48);replay_copy(pb.gpu,pc.gpu,48);
        check(a[8]==0x11 && c[8]==0x33,"GPU chain stays deferred");
        // An earlier reader captures old contents before a later overwrite.
        Upload shared;check(replay::written_read(VK_NULL_HANDLE,b.data(),48,48,1,shared),"destination tracked");
        auto snapshot=upload_alloc(48,16);VkBufferCopy r{shared.offset,0,48};copy(VK_NULL_HANDLE,shared.buffer,snapshot.buffer,1,&r);
        queued_write(a.data(),48,0x9b);replay_copy(pa.gpu,pb.gpu,48);
        finish();
        for(size_t i=0;i<48;++i){auto expected=i>=8&&i<24?0x7a:0x11;check(c[i]==expected && snapshot.ptr[i]==expected,"earlier consumer sees ordered copy");check(b[i]==0x9b,"later overwrite uses latest GPU source");}
        // Both source and destination cross different virtual mapping boundaries.
        replay::written_reset();
        auto& vs=make<MemoryPool>(0x5000);vs.gpu=0x500000;vs.size=64;
        vs.mappings[0]={0x1000,128,20};vs.mappings[20]={0x2000,128,44};
        auto& vd=make<MemoryPool>(0x6000);vd.gpu=0x600000;vd.size=64;
        vd.mappings[0]={0x3000,128,13};vd.mappings[13]={0x3000,160,51};
        queued_write(a.data()+128,20,0x65);queued_write(b.data()+128,44,0x76);
        replay_copy(vs.gpu,vd.gpu,64);finish();
        for(int i=0;i<64;++i){size_t off=i<13?128+i:160+i-13;check(c[off]==(i<20?0x65:0x76),"both mapping boundaries split");}
        check(c[127]==0x33 && c[141]==0x33 && c[159]==0x33 && c[211]==0x33,"mapping holes untouched");
        // Distinct GPU aliases of one host range reuse production tracking.
        auto& alias=make<MemoryPool>(0x7000);alias.gpu=0x700000;alias.size=a.size();alias.memory=pa.memory;
        queued_write(a.data()+256,32,0xb7);replay_copy(alias.gpu+256,pc.gpu+256,32);finish();check(c[256]==0xb7,"host alias reads tracked GPU writes");
        // Non-overlapping subranges in one tracked allocation are valid Vulkan copies.
        queued_write(a.data(),256,0xac);replay_copy(pa.gpu,pa.gpu+128,32);finish();check(a[128]==0xac,"same buffer disjoint copy");
        // Unknown flags, virtual and physical overlap, overflow and late holes
        // fail before any partial transfer or tracking mutation.
        rejects([&]{invoke("nvnCommandBufferCopyBufferToBuffer",cb,pa.gpu,pb.gpu,32,0x20);});
        rejects([&]{replay_buffer_copy(VK_NULL_HANDLE,command(pa.gpu,pb.gpu,32,1));});
        rejects([&]{replay_copy(UINT64_MAX-8,pb.gpu,16);});
        rejects([&]{replay_copy(pa.gpu,UINT64_MAX-8,16);});
        rejects([&]{replay_copy(pa.gpu,pa.gpu+8,32);});
        rejects([&]{replay_copy(pa.gpu,alias.gpu,32);});
        vs.mappings[20]={0x2000,128,43};rejects([&]{replay_copy(vs.gpu,vd.gpu,64);});
        vs.mappings[20]={0x1000,500,44};rejects([&]{replay_copy(vs.gpu,vd.gpu,64);});
        vs.mappings[20]={0x1000,128,44};rejects([&]{replay_copy(vs.gpu,alias.gpu+128,64);});
        auto& bad=make<MemoryPool>(0x8000);bad.gpu=0x800000;bad.size=128;bad.memory=UINT64_MAX-16;
        rejects([&]{replay_copy(bad.gpu+8,pb.gpu,32);});
        rejects([&]{replay_copy(pa.gpu+500,pb.gpu,32);});
        auto n=events.size();replay_copy(0,0,0);check(n==events.size(),"zero copy is no-op");
        puts("bayonetta3_nvn_buffer_copy_test: recording, GPU ordering, tracking, mapping and validation passed");
    }catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
