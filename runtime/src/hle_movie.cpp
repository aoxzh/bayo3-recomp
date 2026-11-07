// movie::DecoderImpl — the game's own Android-MediaCodec-style decoder (subsdk0) that today drives the faked nvdec.
//
// The plan (docs/EXTERNAL-REFERENCES.md) is to serve this layer from FFmpeg, the way AstralChain-PC does, so every
// movie plays independently of the faked nvdec (`ev0005` is opened but never fed — BUGS §2.10/§2.12). Before writing
// that, this file answers the one question the port depends on: **does the loader's HLE redirection reach these
// methods at all?** They are not imports: they are methods of a class in subsdk0, reached through that module's
// vtable, so they are only redirectable if the vtable entries are relocations the loader processes (`find_symbol`
// path). Each probe calls the real function, so a build with this file behaves exactly as before; only
// NX_MOVIE_PROBE=1 adds the lines.
//
// If the probes fire (they should, once per movie), the next step is to replace the data path:
//   SendInputBufferForDecode -> avcodec_send_packet on the guest's VP9 data
//   AcquireOutputBufferIndex/GetOutputBuffer -> avcodec_receive_frame + NV12 into the guest's output Buffer
//   RegisterClientEvents -> the four DecoderEvents the game waits on (input available, output available, format
//   changed, error), which is what its player thread blocks on.
#include <cstdio>
#include <cstdlib>

#include "kernel.h"

namespace nx {
namespace {

// Log at most `cap` lines per method, then call the guest's own implementation unchanged.
#define NX_MOVIE_PROBE(sym, label, cap)                                                     \
    void movie_probe_##label(CPU& c) {                                                      \
        static const bool on = std::getenv("NX_MOVIE_PROBE") != nullptr;                    \
        static int n = 0;                                                                   \
        if (on && n < (cap)) {                                                              \
            ++n;                                                                            \
            log("movie-probe: " #label " this %llx x1 %llx x2 %llx x3 %llx x4 %llx",        \
                (unsigned long long)c.x[0], (unsigned long long)c.x[1],                     \
                (unsigned long long)c.x[2], (unsigned long long)c.x[3],                     \
                (unsigned long long)c.x[4]);                                                \
        }                                                                                   \
        static uint64_t real = find_symbol(sym);                                            \
        if (real) {                                                                          \
            call(c, real);                                                                   \
        } else {                                                                             \
            static bool said = false;                                                        \
            if (!said) said = true, log("movie-probe: " #label " has NO real symbol");        \
        }                                                                                    \
    }                                                                                       \
    NX_HLE(sym, movie_probe_##label);

// OFF BY DEFAULT, and it must stay off until the seam is understood: with these registered the game no longer
// advances past the title screen (202 s of driving, 67 presses, no movie opened, no probe line printed — so the
// redirect happens somewhere that is not the player's data path and stalls it). The lesson is recorded in
// docs/archive/BAYONETTA3_BUGS.md §2.13: the decoder-implementation methods are NOT a safe HLE seam through the loader's
// relocation pass.
#if defined(NX_MOVIE_PROBE_BUILD)
NX_MOVIE_PROBE("_ZN5movie11DecoderImpl15GetOutputFormatEPNS_9MediaDataE", GetOutputFormat, 8)
NX_MOVIE_PROBE("_ZN5movie11DecoderImpl20RegisterClientEventsEPNS_13DecoderEventsE", RegisterClientEvents, 8)
NX_MOVIE_PROBE("_ZN5movie11DecoderImpl23AcquireInputBufferIndexEPiS1_", AcquireInputBufferIndex, 24)
NX_MOVIE_PROBE("_ZN5movie11DecoderImpl14GetInputBufferEiPNS_6BufferE", GetInputBuffer, 24)
NX_MOVIE_PROBE("_ZN5movie11DecoderImpl24SendInputBufferForDecodeEiiilj", SendInputBufferForDecode, 24)
NX_MOVIE_PROBE("_ZN5movie11DecoderImpl24AcquireOutputBufferIndexEPiPlPjS1_", AcquireOutputBufferIndex, 24)
NX_MOVIE_PROBE("_ZN5movie11DecoderImpl15GetOutputBufferEiPNS_6BufferE", GetOutputBuffer, 24)
NX_MOVIE_PROBE("_ZN5movie11DecoderImpl24ReleaseOutputBufferIndexEi", ReleaseOutputBufferIndex, 24)
NX_MOVIE_PROBE("_ZN5movie11DecoderImpl21GetOutputBufferFormatEiPNS_9MediaDataE", GetOutputBufferFormat, 8)
// The extractor side: with the decoder replaced, the packets still come from the game's own extractor.
NX_MOVIE_PROBE("_ZN5movie9Extractor4ReadEPNS_6BufferE", ExtractorRead, 24)
NX_MOVIE_PROBE("_ZN5movie9Extractor7AdvanceEv", ExtractorAdvance, 24)
NX_MOVIE_PROBE("_ZN5movie9Extractor13GetSampleTimeEPl", ExtractorGetSampleTime, 24)

#undef NX_MOVIE_PROBE
#endif  // NX_MOVIE_PROBE_BUILD

}  // namespace
}  // namespace nx
