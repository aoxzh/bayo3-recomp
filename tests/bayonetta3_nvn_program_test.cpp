// Production NVN Program HLE: deferred original stages must never execute as a subset.
#include "../runtime/src/nvn/nvn.h"
#include "../runtime/src/nvn/nvn_shaders.h"
// Keep production control/hash parsing; substitute only shader cache availability.
#define shader_for unused_runtime_shader_for
#include "../runtime/src/nvn/nvn_shaders.cpp"
#undef shader_for
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
using namespace nx;
using namespace nx::nvn;
namespace {
std::map<std::string,HleFn>& registry(){static std::map<std::string,HleFn> r;return r;}
ShaderInfo translated;
bool missing=true;
size_t object_lookups=0;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
uint64_t invoke(const char* n,uint64_t a,uint64_t b=0,uint64_t d=0){CPU c{};c.x[0]=a;c.x[1]=b;c.x[2]=d;registry().at(n)(c);return c.x[0];}
template<class F>void rejects(F f){bool threw=false;try{f();}catch(const std::runtime_error&){threw=true;}check(threw,"pending selected stage must fail");}
}
namespace nx {
HleRegistrar::HleRegistrar(const char* n,HleFn f){registry().emplace(n,f);}
void log(const char*,...){}
[[noreturn]]void fatal(const char*,...){throw std::runtime_error("production fatal");}
bool strict_runtime(){return true;}
std::string describe(uint64_t){return "test";}
}
namespace nx::nvn {
ObjectsLock& objects_lock(){static ObjectsLock l;return l;}
std::unordered_map<uint64_t,std::unique_ptr<Object>>& objects(){static std::unordered_map<uint64_t,std::unique_ptr<Object>> o;return o;}
Object* find_object(uint64_t g,uint64_t){++object_lookups;auto it=objects().find(g);return it==objects().end()?nullptr:it->second.get();}
bool resolve_cache_on(){return false;}
void defer_object(std::unique_ptr<Object>){}
void note_replaced(uint64_t){}
MemoryPool* pool_at_gpu(uint64_t a,uint64_t* off){auto* p=get<MemoryPool>(0x1000);if(p&&a>=p->gpu&&a-p->gpu<p->size){*off=a-p->gpu;return p;}return nullptr;}
const ShaderInfo* shader_for(uint64_t,uint64_t){return missing?nullptr:&translated;}
}
int main(int argc,char**){try{
 const bool strict=argc>1;
 if(strict) setenv("NX_STRICT_SHADERS","1",1); else unsetenv("NX_STRICT_SHADERS");
 const size_t before_null=object_lookups;
 for(uint32_t mask:{0u,1u,2u,4u,8u,16u,32u,0xffffffffu})
     check(program_require_complete(0,mask),"unbound stage is complete");
 check(object_lookups==before_null,"unbound stage must not resolve an object");
 auto pending_fails=[&](uint32_t mask){
     if(strict) rejects([&]{program_require_complete(0x2000,mask);});
     else check(!program_require_complete(0x2000,mask),"pending selected stage must skip the draw");
 };
 std::vector<uint8_t> code(0x100),control(0x880);
 auto put=[&](size_t o,uint32_t n){std::memcpy(control.data()+o,&n,4);};
 put(0,kShaderControlMagic);put(0x704,code.size());put(0x708,0x30);
 uint32_t sph=2u<<10;std::memcpy(code.data()+0x30,&sph,4);
 auto& pool=make<MemoryPool>(0x1000);pool.gpu=0x100000;pool.memory=reinterpret_cast<uint64_t>(code.data());pool.size=code.size();
 uint64_t entry[2]={pool.gpu,reinterpret_cast<uint64_t>(control.data())};
 check(invoke("nvnProgramInitialize",0x2000)==1,"program init");
 check(invoke("nvnProgramSetShaders",0x2000,1,reinterpret_cast<uint64_t>(entry))==1,"valid missing translation may preload");
 check(program_shaders(0x2000)&&program_shaders(0x2000)->empty(),"trace lookup need not execute original");
 check(program_require_complete(0x2000,1)&&program_require_complete(0x2000,2)&&program_require_complete(0x2000,0x20),"unselected pending graphics stage remains usable");
 pending_fails(8);
 translated.stage=ShaderStage::TessellationControl;missing=false;
 invoke("nvnProgramSetShaders",0x2000,1,reinterpret_cast<uint64_t>(entry));
 check(program_require_complete(0x2000,8)&&program_shaders(0x2000)->size()==1,"new complete stages replace pending state");
 missing=true;put(0x708,0);sph=0;std::memcpy(code.data(),&sph,4);
 invoke("nvnProgramSetShaders",0x2000,1,reinterpret_cast<uint64_t>(entry));
 check(program_require_complete(0x2000,0x1f),"unselected pending compute stage remains usable");pending_fails(0x20);
 puts("bayonetta3_nvn_program_test: null lookup bypass, original preload, selected-stage skip/strict rejection and replacement passed");
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
