// Standalone: c++ -std=c++20 -pthread -Iruntime/include tests/opus_service_test.cpp -ldl -o opus_service_test
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <array>
#include "../runtime/src/svc_opus.cpp"

namespace {
std::map<std::string, nx::ServiceFactory>& factories() { static std::map<std::string, nx::ServiceFactory> f; return f; }
std::map<uint32_t, std::shared_ptr<nx::KObject>> handles;
bool fails(std::function<void()> fn) { try { fn(); } catch (const std::runtime_error&) { return true; } return false; }
struct Request {
    std::vector<uint8_t> raw;
    nx::IpcCtx ctx;
    Request(uint32_t cmd, int rate, int channels, uint32_t large = 0, uint32_t bytes = 0) {
        std::array<uint32_t, 5> values{uint32_t(rate), uint32_t(channels), large, 0, bytes};
        raw.resize(cmd == 4 ? 20 : 16); std::memcpy(raw.data(), values.data(), raw.size());
        ctx.cmd = cmd; ctx.in = raw.data(); ctx.in_size = raw.size();
    }
};
template<class T> T read(const std::vector<uint8_t>& data, size_t offset = 0) {
    assert(offset + sizeof(T) <= data.size()); T v; std::memcpy(&v, data.data() + offset, sizeof v); return v;
}
void be32(uint8_t* p, uint32_t x) { p[0] = x >> 24; p[1] = x >> 16; p[2] = x >> 8; p[3] = x; }
struct Encoder;
struct NativeDecoder;
}
namespace nx {
void register_service(const std::string& name, ServiceFactory f) { assert(factories().emplace(name, std::move(f)).second); }
uint32_t Service::handle(IpcCtx&) { throw std::runtime_error("unexpected base service fallback"); }
std::shared_ptr<KObject> handle_get(uint32_t h) { auto it = handles.find(h); return it == handles.end() ? nullptr : it->second; }
void log(const char* f, ...) { va_list ap; va_start(ap, f); vprintf(f, ap); va_end(ap); puts(""); }
[[noreturn]] void fatal(const char* f, ...) {
    char s[1024]; va_list ap; va_start(ap, f); vsnprintf(s, sizeof s, f, ap); va_end(ap); throw std::runtime_error(s);
}
}
int main() {
    void* lib = dlopen("libopus.so.0", RTLD_NOW | RTLD_LOCAL); assert(lib);
    auto create_encoder = reinterpret_cast<Encoder*(*)(int32_t,int,int,int*)>(dlsym(lib,"opus_encoder_create"));
    auto encode = reinterpret_cast<int(*)(Encoder*,const int16_t*,int,unsigned char*,int32_t)>(dlsym(lib,"opus_encode"));
    auto encoder_ctl = reinterpret_cast<int(*)(Encoder*,int,...)>(dlsym(lib,"opus_encoder_ctl"));
    auto destroy_encoder = reinterpret_cast<void(*)(Encoder*)>(dlsym(lib,"opus_encoder_destroy"));
    auto create_decoder = reinterpret_cast<NativeDecoder*(*)(int32_t,int,int*)>(dlsym(lib,"opus_decoder_create"));
    auto decode = reinterpret_cast<int(*)(NativeDecoder*,const unsigned char*,int32_t,int16_t*,int,int)>(dlsym(lib,"opus_decode"));
    auto destroy_decoder = reinterpret_cast<void(*)(NativeDecoder*)>(dlsym(lib,"opus_decoder_destroy"));
    assert(create_encoder && encode && encoder_ctl && destroy_encoder && create_decoder && decode && destroy_decoder);
    auto manager = factories().at("hwopus")();
    for (int rate : {8000,12000,16000,24000,48000}) for (int channels : {1,2}) for (int large : {0,1}) {
        Request sizing(5, rate, channels, large);
        assert(manager->handle(sizing.ctx) == 0 && sizing.ctx.out.size() == 4);
        uint32_t exact = read<uint32_t>(sizing.ctx.out);
        assert(exact == uint32_t(nx::opus().size(channels)) && exact > 0);
        uint32_t size = (exact + 4095) & ~4095u;
        void* work = std::aligned_alloc(4096, size); assert(work);
        std::memset(work, 0xa5, size);
        auto tm = std::make_shared<nx::KTransferMemory>(); tm->addr = reinterpret_cast<uint64_t>(work); tm->size = size;
        handles[42] = tm;
        Request open(4, rate, channels, large, size); open.ctx.in_copy = {42};
        assert(manager->handle(open.ctx) == 0 && open.ctx.out_objects.size() == 1);
        auto decoder = std::move(open.ctx.out_objects.front()); open.ctx.out_objects.clear();
        assert(std::memcmp(work, std::vector<uint8_t>(exact, 0xa5).data(), exact) != 0);
        for (uint32_t i = exact; i < size; ++i) assert(static_cast<uint8_t*>(work)[i] == 0xa5);
        handles.erase(42); // copied transfer handle may close; service retains KTransferMemory
        const int frames = large ? rate * 60 / 1000 : rate / 50; // exercise packets beyond default 40 ms
        std::vector<int16_t> source(frames * channels), pcm(source.size(), 123), reference(source.size());
        for (int i=0;i<frames;++i) for(int ch=0;ch<channels;++ch)
            source[i*channels+ch] = int16_t(std::sin(i * 6.283185307179586 * (440 + ch*110) / rate) * 12000);
        int error = -999;
        Encoder* encoder = create_encoder(rate, channels, 2049, &error); assert(encoder && error == 0);
        std::vector<uint8_t> packet(4096);
        int bytes = encode(encoder, source.data(), frames, packet.data()+8, int(packet.size()-8)); assert(bytes > 0);
        uint32_t range = 0; assert(encoder_ctl(encoder, 4031, &range) == 0 && range != 0);
        be32(packet.data(), uint32_t(bytes)); be32(packet.data()+4, range); packet.resize(bytes+8);
        destroy_encoder(encoder);
        NativeDecoder* native = create_decoder(rate, channels, &error); assert(native && error == 0);
        assert(decode(native,packet.data()+8,bytes,reference.data(),frames,0) == frames);
        destroy_decoder(native);
        uint8_t reset = 0;
        auto request = [&] {
            nx::IpcCtx ctx; ctx.cmd = 8; ctx.in = &reset; ctx.in_size = 1;
            ctx.a.push_back({reinterpret_cast<uint64_t>(packet.data()),packet.size()});
            ctx.b.push_back({reinterpret_cast<uint64_t>(pcm.data()),pcm.size()*2}); return ctx;
        };
        // Input guards: failures have no scalar output, PCM writes or decoder-history changes.
        std::vector<uint8_t> initial(exact); std::memcpy(initial.data(),work,exact);
        auto truncated = request(); truncated.a[0].size = 7;
        assert(decoder->handle(truncated) == 0x226f && truncated.out.empty());
        auto short_output = request(); short_output.b[0].size -= 2;
        assert(fails([&]{decoder->handle(short_output);}));
        assert(std::memcmp(initial.data(),work,exact) == 0);
        assert(std::all_of(pcm.begin(),pcm.end(),[](auto v){return v==123;}));
        // Valid packet with wrong nonzero final range must roll back the actual decode.
        be32(packet.data()+4, range ^ 1);
        auto mismatch = request();
        assert(decoder->handle(mismatch) == 0x226f && mismatch.out.empty());
        assert(std::memcmp(initial.data(),work,exact) == 0);
        assert(std::all_of(pcm.begin(),pcm.end(),[](auto v){return v==123;}));
        be32(packet.data()+4, range);
        auto first = request(); assert(decoder->handle(first) == 0);
        assert(first.out.size()==16 && read<int32_t>(first.out)==bytes+8 && read<int32_t>(first.out,4)==frames);
        assert(read<int64_t>(first.out,8)>=0);
        assert(pcm==reference && std::any_of(pcm.begin(),pcm.end(),[](auto v){return v!=0;}));
        reset = 1; auto repeat = request(); assert(decoder->handle(repeat)==0 && pcm==reference);
        be32(packet.data()+4,0); auto zero_range=request(); assert(decoder->handle(zero_range)==0 && pcm==reference);
        auto unknown=request(); unknown.cmd=9; assert(fails([&]{decoder->handle(unknown);}));
        decoder.reset(); tm.reset(); std::free(work);
        printf("PASS %d Hz %dch large=%d: size=%u init/nonzero PCM/reference/reset/range/error isolation\n",rate,channels,large,exact);
    }
    for (auto values : {std::array<int,3>{44100,2,0},{48000,6,0},{48000,2,2}}) {
        Request invalid(5,values[0],values[1],values[2]); assert(fails([&]{manager->handle(invalid.ctx);}));
    }
    Request short_input(5,48000,2); short_input.ctx.in_size=8; assert(fails([&]{manager->handle(short_input.ctx);}));
    Request wrong_handle(4,48000,2,0,4096); wrong_handle.ctx.in_copy={7};
    assert(fails([&]{manager->handle(wrong_handle.ctx);}) && wrong_handle.ctx.out_objects.empty());
    auto tiny=std::make_shared<nx::KTransferMemory>(); tiny->addr=0x1000; tiny->size=4096; handles[7]=tiny;
    assert(fails([&]{manager->handle(wrong_handle.ctx);}) && wrong_handle.ctx.out_objects.empty());
    Request unknown(7,48000,2); assert(fails([&]{manager->handle(unknown.ctx);}));
    puts("PASS malformed manager inputs, unsupported formats/commands and undersized transfer memory fail closed");
}
