// Exercises production recorder, replay, raster and shared GPU-write tracking.
// Vulkan commands are queued by a deterministic fake GPU, never applied at record time.
#include "../runtime/src/nvn/nvn_cmd_replay.h"
#include "../runtime/src/nvn/nvn_draw_state.h"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>
#include <limits>

using namespace nx;
using namespace nx::nvn;
namespace {
std::map<std::string, HleFn>& registry() { static std::map<std::string, HleFn> r; return r; }
std::vector<std::function<void()>> gpu;
std::vector<std::unique_ptr<std::vector<uint8_t>>> allocations;
int ends = 0, flushes = 0, barriers = 0;
VkPhysicalDeviceFeatures features{};
float last_bounds[2]{};
float last_bias[3]{};
unsigned bounds_calls=0;
bool unrestricted_depth=false;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
uint64_t invoke(const char* name, uint64_t a=0,uint64_t b=0,uint64_t d=0,uint64_t e=0,float f=1) {
    CPU c{}; c.x[0]=a;c.x[1]=b;c.x[2]=d;c.x[3]=e;c.v[0].f32[0]=f;
    check(registry().count(name), name); registry().at(name)(c); return c.x[0];
}
void run_gpu() { for(auto& f:gpu) f(); gpu.clear(); }
void apply_write_back(WriteBack w) { std::vector<WriteBack> batch{std::move(w)}; write_backs_apply(batch); }
void VKAPI_PTR fill(VkCommandBuffer,VkBuffer b,VkDeviceSize offset,VkDeviceSize size,uint32_t value) {
    gpu.emplace_back([=]{auto* p=reinterpret_cast<uint8_t*>(b)+offset;for(uint64_t i=0;i<size;i+=4)std::memcpy(p+i,&value,4);});
}
void VKAPI_PTR copy(VkCommandBuffer,VkBuffer src,VkBuffer dst,uint32_t count,const VkBufferCopy* ranges) {
    for(uint32_t i=0;i<count;++i) { auto r=ranges[i];gpu.emplace_back([=]{std::memmove(reinterpret_cast<uint8_t*>(dst)+r.dstOffset,reinterpret_cast<uint8_t*>(src)+r.srcOffset,r.size);}); }
}
void VKAPI_PTR barrier(VkCommandBuffer,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags,uint32_t n,const VkMemoryBarrier* b,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*) {
    check(src && dst,"barrier stages"); if(n)check(b[0].srcAccessMask && b[0].dstAccessMask,"barrier accesses"); ++barriers;
}
void VKAPI_PTR properties(VkPhysicalDevice,VkPhysicalDeviceProperties* p) { p->limits.minStorageBufferOffsetAlignment=256;p->limits.minUniformBufferOffsetAlignment=256; }
void VKAPI_PTR stencil(VkCommandBuffer,VkStencilFaceFlags,uint32_t) {}
void VKAPI_PTR bias(VkCommandBuffer,float units,float clamp,float factor) {
    last_bias[0]=units;last_bias[1]=clamp;last_bias[2]=factor;
}
void VKAPI_PTR bounds(VkCommandBuffer,float lo,float hi){last_bounds[0]=lo;last_bounds[1]=hi;++bounds_calls;}
template<class F> void rejects(F f) { bool threw=false;try{f();}catch(const std::runtime_error&){threw=true;}check(threw,"invalid input rejected"); }
}
namespace nx::plat {
uint64_t monotonic_ns() { return 0; }
bool late_guard_on(){return false;} uint64_t late_guard_add(uint64_t,uint64_t){return 0;} uint32_t late_guard_open(LateGuardEvent*,uint32_t){return 0;}
void set_thread_name(const char*) {}
}
namespace nx {
HleRegistrar::HleRegistrar(const char* n,HleFn f){ check(registry().emplace(n,f).second,"duplicate HLE"); }
void log(const char*,...) {}
[[noreturn]] void fatal(const char*,...){throw std::runtime_error("production fatal");}
std::string describe(uint64_t){return "test";}
}
namespace nx::vk {
PFN_vkCmdFillBuffer p_vkCmdFillBuffer=fill;
PFN_vkCmdCopyBuffer p_vkCmdCopyBuffer=copy;
PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier=::barrier;
PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties=properties;
PFN_vkCmdSetStencilReference p_vkCmdSetStencilReference=stencil;
PFN_vkCmdSetStencilCompareMask p_vkCmdSetStencilCompareMask=stencil;
PFN_vkCmdSetStencilWriteMask p_vkCmdSetStencilWriteMask=stencil;
PFN_vkCmdSetDepthBias p_vkCmdSetDepthBias=bias;
PFN_vkCmdSetDepthBounds p_vkCmdSetDepthBounds=bounds;
// This harness uses the upload-ring fallback; kept allocations are exercised by the buffer-copy harness.
VkResult VKAPI_PTR create_buffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer*) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
PFN_vkCreateBuffer p_vkCreateBuffer=create_buffer;
PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements=nullptr;
PFN_vkAllocateMemory p_vkAllocateMemory=nullptr;
PFN_vkBindBufferMemory p_vkBindBufferMemory=nullptr;
PFN_vkMapMemory p_vkMapMemory=nullptr;
PFN_vkDestroyBuffer p_vkDestroyBuffer=nullptr;
PFN_vkFreeMemory p_vkFreeMemory=nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties=nullptr;
PFN_vkUnmapMemory p_vkUnmapMemory=nullptr;
}
namespace nx::nvn {
Texture::~Texture()=default;
ObjectsLock& objects_lock(){static ObjectsLock l;return l;}
std::unordered_map<uint64_t,std::unique_ptr<Object>>& objects(){static std::unordered_map<uint64_t,std::unique_ptr<Object>> o;return o;}
bool resolve_cache_on(){return false;}
Object* find_object(uint64_t g,uint64_t){auto i=objects().find(g);return i==objects().end()?nullptr:i->second.get();}
void defer_object(std::unique_ptr<Object>){}
void note_replaced(uint64_t){}
uint64_t current_epoch(){return 1;}
const std::vector<const ShaderInfo*>* program_shaders(uint64_t){return nullptr;}
ViewInfo read_view(CPU&,uint64_t){return {};}
MemoryPool* pool_at_gpu(uint64_t g,uint64_t* off){for(auto& [_,o]:objects())if(auto* p=dynamic_cast<MemoryPool*>(o.get());p && g>=p->gpu && g-p->gpu<p->size){*off=g-p->gpu;return p;}return nullptr;}
uint64_t pool_host(const MemoryPool& p,uint64_t off){if(p.memory)return p.memory+off;auto i=p.mappings.upper_bound(off);if(i==p.mappings.begin())return 0;--i;if(off-i->first>=i->second.size)return 0;auto* physical=get<MemoryPool>(i->second.physical);return physical?physical->memory+i->second.physical_offset+off-i->first:0;}
Upload upload_alloc(uint64_t n,uint64_t){auto a=std::make_unique<std::vector<uint8_t>>(n);auto* p=a->data();allocations.push_back(std::move(a));return {reinterpret_cast<VkBuffer>(p),0,p};}
VkPhysicalDevice vk_physical(){return VK_NULL_HANDLE;}
VkDevice vk_device(){return VK_NULL_HANDLE;}
uint32_t memory_type(uint32_t,VkMemoryPropertyFlags){return 0;}
Upload host_import_upload(const void*,uint64_t,uint64_t){return {};}
bool vk_host_import(){return false;}
bool async_gpu(){return false;}
void frame_wait_all(){throw std::runtime_error("unexpected retained-buffer wait");}
void frame_carry_wait(){}
void crash_nvn_note(const Cmd&,const CmdList&,int,uint64_t){}
const VkPhysicalDeviceFeatures& vk_features(){return features;}
bool vk_depth_range_unrestricted(){return unrestricted_depth;}
void draw_end_rendering(VkCommandBuffer,int){++ends;}
void clears_flush(VkCommandBuffer){++flushes;}
bool pass_open(){return false;}
VkCommandBuffer frame_chunk(VkCommandBuffer cb,uint64_t,bool){return cb;}
VkCommandBuffer frame_cb(VkCommandBuffer cb){return cb;}
VkCommandBuffer frame_after_dispatch(VkCommandBuffer cb){return cb;}
bool gpu_profile_on(){return false;}
void gpu_profile_mark(VkCommandBuffer,const std::string&){}
void draw_clear(VkCommandBuffer,const Targets&,const Cmd&) {throw std::runtime_error("unexpected draw clear");}
void replay_copy(VkCommandBuffer,const Cmd&,const CmdList&){throw std::runtime_error("unexpected texture copy");}
void draw_command(VkCommandBuffer,const Cmd& c,const CmdList&,const Targets&) {check(replay::raster_command(c),"unexpected draw command");}
namespace replay {
State g_state;
Stats g_stats;
void compute_write_barrier(VkCommandBuffer){}  // This harness never dispatches a compute shader.
void trace_nan_write_back(const uint8_t*,const uint8_t*,uint64_t){}
void dedup_invalidate(const uint8_t*,uint64_t){}
Upload dedup_upload(const uint8_t* p,uint64_t n,uint64_t padded,uint64_t align, bool){auto a=upload_alloc(padded,align);std::memcpy(a.ptr,p,n);return a;}
}
}
int main(int argc,char** argv){
    try {
        unsetenv("NX_EQUAL_SLACK");unsetenv("NX_EQUAL_SLACK_FACTOR");
        unsetenv("NX_GREATER_SLACK");unsetenv("NX_DEPTH_BIAS_UNITS");
        float tolerance=4.0f,slope=0.0f;
        if(argc>1 && std::strcmp(argv[1],"old-slope")==0){setenv("NX_EQUAL_SLACK_FACTOR","1",1);slope=1.0f;}
        if(argc>1 && std::strcmp(argv[1],"no-slack")==0){setenv("NX_EQUAL_SLACK","0",1);tolerance=0.0f;}
        if(argc>1 && std::strcmp(argv[1],"more-units")==0){setenv("NX_EQUAL_SLACK","8",1);tolerance=8.0f;}
        DepthStencilInfo depth{};depth.depth_test=1;depth.depth_func=3;
        check(equal_slack_pass(depth)==(tolerance!=0),"read-only EQUAL compensation eligibility");
        depth.depth_write=1;check(!equal_slack_pass(depth),"depth-writing pre-pass must not receive tolerance");
        depth.depth_write=0;depth.depth_func=2;check(!equal_slack_pass(depth),"LESS must not receive tolerance");
        depth.depth_func=5;check(!equal_slack_pass(depth),"GREATER compensation remains opt-in");
        depth.depth_func=3;
        features.depthBiasClamp=VK_TRUE;
        Cmd polygon_bias{Cmd::SetPolygonOffsetClamp};
        polygon_bias.f[0]=2.0f;polygon_bias.f[1]=3.0f;polygon_bias.f[2]=0.5f;
        check(replay::raster_command(polygon_bias),"guest polygon offset recorded");
        replay::raster_dynamic(VK_NULL_HANDLE,false);
        check(last_bias[0]==3 && last_bias[1]==0.5f && last_bias[2]==2,"guest offset survives without compensation");
        replay::raster_dynamic(VK_NULL_HANDLE,equal_slack_pass(depth));
        check(last_bias[0]==3-tolerance && last_bias[2]==2-slope,"constant tolerance must not introduce slope bias");
        check(last_bias[1]==(tolerance!=0 ? 0.0f : 0.5f),"clamp changes only for a compensated draw");
        replay::raster_dynamic(VK_NULL_HANDLE,false);
        check(last_bias[0]==3 && last_bias[1]==0.5f && last_bias[2]==2,"compensation must not leak into subsequent draws");
        replay::g_state={};features={};
        unsetenv("NX_COMPUTE_SEPARATE");
        std::vector<uint8_t> bytes(0x300,0xa5);
        auto& pool=make<MemoryPool>(0x1000);pool.memory=reinterpret_cast<uint64_t>(bytes.data());pool.gpu=0x100000;pool.size=bytes.size();
        constexpr uint64_t cb=0x2000;
        invoke("nvnCommandBufferInitialize",cb);
        invoke("nvnCommandBufferBeginRecording",cb);
        invoke("nvnCommandBufferSetShaderScratchMemory",cb,0x1000,0x40,0x100);
        invoke("nvnCommandBufferSetLineWidth",cb,0xdeadbeef,0,0,2.5f);
        invoke("nvnCommandBufferClearBuffer",cb,pool.gpu+0x40,0x80,0x12345678);
        invoke("nvnCommandBufferSetTiledCacheAction",cb,3);
        auto list=command_list(invoke("nvnCommandBufferEndRecording",cb));
        check(list && list->cmds.size()==4,"record count");
        check(list->cmds[0].a[3]==pool.gpu+0x40,"scratch GPU resolution");
        check(list->cmds[1].f[0]==2.5f,"float ABI uses s0");
        check(list->cmds[2].n==0x12345678 && list->cmds[2].a[1]==0x80,"clear ABI");
        for(auto b:bytes)check(b==0xa5,"recording cannot mutate guest");
        Targets targets{};
        submit_lists_reset();
        replay_list(VK_NULL_HANDLE,targets,*list,0);
        check(replay::g_state.line_width==2.5f,"raster line state");
        check(command_memory_state().scratch_pool==0x1000 && command_memory_state().scratch_size==0x100,"scratch replay contract");
        check(flushes==2 && ends==2 && barriers>=3,"clear/cache explicit ordering");
        for(auto b:bytes)check(b==0xa5,"replay cannot eagerly mutate guest");
        // Read after clear, before overlapping second clear: an independent GPU
        // snapshot captures the first value; the shared range gets the second.
        auto first=upload_alloc(0x80,256);
        Upload shared;
        check(replay::written_read(VK_NULL_HANDLE,bytes.data()+0x40,0x80,0x80,4,shared),"clear tracked as GPU write");
        VkBufferCopy r{shared.offset,0,0x80};copy(VK_NULL_HANDLE,shared.buffer,first.buffer,1,&r);
        Cmd second{Cmd::ClearBuffer};second.a[0]=pool.gpu+0x80;second.a[1]=0x80;second.n=0xfedcba98;
        replay_clear_buffer(VK_NULL_HANDLE,second);
        auto writes=replay::written_take();check(writes.size()==1,"overlap merged into one writeback");
        run_gpu();
        check(ld<uint32_t>(reinterpret_cast<uint64_t>(first.ptr)+0x40)==0x12345678,"earlier read retains first fill");
        for(const auto& w:writes)apply_write_back(w);
        for(size_t i=0;i<bytes.size();i+=4){uint32_t expected=i>=0x80&&i<0x100?0xfedcba98:i>=0x40&&i<0x80?0x12345678:0xa5a5a5a5;check(ld<uint32_t>(reinterpret_cast<uint64_t>(bytes.data()+i))==expected,"ordered clear and sentinels");}
        for(uint32_t action=1;action<=6;++action){Cmd c{Cmd::SetTiledCacheAction};c.n=action;int before=barriers;replay_command_memory(VK_NULL_HANDLE,c);check(barriers==before+1,"each tiled action maps barrier");check(command_memory_state().last_cache_action==action,"cache action stored");}
        check(!command_memory_state().render_target_binning,"binning disabled");
        Cmd zero{Cmd::ClearBuffer};zero.a[0]=pool.gpu;replay_clear_buffer(VK_NULL_HANDLE,zero);
        rejects([&]{invoke("nvnCommandBufferClearBuffer",cb,pool.gpu+1,4,0);});
        rejects([&]{invoke("nvnCommandBufferClearBuffer",cb,UINT64_MAX-3,8,0);});
        rejects([&]{invoke("nvnCommandBufferSetLineWidth",cb,0,0,0,std::numeric_limits<float>::quiet_NaN());});
        rejects([&]{invoke("nvnCommandBufferSetTiledCacheAction",cb,0);});
        rejects([&]{checked_scratch_address(0x1000,UINT64_MAX,4);});
        rejects([&]{checked_scratch_address(0x1000,0x200,0x101);});
        check(checked_scratch_address(0,0,0)==0,"scratch disable");
        // Split a virtual clear at physical mapping boundaries.
        replay::written_reset();
        std::vector<uint8_t> other(0x100,0x33);
        auto& physical=make<MemoryPool>(0x3000);physical.memory=reinterpret_cast<uint64_t>(other.data());physical.size=other.size();physical.gpu=0x200000;
        auto& virt=make<MemoryPool>(0x4000);virt.size=0x100;virt.gpu=0x300000;
        virt.mappings[0]={0x1000,0x100,0x80};virt.mappings[0x80]={0x3000,0,0x80};
        Cmd v{Cmd::ClearBuffer};v.a[0]=virt.gpu;v.a[1]=0x100;v.n=0xabcdef01;
        replay_clear_buffer(VK_NULL_HANDLE,v);auto split=replay::written_take();check(split.size()==2,"virtual mappings split");run_gpu();for(const auto& w:split)apply_write_back(w);
        check(ld<uint32_t>(reinterpret_cast<uint64_t>(bytes.data()+0x100))==0xabcdef01 && ld<uint32_t>(reinterpret_cast<uint64_t>(other.data()))==0xabcdef01,"virtual backing updated");
        check(other[0x80]==0x33 && bytes[0x180]==0xa5,"virtual boundaries preserved");
        invoke("nvnCommandBufferBeginRecording",cb);
        for(uint64_t raw : {0ull,1ull,2ull,0x101ull,0x100ull})
            invoke("nvnCommandBufferSetColorReductionEnable",cb,raw);
        for(uint32_t format=0;format<6;++format)
            invoke("nvnCommandBufferSetColorReductionThresholds",cb,format,500,UINT64_MAX);
        auto abi=command_list(invoke("nvnCommandBufferEndRecording",cb));
        for(size_t k=0;k<5;++k)
            check(abi->cmds[k].n==(k==1 || k==3),"SDK enable low-byte normalization");
        for(uint32_t format=0;format<6;++format)
            check(abi->cmds[5+format].i[0]==(format==3?63:255) && abi->cmds[5+format].i[1]==0,"all SDK threshold classes clamp signed inputs");
        rejects([&]{invoke("nvnCommandBufferSetColorReductionThresholds",cb,6);});
        // Actual B3 color reduction state: disabled, zero thresholds for classes 0/5/3.
        invoke("nvnCommandBufferBeginRecording",cb);
        invoke("nvnCommandBufferSetPatchSize",cb,4);
        invoke("nvnCommandBufferSetColorReductionEnable",cb,0);
        for(uint32_t format : {0u,5u,3u})invoke("nvnCommandBufferSetColorReductionThresholds",cb,format,0,0);
        invoke("nvnCommandBufferSetColorReductionThresholds",cb,3,500,UINT64_MAX);
        auto colors=command_list(invoke("nvnCommandBufferEndRecording",cb));
        check(colors->cmds.back().i[0]==63 && colors->cmds.back().i[1]==0,"SDK threshold clamping");
        replay_list(VK_NULL_HANDLE,targets,*colors,0);
        check(replay::g_state.patch_size==4,"patch size raster state");
        rejects([&]{invoke("nvnCommandBufferSetPatchSize",cb,0);});
        rejects([&]{invoke("nvnCommandBufferSetPatchSize",cb,33);});
        check(!command_memory_state().color_reduction_enabled && command_memory_state().color_reduction_thresholds[3][0]==63,"disabled precision state preserved");
        Cmd enable{Cmd::SetColorReductionEnable};enable.n=1;
        replay_command_memory(VK_NULL_HANDLE,enable);
        check(command_memory_state().color_reduction_enabled && command_memory_state().color_reduction_thresholds[3][0]==63,
              "nonzero thresholds replay at native precision");
        reset_command_memory_state();replay_command_memory(VK_NULL_HANDLE,enable);
        check(command_memory_state().color_reduction_enabled,"zero thresholds preserve native precision");
        // SetAlphaRef uses s0, copies every bit without a clamp, and only
        // changes replay state when its command executes. It does not enable
        // ColorState's alpha test (SDK ALWAYS=8).
        static_assert(Cmd::SetAlphaRef == Cmd::SetPatchSize + 1, "append-only command ABI");
        check(ColorStateInfo{}.alpha_test==8,"SDK default alpha ALWAYS");
        replay::g_state.alpha_ref=0.125f;
        invoke("nvnCommandBufferBeginRecording",cb);
        constexpr uint32_t alpha_bits[]={0u,0x80000000u,0x3f000000u,0xbf800000u,
                                        0x40000000u,0x7f800000u,0xff800000u,0x7fc12345u};
        for(uint32_t bits:alpha_bits)
            invoke("nvnCommandBufferSetAlphaRef",cb,0xdeadbeef,0,0,std::bit_cast<float>(bits));
        auto alphas=command_list(invoke("nvnCommandBufferEndRecording",cb));
        check(replay::g_state.alpha_ref==0.125f,"record alpha has no replay side effect");
        check(alphas->cmds.size()==std::size(alpha_bits),"alpha record count");
        for(size_t k=0;k<std::size(alpha_bits);++k){
            check(alphas->cmds[k].op==Cmd::SetAlphaRef &&
                  std::bit_cast<uint32_t>(alphas->cmds[k].f[0])==alpha_bits[k],"raw s0 alpha ABI");
            CmdList one;one.cmds.push_back(alphas->cmds[k]);
            replay_list(VK_NULL_HANDLE,targets,one,0);
            check(std::bit_cast<uint32_t>(replay::g_state.alpha_ref)==alpha_bits[k],"raw alpha replay bits");
            require_alpha_test_supported(replay::g_state.color);
        }
        for(uint32_t compare=0;compare<=9;++compare){
            ColorStateInfo color;color.alpha_test=compare;
            if(compare==8)require_alpha_test_supported(color);
            else rejects([&]{require_alpha_test_supported(color);});
        }
        check(replay::g_state.color.alpha_test==8,"alpha reference does not enable test");
        // CallList keeps replay order; distinct command memory prevents retirement.
        constexpr uint64_t child_cb=0x6000,parent_cb=0x7000;
        invoke("nvnCommandBufferInitialize",child_cb);
        invoke("nvnCommandBufferAddCommandMemory",child_cb,0x1000,0,0x100);
        invoke("nvnCommandBufferSetLineWidth",child_cb,0,0,0,4.0f);
        invoke("nvnCommandBufferSetAlphaRef",child_cb,0,0,0,0.75f);
        invoke("nvnCommandBufferClearBuffer",child_cb,pool.gpu,4,0x11111111);
        uint64_t child=invoke("nvnCommandBufferEndRecording",child_cb);
        invoke("nvnCommandBufferInitialize",parent_cb);
        invoke("nvnCommandBufferAddCommandMemory",parent_cb,0x1000,0x180,0x100);
        invoke("nvnCommandBufferCallCommands",parent_cb,1,reinterpret_cast<uint64_t>(&child));
        invoke("nvnCommandBufferSetLineWidth",parent_cb,0,0,0,1.0f);
        invoke("nvnCommandBufferSetAlphaRef",parent_cb,0,0,0,0.25f);
        invoke("nvnCommandBufferClearBuffer",parent_cb,pool.gpu,4,0x22222222);
        invoke("nvnCommandBufferCopyBufferToBuffer",parent_cb,pool.gpu,pool.gpu+8,4);
        auto parent=command_list(invoke("nvnCommandBufferEndRecording",parent_cb));
        check(parent->cmds[0].op==Cmd::CallList && parent->cmds[0].a[0]==child,"nested call recorded");
        submit_lists_reset();check(command_memory_state().scratch_size==0,"submit resets memory state");
        replay_list(VK_NULL_HANDLE,targets,*parent,0);
        check(replay::g_state.line_width==1.0f,"parent state follows nested list");
        check(replay::g_state.alpha_ref==0.25f,"parent alpha follows nested list");
        auto nested=replay::written_take();run_gpu();for(const auto& w:nested)apply_write_back(w);
        check(ld<uint32_t>(pool.memory)==0x22222222,"parent fill follows nested fill");
        check(ld<uint32_t>(pool.memory+8)==0x22222222,"copy replays after nested and parent fills");
        invoke("nvnCommandBufferBeginRecording",cb);
        invoke("nvnCommandBufferSetDepthBounds",cb,2,0,0,.25f);
        invoke("nvnCommandBufferSetDepthBounds",cb,0x100,0,0,.75f);
        auto bounds_recording=command_list(invoke("nvnCommandBufferEndRecording",cb));
        check(bounds_recording->cmds.size()==2&&bounds_recording->cmds[0].n==1&&bounds_recording->cmds[1].n==0,"SDK enable tests full low byte rather than bit0");
        check(bounds_recording->cmds[0].f[0]==.25f&&bounds_recording->cmds[1].f[0]==.75f,"depth bounds record raw s0");
        Cmd depth_bounds{Cmd::SetDepthBounds};depth_bounds.n=1;depth_bounds.f[0]=0;depth_bounds.f[1]=.999687f;
        check(replay::raster_command(depth_bounds),"depth bounds command accepted");
        replay::raster_dynamic(VK_NULL_HANDLE,false);
        check(bounds_calls==0,"unsupported hardware uses the existing logged depth-bounds fallback");
        features.depthBounds=VK_TRUE;
        replay::raster_dynamic(VK_NULL_HANDLE,false);
        check(bounds_calls==1&&last_bounds[0]==0&&last_bounds[1]==.999687f,"requested depth bounds emitted exactly");
        depth_bounds.f[0]=.25f;depth_bounds.f[1]=.75f;replay::raster_command(depth_bounds);replay::raster_dynamic(VK_NULL_HANDLE,false);
        check(bounds_calls==2&&last_bounds[0]==.25f&&last_bounds[1]==.75f,"changed depth bounds propagated");
        depth_bounds.f[0]=.594697f;depth_bounds.f[1]=1.3947f;
        replay::raster_command(depth_bounds);replay::raster_dynamic(VK_NULL_HANDLE,false);
        check(last_bounds[0]==.594697f&&last_bounds[1]==1,"actual interval intersects normal depth range");
        unrestricted_depth=true;replay::raster_dynamic(VK_NULL_HANDLE,false);
        check(last_bounds[0]==.594697f&&last_bounds[1]==1.3947f,"unrestricted extension preserves native interval");
        unrestricted_depth=false;
        for(auto interval : {std::pair{-2.f,-1.f},std::pair{2.f,3.f},std::pair{1.f,0.f}}){
            depth_bounds.f[0]=interval.first;depth_bounds.f[1]=interval.second;
            replay::raster_command(depth_bounds);replay::raster_dynamic(VK_NULL_HANDLE,false);
            check(last_bounds[0]==1&&last_bounds[1]==0,"empty interval must not admit endpoint fragments");
        }
        depth_bounds.f[0]=-std::numeric_limits<float>::infinity();depth_bounds.f[1]=std::numeric_limits<float>::infinity();
        replay::raster_command(depth_bounds);replay::raster_dynamic(VK_NULL_HANDLE,false);
        check(last_bounds[0]==0&&last_bounds[1]==1,"infinite interval covers all normal attachment depths");
        depth_bounds.f[0]=std::numeric_limits<float>::quiet_NaN();rejects([&]{replay::raster_command(depth_bounds);});
        depth_bounds.n=0;replay::raster_command(depth_bounds);replay::raster_dynamic(VK_NULL_HANDLE,false);
        check(last_bounds[0]==0&&last_bounds[1]==1,"disabled bounds emit legal default interval");
        // Bulk readback must preserve the original changed-word merge, including
        // a chunk boundary, an unchanged CPU-owned word and byte-granular tails.
        for(size_t length : {size_t(0),size_t(7),size_t(13),size_t((1u<<20)+13)}) {
            auto baseline=std::make_shared<std::vector<uint8_t>>(length,0x11);
            auto mapped=*baseline;
            std::vector<uint8_t> guest(length,0xa5),expected=guest;
            for(size_t position : {size_t(1),size_t(7),size_t((1u<<20)-1),size_t(1u<<20),length ? length-1 : 0})
                if(position<length)mapped[position]=0x22;
            size_t at=0;
            for(;at+8<=length;at+=8)
                for(size_t word=at;word<at+8;word+=4)
                    if(std::memcmp(mapped.data()+word,baseline->data()+word,4))
                        std::memcpy(expected.data()+word,mapped.data()+word,4);
            for(;at<length;++at)if(mapped[at]!=(*baseline)[at])expected[at]=mapped[at];
            apply_write_back({mapped.data(),guest.data(),length,baseline});
            check(guest==expected,"bulk readback preserves concurrent guest words and original tail granularity");
            check(mapped.size()==baseline->size(),"readback source unchanged");
        }
        puts("bayonetta3_nvn_command_test: production recording/replay/raster/writeback/depth-bounds checks passed");
    }catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
