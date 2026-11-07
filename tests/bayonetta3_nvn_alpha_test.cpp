#include "nvn/nvn_shaders.h"
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstdlib>
int main(int argc,char** argv){
    if(argc!=5)return 2;
    std::ifstream in(argv[1],std::ios::binary|std::ios::ate);
    auto size=in.tellg();if(size<0||size%4)return 3;
    std::vector<uint32_t> words(size_t(size)/4);in.seekg(0);in.read(reinterpret_cast<char*>(words.data()),size);
    std::vector<uint32_t> out;
    if(!nx::nvn::patch_alpha_test(words,std::strtoul(argv[3],nullptr,0),std::strtof(argv[4],nullptr),out))return 4;
    std::ofstream f(argv[2],std::ios::binary);f.write(reinterpret_cast<const char*>(out.data()),out.size()*4);
}
