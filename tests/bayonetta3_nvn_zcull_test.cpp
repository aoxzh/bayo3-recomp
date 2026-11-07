// Production ZCull HLE/replay with captured commands and Vulkan dependencies.
#include "../runtime/src/nvn/nvn_zcull.h"
#include "../runtime/src/nvn/nvn_draw_state.h"
#include <array>
#include <cstdio>
#include <stdexcept>

using namespace nx;
using namespace nx::nvn;
namespace {
std::map<std::string, HleFn>& registry() { static std::map<std::string, HleFn> r; return r; }
std::vector<Cmd> commands;
std::vector<char> events;
std::array<uint8_t, 128> depth;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
uint64_t invoke(const char* name, uint64_t a, uint64_t b=0, uint64_t c=0) {
    CPU cpu{}; cpu.x[0]=a; cpu.x[1]=b; cpu.x[2]=c;
    registry().at(name)(cpu); return cpu.x[0];
}
void VKAPI_PTR barrier(VkCommandBuffer,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags,
                       uint32_t n,const VkMemoryBarrier* b,uint32_t buffers,const VkBufferMemoryBarrier*,
                       uint32_t images,const VkImageMemoryBarrier*) {
    check(src==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,"ZCull ordering stages");
    check(n==1 && !buffers && !images,"ZCull must not transition images or copy metadata");
    check((b->srcAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) &&
          (b->dstAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT) &&
          (b->dstAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),"depth read/write dependency");
    events.push_back('B');
}
template<class F> void rejects(F f) {
    auto before=commands.size(), gpu=events.size();
    bool threw=false; try { f(); } catch(const std::runtime_error&) { threw=true; }
    check(threw && before==commands.size() && gpu==events.size(),"reject before recording/GPU mutation");
}
}
namespace nx {
HleRegistrar::HleRegistrar(const char* n,HleFn f) { check(registry().emplace(n,f).second,"duplicate HLE"); }
[[noreturn]] void fatal(const char*,...) { throw std::runtime_error("production fatal"); }
}
namespace nx::vk { PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier=::barrier; }
namespace nx::nvn {
Texture::~Texture()=default;
ObjectsLock& objects_lock() { static ObjectsLock l; return l; }
std::unordered_map<uint64_t,std::unique_ptr<Object>>& objects() { static std::unordered_map<uint64_t,std::unique_ptr<Object>> o; return o; }
bool resolve_cache_on() { return false; }
Object* find_object(uint64_t guest,uint64_t) { auto i=objects().find(guest);return i==objects().end()?nullptr:i->second.get(); }
void defer_object(std::unique_ptr<Object>) {}
void record(uint64_t cb,Cmd command,const void*,size_t) {
    check(cb==0x2000,"command-buffer ABI"); commands.push_back(command);
}
void draw_end_rendering(VkCommandBuffer,int) { events.push_back('E'); }
void clears_flush(VkCommandBuffer) { events.push_back('F'); }
}
int main() {
    try {
        auto& texture=make<Texture>(0x1000);
        texture.format=0x2A; texture.storage_size=depth.size();
        texture.memory=reinterpret_cast<uint64_t>(depth.data());
        depth.fill(0x9B);
        check(invoke("nvnTextureGetZCullStorageSize",0x1000)==0,"host has no transferable hierarchical depth metadata");
        rejects([&]{invoke("nvnTextureGetZCullStorageSize",0x1001);});
        invoke("nvnCommandBufferSaveZCullData",0x2000,UINT64_MAX,0);
        invoke("nvnCommandBufferRestoreZCullData",0x2000,0,0);
        check(commands.size()==2 && commands[0].op==Cmd::ZCullBarrier && commands[0].n==0 && commands[1].n==1,"save/restore markers retained in order");
        check(commands[0].a[0]==UINT64_MAX && !commands[0].a[1],"opaque GPU address is retained but never dereferenced for zero bytes");
        check(events.empty(),"recording cannot issue Vulkan work");
        for(const auto& command:commands) replay_zcull(VK_NULL_HANDLE,command);
        check(events==std::vector<char>({'E','F','B','E','F','B'}),"replay must end rendering and flush before dependency");
        for(auto b:depth)check(b==0x9B,"no fabricated depth or metadata modification");
        check(texture.storage_size==depth.size() && !texture.dirty,"depth storage size/dirtiness unchanged");
        rejects([&]{invoke("nvnCommandBufferSaveZCullData",0x2000,0,1);});
        rejects([&]{invoke("nvnCommandBufferRestoreZCullData",0x2000,0,1);});
        Cmd forged{Cmd::ZCullBarrier};forged.a[1]=32;
        rejects([&]{replay_zcull(VK_NULL_HANDLE,forged);});
        forged.a[1]=0;forged.n=2;
        rejects([&]{replay_zcull(VK_NULL_HANDLE,forged);});
        puts("bayonetta3_nvn_zcull_test: size-zero metadata, recording, ordering and depth preservation passed");
    }catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
