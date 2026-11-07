// Production indirect HLE and GPU-written tracker with a deferred Vulkan queue.
// The mock consumes argument/count/index bytes only on GPU execution, exposing
// premature host reads of pending compute/copy outputs.
#include "../runtime/src/nvn/nvn_indirect.h"
#include "../runtime/src/nvn/nvn_draw_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
using namespace nx;
using namespace nx::nvn;
namespace {
std::map<std::string,HleFn>& registry(){static std::map<std::string,HleFn> r;return r;}
std::vector<Cmd> recorded;
std::vector<std::unique_ptr<std::vector<uint8_t>>> allocations;
std::vector<std::function<void()>> gpu;
struct Draw { VkDrawIndexedIndirectCommand args; uint32_t first_index; };
std::vector<Draw> draws;
VkPhysicalDeviceFeatures features{};
bool native_count=true;
uint32_t count_calls=0, indirect_calls=0, vertex_preparations=0, barriers=0;
uint64_t allocation_bytes=0, index_remaining=0, recorded_draw_calls=0;
Upload bound_indices;
VkIndexType bound_type;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
uint8_t* bytes(VkBuffer b,uint64_t off){return reinterpret_cast<uint8_t*>(b)+off;}
void run_gpu(){for(auto& fn:gpu)fn();gpu.clear();}
void consume(VkBuffer b,uint64_t off,uint32_t count,uint32_t stride,Upload index,VkIndexType type){
    for(uint32_t k=0;k<count;++k){
        VkDrawIndexedIndirectCommand a;std::memcpy(&a,bytes(b,off+uint64_t(k)*stride),20);
        uint32_t first=0;
        if(a.indexCount && a.instanceCount){
            auto* p=bytes(index.buffer,index.offset+uint64_t(a.firstIndex)*(type==VK_INDEX_TYPE_UINT16?2:4));
            std::memcpy(&first,p,type==VK_INDEX_TYPE_UINT16?2:4);
        }
        draws.push_back({a,first});
    }
}
void VKAPI_PTR draw(VkCommandBuffer,VkBuffer b,VkDeviceSize off,uint32_t count,uint32_t stride){
    indirect_calls++;auto index=bound_indices;auto type=bound_type;
    gpu.emplace_back([=]{consume(b,off,count,stride,index,type);});
}
void VKAPI_PTR draw_count(VkCommandBuffer,VkBuffer b,VkDeviceSize off,VkBuffer cb,VkDeviceSize co,uint32_t max,uint32_t stride){
    count_calls++;auto index=bound_indices;auto type=bound_type;
    gpu.emplace_back([=]{uint32_t n;std::memcpy(&n,bytes(cb,co),4);consume(b,off,std::min(n,max),stride,index,type);});
}
void VKAPI_PTR copy(VkCommandBuffer,VkBuffer src,VkBuffer dst,uint32_t n,const VkBufferCopy* ranges){
    for(uint32_t k=0;k<n;++k){auto r=ranges[k];gpu.emplace_back([=]{std::memcpy(bytes(dst,r.dstOffset),bytes(src,r.srcOffset),r.size);});}
}
void VKAPI_PTR barrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*){barriers++;}
void VKAPI_PTR props(VkPhysicalDevice,VkPhysicalDeviceProperties* p){p->limits.maxDrawIndirectCount=4;p->limits.minStorageBufferOffsetAlignment=256;p->limits.minUniformBufferOffsetAlignment=256;}
Cmd invoke(const char* name,std::initializer_list<uint64_t> args){CPU c{};size_t i=0;for(auto a:args)c.x[i++]=a;registry().at(name)(c);return recorded.back();}
MemoryPool& pool(uint64_t key,uint64_t address,std::vector<uint8_t>& data){auto& p=make<MemoryPool>(key);p.gpu=address;p.size=data.size();p.memory=reinterpret_cast<uint64_t>(data.data());return p;}
void queued_write(const uint8_t* p,const void* value,size_t size){auto u=replay::written_bind(VK_NULL_HANDLE,p,size,size);std::vector<uint8_t> v(static_cast<const uint8_t*>(value),static_cast<const uint8_t*>(value)+size);gpu.emplace_back([=]{std::memcpy(u.ptr,v.data(),size);});}
void replay_draw(const Cmd& c,IndexedIndirect* saved=nullptr){
    replay::DrawInputs in;IndexedIndirect indirect;
    if(!prepare_indexed_indirect(VK_NULL_HANDLE,nullptr,c,in,indirect))return;
    bound_indices=in.index;bound_type=in.index_type;emit_indexed_indirect(VK_NULL_HANDLE,indirect);
    if(saved)*saved=indirect;
}
template<class F> void rejects(F fn){bool threw=false;try{fn();}catch(const std::runtime_error&){threw=true;}check(threw,"unsupported input must fail explicitly");}
void reset(){run_gpu();replay::written_reset();draws.clear();count_calls=indirect_calls=0;}
}
namespace nx {
HleRegistrar::HleRegistrar(const char* n,HleFn f){check(registry().emplace(n,f).second,"duplicate HLE");}
void log(const char*,...){}
[[noreturn]] void fatal(const char*,...){throw std::runtime_error("production fatal");}
}
namespace nx::vk {
// This test uses the upload-ring fallback; retained Vulkan buffers are tested separately.
VkResult VKAPI_PTR reject_kept_buffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer*){return VK_ERROR_OUT_OF_HOST_MEMORY;}
PFN_vkCreateBuffer p_vkCreateBuffer=reject_kept_buffer;
PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements=nullptr;
PFN_vkAllocateMemory p_vkAllocateMemory=nullptr;
PFN_vkBindBufferMemory p_vkBindBufferMemory=nullptr;
PFN_vkMapMemory p_vkMapMemory=nullptr;
PFN_vkDestroyBuffer p_vkDestroyBuffer=nullptr;
PFN_vkFreeMemory p_vkFreeMemory=nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties=nullptr;
PFN_vkUnmapMemory p_vkUnmapMemory=nullptr;
PFN_vkCmdDrawIndexedIndirect p_vkCmdDrawIndexedIndirect=draw;
PFN_vkCmdDrawIndexedIndirectCount p_vkCmdDrawIndexedIndirectCount=draw_count;
PFN_vkCmdCopyBuffer p_vkCmdCopyBuffer=copy;
PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier=::barrier;
PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties=props;
}
namespace nx::plat { uint64_t monotonic_ns(){return 0;} }
namespace nx::nvn {
ObjectsLock& objects_lock(){static ObjectsLock l;return l;}
std::unordered_map<uint64_t,std::unique_ptr<Object>>& objects(){static std::unordered_map<uint64_t,std::unique_ptr<Object>> o;return o;}
bool resolve_cache_on(){return false;}
Object* find_object(uint64_t g,uint64_t){auto it=objects().find(g);return it==objects().end()?nullptr:it->second.get();}
void defer_object(std::unique_ptr<Object>){}
void record(uint64_t cb,Cmd c,const void*,size_t){check(cb==0x999,"recorder x0");recorded.push_back(c);}
MemoryPool* pool_at_gpu(uint64_t g,uint64_t* off){for(auto& [_,o]:objects())if(auto* p=dynamic_cast<MemoryPool*>(o.get());p && g>=p->gpu && g-p->gpu<p->size){*off=g-p->gpu;return p;}return nullptr;}
uint64_t buffer_remaining_at_gpu(uint64_t){return index_remaining;}
Upload upload_alloc(uint64_t n,uint64_t){allocation_bytes+=n;auto a=std::make_unique<std::vector<uint8_t>>(n);auto* p=a->data();allocations.push_back(std::move(a));return {reinterpret_cast<VkBuffer>(p),0,p};}
VkPhysicalDevice vk_physical(){return VK_NULL_HANDLE;}
VkDevice vk_device(){return VK_NULL_HANDLE;}
uint32_t memory_type(uint32_t,VkMemoryPropertyFlags){return 0;}
bool vk_host_import(){return false;}
bool async_gpu(){return false;}
void frame_wait_all(){throw std::runtime_error("unexpected retained-buffer wait");}
void frame_carry_wait(){}
Upload host_import_upload(const void*,uint64_t,uint64_t){return {};}
bool pass_open(){return false;}
void count_recorded_draw(){++recorded_draw_calls;}
const VkPhysicalDeviceFeatures& vk_features(){return features;}
bool vk_draw_indirect_count(){return native_count;}
void draw_end_rendering(VkCommandBuffer,int){}
VkIndexType vk_index_type(uint32_t t){return t==0?VK_INDEX_TYPE_UINT8_EXT:t==1?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;}
uint32_t index_size(uint32_t t){return 1u<<t;}
namespace replay {
State g_state;
// This isolated test queues writes directly instead of replaying dispatches.
void compute_write_barrier(VkCommandBuffer){}
void dedup_invalidate(const uint8_t*,uint64_t){}
Upload dedup_upload(const uint8_t* p,uint64_t n,uint64_t padded,uint64_t align, bool){auto u=upload_alloc(padded,align);if(n)std::memcpy(u.ptr,p,n);return u;}
bool draw_inputs_shared(VkCommandBuffer,const ShaderInfo*,bool indexed,const Cmd&,DrawInputs&){check(!indexed,"indirect must bypass direct count-based index preparation");vertex_preparations++;return true;}
}
}
int main(int argc,char**){
try{
    if(argc==1)unsetenv("NX_COMPUTE_SEPARATE");
    features.multiDrawIndirect=features.drawIndirectFirstInstance=VK_TRUE;
    std::vector<uint8_t> idx(512),args(512),count(16);
    auto& pi=pool(1,0x10000,idx);auto& pa=pool(2,0x20000,args);auto& pc=pool(3,0x30000,count);
    for(uint32_t i=0;i<128;++i)std::memcpy(idx.data()+i*4,&i,4);
    VkDrawIndexedIndirectCommand a{3,2,7,-5,9};std::memcpy(args.data(),&a,20);
    auto single=invoke("nvnCommandBufferDrawElementsIndirect",{0x999,5,2,pi.gpu,pa.gpu,0xffffffff});
    check(single.op==Cmd::DrawElementsIndirect && single.a[0]==pi.gpu && single.a[1]==pa.gpu && single.n==5 && single.mask==2 && single.i[0]==1 && single.i[1]==20,"single ABI");
    check(gpu.empty(),"recording must not submit");
    check(recorded_draw_calls==1,"one recorded draw for the single indirect API call");
    if(argc>1){rejects([&]{replay_draw(single);});puts("PASS: indirect rejects disabled shared write tracking");return 0;}
    replay_draw(single);check(draws.empty() && indirect_calls==1,"draw stays deferred");run_gpu();
    check(draws.size()==1 && draws[0].first_index==7 && draws[0].args.vertexOffset==-5 && draws[0].args.firstInstance==9 && draws[0].args.instanceCount==2,"five original arguments preserved");reset();
    // GPU rewrites args and indices while guest backing stays stale. The actual
    // queued draw must consume both, including nonzero firstIndex and baseInstance.
    index_remaining=128;VkDrawIndexedIndirectCommand updated{2,3,11,-17,21};
    queued_write(args.data(),&updated,20);uint32_t index_value=0xface;
    queued_write(idx.data()+44,&index_value,4);
    replay_draw(single);check(std::memcmp(args.data(),&a,20)==0,"GPU args not available on host");run_gpu();
    check(draws.size()==1 && draws[0].args.vertexOffset==-17 && draws[0].args.firstInstance==21 && draws[0].first_index==index_value,"GPU args and GPU indices consumed");reset();
    // Native count reads pending GPU count. Native path never scans records above
    // actual count (uninitialized sentinel records can contain arbitrary values).
    std::memset(args.data(),0xff,args.size());std::memcpy(args.data(),&a,20);
    uint32_t two=2;queued_write(count.data(),&two,4);
    VkDrawIndexedIndirectCommand second{1,0,9,-2,0};queued_write(args.data()+32,&second,20);
    auto multi=invoke("nvnCommandBufferMultiDrawElementsIndirectCount",{0x999,4,2,pi.gpu,pa.gpu,pc.gpu,3,32});
    check(multi.op==Cmd::MultiDrawElementsIndirectCount && multi.a[2]==pc.gpu && multi.i[0]==3 && multi.i[1]==32,"count ABI all eight registers");
    check(recorded_draw_calls==2,"one recorded draw for a multi-draw API call");
    replay_draw(multi);check(count_calls==1 && count[0]==0,"count stays GPU data");run_gpu();
    check(draws.size()==2 && draws[0].args.indexCount==3 && draws[1].args.instanceCount==0,"GPU count, stride and zero instances preserved");reset();
    // CPU count fallback clamps to maximum and emits Vulkan indirect draws. Devices
    // without multiDrawIndirect use one call per record, not reconstructed draws.
    native_count=false;features.multiDrawIndirect=VK_FALSE;uint32_t large=99;std::memcpy(count.data(),&large,4);
    for(int i=0;i<3;++i)std::memcpy(args.data()+32*i,&a,20);
    replay_draw(multi);run_gpu();check(draws.size()==3 && indirect_calls==3 && !count_calls,"bounded CPU count fallback without multidraw");reset();
    // With no native indirect-count extension, production deliberately draws the
    // maximum when the count is still GPU-owned; it must not read stale host bytes.
    queued_write(count.data(),&two,4);replay_draw(multi);run_gpu();
    check(draws.size()==3 && indirect_calls==3 && !count_calls,"GPU count fallback draws maximum without a host read");reset();
    native_count=true;features.multiDrawIndirect=VK_TRUE;
    // A uint8 host index stream widens without altering firstIndex semantics.
    single.mask=0;idx[7]=0x7b;replay_draw(single);run_gpu();check(bound_type==VK_INDEX_TYPE_UINT16 && draws[0].first_index==0x7b,"8-bit CPU widening");reset();single.mask=2;
    // Virtual arguments crossing non-contiguous physical mappings are composed on
    // the GPU through the same shared-written tracker.
    auto& vp=make<MemoryPool>(4);vp.gpu=0x40000;vp.size=20;
    vp.mappings[0]={2,128,8};vp.mappings[8]={2,256,12};
    std::memcpy(args.data()+128,&a,8);std::memcpy(args.data()+256,reinterpret_cast<uint8_t*>(&a)+8,12);
    updated=a;updated.vertexOffset=-123;queued_write(args.data()+256,reinterpret_cast<uint8_t*>(&updated)+8,12);
    single.a[1]=vp.gpu;replay_draw(single);run_gpu();check(draws[0].args.vertexOffset==-123,"virtual mapping GPU composition");reset();single.a[1]=pa.gpu;
    auto invalid=multi;invalid.i[1]=19;rejects([&]{replay_draw(invalid);});
    invalid=multi;invalid.a[1]=UINT64_MAX-3;rejects([&]{replay_draw(invalid);});
    invalid=multi;invalid.i[0]=5;rejects([&]{replay_draw(invalid);});
    invalid=single;invalid.a[0]=pi.gpu+1;rejects([&]{replay_draw(invalid);});
    invalid=multi;invalid.i[0]=0;auto before=allocation_bytes;replay_draw(invalid);check(before==allocation_bytes,"zero maximum does no GPU work");
    auto normalized=invoke("nvnCommandBufferMultiDrawElementsIndirectCount",{0x999,4,2,pi.gpu,pa.gpu,pc.gpu,3,0xffffffff});check(normalized.i[1]==20,"SDK signed nonpositive stride normalization");
    check(vertex_preparations && barriers,"ordinary vertex preparation and memory dependencies used");
    puts("PASS: indirect HLE ABI, GPU args/count/indices, stride, count fallback, virtual mappings and validation");
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
