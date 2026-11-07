// Production upload/written/draw_inputs paths with deferred fake GPU operations.
#include "../runtime/src/nvn/nvn_draw_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>

using namespace nx;
using namespace nx::nvn;
namespace {
std::vector<std::function<void()>> gpu;
std::vector<std::unique_ptr<std::vector<uint8_t>>> allocations;
std::vector<std::vector<uint8_t>> images;
bool bad_astc_metadata = false;
unsigned astc_decoder_calls = 0;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void run_gpu() { for (auto& f : gpu) f(); gpu.clear(); }
void VKAPI_PTR copy(VkCommandBuffer, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* ranges) {
    for (uint32_t k=0;k<n;++k) { auto r=ranges[k];gpu.emplace_back([=]{std::memcpy(reinterpret_cast<uint8_t*>(dst)+r.dstOffset,reinterpret_cast<const uint8_t*>(src)+r.srcOffset,r.size);}); }
}
void VKAPI_PTR barrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*) {}
void VKAPI_PTR properties(VkPhysicalDevice,VkPhysicalDeviceProperties* p) { p->limits.minStorageBufferOffsetAlignment=256;p->limits.minUniformBufferOffsetAlignment=256; }
void VKAPI_PTR to_image(VkCommandBuffer,VkBuffer src,VkImage,VkImageLayout,uint32_t n,const VkBufferImageCopy* ranges) {
    check(n==1,"one pitch-linear upload");auto r=ranges[0];
    gpu.emplace_back([=]{std::vector<uint8_t> pixels;for(uint32_t y=0;y<r.imageExtent.height;++y){auto* p=reinterpret_cast<uint8_t*>(src)+r.bufferOffset+y*(r.bufferRowLength?r.bufferRowLength:r.imageExtent.width);pixels.insert(pixels.end(),p,p+r.imageExtent.width);}images.push_back(std::move(pixels));});
}
void VKAPI_PTR clear_color(VkCommandBuffer,VkImage,VkImageLayout,const VkClearColorValue*,uint32_t,const VkImageSubresourceRange*) { throw std::runtime_error("unexpected image clear"); }
void VKAPI_PTR clear_depth(VkCommandBuffer,VkImage,VkImageLayout,const VkClearDepthStencilValue*,uint32_t,const VkImageSubresourceRange*) { throw std::runtime_error("unexpected depth clear"); }
template<class F> void rejects(F f) { auto n=gpu.size();bool threw=false;try{f();}catch(const std::runtime_error&){threw=true;}check(threw,"unsupported CPU read must fail");check(n==gpu.size(),"rejection precedes GPU work"); }
void queued_write(const uint8_t* p,uint64_t n,uint8_t value) {auto u=replay::written_bind(VK_NULL_HANDLE,p,n,n);gpu.emplace_back([=]{std::memset(u.ptr,value,n);});}
}
namespace nx {
void log(const char*,...) {}
[[noreturn]] void fatal(const char*,...) {throw std::runtime_error("production fatal");}
}
namespace nx::vk {
// No persistent Vulkan allocation is available in this test; exercise the normal
// upload-ring fallback. A retained-buffer regression has its own real fake allocator.
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
PFN_vkCmdCopyBuffer p_vkCmdCopyBuffer=copy;
PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier=::barrier;
PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties=properties;
PFN_vkCmdCopyBufferToImage p_vkCmdCopyBufferToImage=to_image;
PFN_vkCmdClearColorImage p_vkCmdClearColorImage=clear_color;
PFN_vkCmdClearDepthStencilImage p_vkCmdClearDepthStencilImage=clear_depth;
PFN_vkCmdDispatchIndirect p_vkCmdDispatchIndirect=nullptr;
}
namespace nx::plat { uint64_t monotonic_ns(){return 0;} }
namespace nx::nvn {
Texture::~Texture() = default;
Upload upload_alloc(uint64_t n,uint64_t) {auto v=std::make_unique<std::vector<uint8_t>>(n);auto* p=v->data();allocations.push_back(std::move(v));return {reinterpret_cast<VkBuffer>(p),0,p};}
Upload staging_alloc(uint64_t n){return upload_alloc(n,16);}
Upload upload(const void* p,uint64_t n,uint64_t a){auto u=upload_alloc(n,a);std::memcpy(u.ptr,p,n);return u;}
VkPhysicalDevice vk_physical(){return VK_NULL_HANDLE;}
VkDevice vk_device(){return VK_NULL_HANDLE;}
uint32_t memory_type(uint32_t,VkMemoryPropertyFlags){return 0;}
bool vk_host_import(){return false;}
bool async_gpu(){return false;}
void frame_wait_all(){throw std::runtime_error("unexpected retained-buffer wait");}
void frame_carry_wait(){}
Upload host_import_upload(const void*,uint64_t,uint64_t){return {};}
void draw_end_rendering(VkCommandBuffer,int) {}
void transition(VkCommandBuffer,GpuImage& img,VkImageLayout layout){img.layout=layout;}
bool gpu_profile_on(){return false;}
void gpu_profile_mark(VkCommandBuffer,const std::string&) {}
uint64_t frame_ring_size(){return 0;}
uint64_t frame_upload_used(){return 0;}
void note_astc_decoded(uint64_t) {}
bool bc_host_decode(uint32_t){return false;}
uint32_t bc_decoded_bytes(uint32_t){return 4;}
void bc_decode(uint32_t,const uint8_t*,uint32_t,uint32_t,uint32_t,uint8_t*){throw std::runtime_error("unexpected BC decode");}
bool astc_decoded(uint32_t f){return f==0x79;}
uint64_t astc_decode_image(const uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,bool,uint8_t*){++astc_decoder_calls;throw std::runtime_error("unexpected ASTC decode");}
void format_block(uint32_t f,uint32_t& w,uint32_t& h,uint32_t& b){w=h=b=1;if(f==0x79)b=bad_astc_metadata?4:16;}
uint32_t vk_block_bytes(VkFormat){return 1;}
VkIndexType vk_index_type(uint32_t n){return n==0?VK_INDEX_TYPE_UINT8_EXT:VK_INDEX_TYPE_UINT16;}
uint32_t index_size(uint32_t n){return n==0?1:2;}
namespace replay {
State g_state;
// No dispatch commands are replayed in this isolated upload test.
void compute_write_barrier(VkCommandBuffer){}
const uint8_t* guest_bytes(uint64_t a){return reinterpret_cast<const uint8_t*>(a);}
void dedup_invalidate(const uint8_t*,uint64_t) {}
Upload dedup_upload(const uint8_t* p,uint64_t n,uint64_t padded,uint64_t align, bool){auto u=upload_alloc(padded,align);std::memcpy(u.ptr,p,n);return u;}
}
}
int main(){try{
    unsetenv("NX_COMPUTE_SEPARATE");unsetenv("NX_FULL_UPLOAD");
    std::vector<uint8_t> memory(512,0x11);
    Texture t;t.memory=reinterpret_cast<uint64_t>(memory.data());t.storage_size=32;t.stride=8;t.width=8;t.height=4;
    GpuImage img;img.width=8;img.height=4;img.format=VK_FORMAT_R8_UNORM;
    upload_texture(VK_NULL_HANDLE,t,img);run_gpu();check(images.back()==std::vector<uint8_t>(32,0x11),"CPU-only upload unchanged");
    queued_write(memory.data()+8,8,0x7a);
    check(replay::written_texture_stale(t,img),"cached texture invalidated by partial GPU write");
    upload_texture(VK_NULL_HANDLE,t,img,0,8); // CPU dirtiness must not hide GPU-written second row.
    check(!replay::written_texture_stale(t,img),"upload records revision once");
    auto first_version=img.uploaded_write_version;
    queued_write(memory.data()+8,8,0x9b);
    check(replay::written_texture_stale(t,img),"later in-place write invalidates again");
    upload_texture(VK_NULL_HANDLE,t,img);
    check(img.uploaded_write_version>first_version,"reused tracking allocation advances revision");
    check(memory[8]==0x11,"no eager guest writeback");run_gpu();
    for(size_t i=0;i<32;++i){check(images[1][i]==(i>=8&&i<16?0x7a:0x11),"earlier upload snapshot preserves ordered bytes and CPU gaps");check(images[2][i]==(i>=8&&i<16?0x9b:0x11),"later upload sees latest GPU bytes");}
    queued_write(memory.data()+16,8,0x55);
    img.rendered=true;check(!replay::written_texture_stale(t,img),"rendered image authority preserved");img.rendered=false;
    img.texels=true;check(!replay::written_texture_stale(t,img),"WriteTexels authority preserved");img.texels=false;
    auto reject_texture=[&]{auto n=img.uploaded_write_version;rejects([&]{upload_texture(VK_NULL_HANDLE,t,img);});check(n==img.uploaded_write_version,"failed conversion leaves cache revision unchanged");};
    t.stride=0;reject_texture();t.stride=8;img.alpha_from_r=true;reject_texture();img.alpha_from_r=false;
    t.format=0x3f;img.format=VK_FORMAT_R16G16B16A16_SFLOAT;reject_texture();t.format=0;img.format=VK_FORMAT_R8_UNORM;
    ShaderInfo vs;Cmd c{};c.a[0]=t.memory;c.i[0]=32;c.mask=0;replay::DrawInputs inputs;
    rejects([&]{replay::draw_inputs(VK_NULL_HANDLE,&vs,true,c,inputs);});
    c.a[0]=t.memory+64;c.i[0]=3;memory[64]=0;memory[65]=127;memory[66]=255;
    check(replay::draw_inputs(VK_NULL_HANDLE,&vs,true,c,inputs),"CPU u8 widening supported");
    auto* indices=reinterpret_cast<uint16_t*>(inputs.index.ptr);check(indices[0]==0&&indices[1]==127&&indices[2]==255&&inputs.index_type==VK_INDEX_TYPE_UINT16,"u8 widening unchanged");
    run_gpu();auto previous=img.uploaded_write_version;replay::written_reset();
    check(!replay::written_texture_stale(t,img),"retired tracking does not dirty persistent image");
    queued_write(memory.data(),8,0x88);check(replay::written_texture_version(memory.data(),32)>previous,"versions monotonic across submit resets");
    check(replay::written_texture_stale(t,img),"new submit cannot reuse stale cache revision");
    run_gpu();replay::written_reset();
    // Fully tracked source, aligned and directly bindable, must still re-upload after another write.
    queued_write(memory.data(),32,0x66);upload_texture(VK_NULL_HANDLE,t,img);run_gpu();
    check(images.back()==std::vector<uint8_t>(32,0x66),"fully tracked upload reads GPU source");
    auto version=img.uploaded_write_version;replay::written_count_bind();check(replay::written_texture_version(memory.data(),32)>version,"graphics storage writes invalidate texture revision");
    run_gpu();replay::written_reset();
    // Reproduce the B3 startup defect: inherited SDK table defaults supplied
    // 1x1/4 metadata to the ASTC path. It must reject before decoder input reads.
    std::vector<uint8_t> tiled(512);
    Texture astc;astc.memory=reinterpret_cast<uint64_t>(tiled.data());astc.storage_size=tiled.size();astc.format=0x79;
    GpuImage astc_img;astc_img.width=4;astc_img.height=4;astc_img.format=VK_FORMAT_R8G8B8A8_UNORM;
    bad_astc_metadata=true;
    rejects([&]{upload_texture(VK_NULL_HANDLE,astc,astc_img);});
    check(astc_decoder_calls==0,"invalid ASTC footprint rejected before decoder reads");
    puts("bayonetta3_nvn_host_consumers_test: ordered uploads, cache revisions, CPU conversion guards and ASTC footprint rejection passed");
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
