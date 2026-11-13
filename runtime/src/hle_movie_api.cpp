// movie:: served from FFmpeg by patching the concrete decoder's vtable in the loaded image.
//
// THE PROBLEM (docs/archive/LINUX-VERIFICATION-2026-10-22.md §2, §8, §12):
//
// Bayonetta 3 plays cutscenes through the game's own Android-MediaCodec-style layer (`movie::`), which lives entirely
// inside subsdk0. `movie::NnCodec` is its NVDEC front end: it drives /dev/nvhost-nvdec, and our runtime answers those
// ioctls with a faked decoder, so the output planes stay blank until runtime/src/movie.cpp fills them from ffmpeg on
// the game's `WriteTexels` call. That works for `ev0000` and stops dead for `ev0005`: the game opens the file, opens
// two nvdec and two VIC sessions, and never issues another ioctl or WriteTexels. We wait for a decoder the game never
// drives.
//
// THE SEAM (§12): the vtables are static data with relocations, so the slot maps were read offline from
// gen/subsdk0/table.cpp + analysis/subsdk0.bin — no probe build needed:
//
//   movie::NnCodec   (concrete decoder, 28 slots, _ZTVN5movie7NnCodecE  @ module+0x550fa0)
//   movie::Extractor (demuxer,         18 slots, _ZTVN5movie9ExtractorE @ module+0x5518d0)
//   movie::Decoder   (the interface the player holds, 21 slots; every method is a 20-byte stub)
//
// The vtables sit in the module's `data` segment, which the loader maps read/write (loader.cpp: vm_set(... data ...,
// kMemCodeData, 3)), so they can be overwritten **in place after load** with no constructor hook at all: every
// NnCodec the game ever constructs then dispatches into host code. That is why this file patches data instead of
// registering constructor HLEs — the constructors are not imports, and §2.13 already proved method-level NX_HLE
// cannot reach a virtual call.
//
// STATUS: `NX_MOVIE_VTABLE=log` installs a logging table and tail-calls the game's own implementation for every slot,
// so a log build behaves exactly like a normal one. Serving frames from FFmpeg (the `NX_MOVIE_FFMPEG` build) reuses
// the same table with real implementations; it needs FFmpeg linked into the runtime and is not written yet.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kernel.h"
#include "movie.h"  // nx::movie_ledger: the resource walk/formatting, unit-tested in tests/movie_ledger_test.cpp
#include "movie_frame_source.h"
#include "platform.h"

namespace nx {

// Installed on the first cutscene open; defined with the other vtable installers below.
namespace plat { void install_decoder_probe(); }

// kernel_watch.cpp's guest call chain (AArch64 frame records, every frame described with its nearest symbol).
// Declared here because the only users are the crash report and this diagnostic log.
void backtrace(CPU& c);

namespace {

// Module-relative addresses of the two vtables, and their sizes (docs/archive/LINUX-VERIFICATION-2026-10-22.md §12).
// Kept next to the slot names below so the two stay in step; a mismatch is caught by the sanity check in
// patch_movie_vtables().
constexpr uint64_t kNnCodecVtable = 0x550fa0ull;  // _ZTVN5movie7NnCodecE, 28 slots
constexpr int kNnCodecSlots = 28;
constexpr uint64_t kExtractorVtable = 0x5518d0ull;  // _ZTVN5movie9ExtractorE, 18 slots
constexpr int kExtractorSlots = 18;
// The demuxer the game actually uses: NnExtractor derives from Extractor, and Extractor's slots alone only see the
// setup calls (docs/archive/LINUX-VERIFICATION-2026-10-22.md §15.1 — NnCodec is never called at all, so the sample path is
// this one).
constexpr uint64_t kNnExtractorVtable = 0x550c40ull;  // _ZTVN5movie10NnExtractorE, 31 slots
constexpr int kNnExtractorSlots = 31;
// The player: the class that owns the pull loop. NnExtractor delivers 4 samples and then nothing asks again
// (docs/archive/LINUX-VERIFICATION-2026-10-22.md §16), so the stop has to be visible in this object's slots.
constexpr uint64_t kNnCodecPlayerVtable = 0x551158ull;  // _ZTVN5movie13NnCodecPlayerE, 46 slots
constexpr int kNnCodecPlayerSlots = 46;
// The player's own event plumbing. `NnCodecPlayer::InitializeOutputReceivers` creates these message handlers, and
// the decoder side posts input/output-available, format-changed and error messages to them — so their
// onMessageReceived is the *visible* half of the handshake that never starts for ev0005. The callbacks
// (VideoCodecCallback) are the other half: OnInputBufferAvailable/OnOutputBufferAvailable are what makes a player
// pull the next sample. Patching them answers "did the decoder ever answer for movie 2" directly.
constexpr uint64_t kVideoCodecCallbackVtable = 0x551308ull;  // _ZTVN5movie18VideoCodecCallbackE, 8 slots
constexpr int kVideoCodecCallbackSlots = 8;
constexpr uint64_t kNnCodecMessageReceiverVtable = 0x551080ull;  // _ZTVN5movie22NnCodecMessageReceiverE, 9 slots
constexpr int kNnCodecMessageReceiverSlots = 9;
constexpr uint64_t kNnCodecVideoOutReceiverVtable = 0x551470ull;  // _ZTVN5movie26NnCodecVideoOutputReceiverE, 9
constexpr int kNnCodecVideoOutReceiverSlots = 9;
constexpr uint64_t kNnCodecAudioOutReceiverVtable = 0x551500ull;  // _ZTVN5movie26NnCodecAudioOutputReceiverE, 9
constexpr int kNnCodecAudioOutReceiverSlots = 9;
// The controller (main_00CD6318) keeps the stream source it asks for data at this+0x160; the object is the
// ClientStreamReader* passed to Extractor::SetDataSource and its class vtable is a main-module local symbol, so it
// is discovered at runtime from the object's vptr rather than from a table symbol.
constexpr int kStreamReaderMaxSlots = 32;
// The decoder interface the player actually drives: run 5 showed the object the controller keeps at +0x160 has
// _ZTVN5movie7DecoderE, and the tick's first pull step calls its slot 11 (AcquireInputBufferIndex). The base methods
// are thin delegators to the real implementation at this+8 (a PLT tail branch) or return 0x1000/0x100c/0x2002 when
// that member is absent — so the return code here is what decides whether the player ever pulls a sample.
constexpr uint64_t kDecoderVtable = 0x551730ull;  // _ZTVN5movie7DecoderE, 21 slots
constexpr int kDecoderSlots = 21;
// The decoder-creation gate at main_00CD5BF0+0x3b4..0x3c8 is
//     MediaData::FindString(md, "mime", &value) && !strncasecmp(value, "video/x-vnd.on2.vp9", 19)
// and both MediaData lookups are *imports* of main, reached through PLT stubs (main_01321BD0 / main_01321B70),
// whose GOT slots are relocations in main's table. Redirecting those GOT entries logs the key and the value the
// game got back without registering an NX_HLE (which is what §2.13's stalled probe did).
constexpr uint64_t kMainFindStringGot = 0x249b698ull;  // reloc -> movie::MediaData::FindString(char const*, char const**)
constexpr uint64_t kMainFindInt32Got = 0x249b6b0ull;   // reloc -> movie::MediaData::FindInt32(char const*, int*)
constexpr uint64_t kMainTryWaitEventGot = 0x249b6d0ull;  // reloc -> nn::os::TryWaitEvent(EventType*)
constexpr uint64_t kMainClearEventGot = 0x249b6d8ull;    // reloc -> nn::os::ClearEvent(EventType*)
constexpr uint64_t kMainSignalEventGot = 0x249bb40ull;   // reloc -> nn::os::SignalEvent(EventType*)
// The decoder that signals the four client events lives in subsdk0 and calls nn::os::SignalEvent through *its* GOT,
// not main's, so patching only main's slot observes nothing (run 6). Its slot is found by content (below), not by a
// relocation index: a parsed index pointed at 0x577a48, which holds std::mutex's destructor, and patching that hung
// the boot — the same class of mistake as trusting a clobbered register.
// The pull loop only runs when controller+0x168 is signalled (main_00CD6318: TryWaitEvent(ctl+0x168); if signalled,
// ClearEvent then decoder->AcquireInputBufferIndex...). NX_MOVIE_SHIM_EVENT=1 pretends that event is signalled for
// the active movie controller, i.e. it reports "the stream has data", which the runtime knows to be true (the file
// source is fed by the FS HLE and the host ffmpeg decode). It is a diagnostic and a candidate fix for the case where
// the game's own reader never raises it for the second movie.

// Slot names, verbatim from the resolved relocations. Used for the log and for the per-slot dispatch.
const char* const kNnCodecSlotNames[kNnCodecSlots] = {
    "(vbase 0)", "(vbase 1)",
    "Configure(MediaData*, vi::Layer*, void*)",  // 2
    "SetCallback(CodecCallback*)",               // 3
    "SetInputLayer(vi::Layer*)",                 // 4
    "Start()",                                   // 5
    "Stop()",                                    // 6
    "Reset()",                                   // 7
    "Release()",                                 // 8
    "Flush()",                                   // 9
    "GetInputBuffer(int, Buffer*)",              // 10
    "QueueInputBuffer(int,int,int,int64,uint32)",// 11
    "GetOutputBuffer(int, Buffer*)",             // 12
    "ReleaseOutputBuffer(int)",                  // 13
    "RenderOutputBufferAndRelease(int)",         // 14
    "RenderOutputBufferAndRelease(int,int64)",   // 15
    "SignalEndOfInputStream()",                  // 16
    "GetOutputFormat(MediaData*) const",         // 17
    "GetInputFormat(MediaData*) const",          // 18
    "GetOutputFormat(int, MediaData*)",          // 19
    "GetName(char*, int*) const",                // 20
    "CopyOutputBufferAndRelease(int,int)",       // 21
    "GetStride(int*,int*,int*)",                 // 22
    "SetShadowBuffer(Buffer*, int)",             // 23
    "SetShadowBuffersParam(int,int)",            // 24
    "~NnCodec() (D2)",                           // 25
    "~NnCodec() (D0)",                           // 26
    "RequestIdrFrame()",                         // 27
};

const char* const kExtractorSlotNames[kExtractorSlots] = {
    "(vbase 0)", "(vbase 1)",
    "~Extractor() (D2)",                          // 2
    "~Extractor() (D0)",                          // 3
    "SetDataSource(const char*)",                 // 4
    "SetDataSource(const char*, int)",            // 5
    "GetTrackCount(int*)",                        // 6
    "GetTrackConfiguration(int, MediaData*)",     // 7
    "SelectTrack(int)",                           // 8
    "UnselectTrack(int)",                         // 9
    "SeekTo(int64)",                              // 10
    "GetTrackIndexForAvailableData(size_t*)",     // 11
    "Read(Buffer*)",                              // 12
    "Advance()",                                  // 13
    "GetSampleTime(int64*)",                      // 14
    "GetSampleMetaData(MediaData*)",              // 15
    "GetBufferedRange(int64*, int64*)",           // 16
    "SetDataSource(ClientStreamReader*, const char*)",  // 17
};

// NnExtractor slot names, from tools/movie_vtables.py --class NnExtractor.
const char* const kNnExtractorSlotNames[kNnExtractorSlots] = {
    "(vbase 0)", "(vbase 1)",
    "SetDataSource()",                              // 2
    "SetDataSource()",                              // 3
    "SetDataSource()",                              // 4
    "CountTracks(int*) const",                      // 5
    "GetTrackFormat(int, MediaData*) const",        // 6
    "GetFileFormat(MediaData*) const",              // 7
    "SelectTrack(int)",                             // 8
    "UnselectTrack(int)",                           // 9
    "SeekTo(int64)",                                // 10
    "Advance()",                                    // 11
    "ReadSampleData(Buffer*)",                      // 12
    "GetSampleTrackIndex(size_t*)",                 // 13
    "GetSampleTime(int64*)",                        // 14
    "GetSampleTime(int64*)",                        // 15
    "GetSampleMeta(MediaData*)",                    // 16
    "GetFragmentIndex(int*)",                       // 17
    "GetCachedDuration(int64*) const",              // 18
    "CancelSetDataSource()",                        // 19
    "IsCacheFull()",                                // 20
    "IsSeekable() const",                           // 21
    "TerminateNetworkConnection()",                 // 22
    "GetMediaStatistics(MediaData*)",               // 23
    "ResetMediaStatistics()",                       // 24
    "~NnExtractor() (D2)",                          // 25
    "~NnExtractor() (D0)",                          // 26
    "SetDataSource()",                              // 27
    "SeekTo(int64)",                                // 28
    "GetBufferedRange(int64*, int64*) const",       // 29
    "setBufferUpdateNotify(void*)",                 // 30
};

// NnCodecPlayer slot names, from tools/movie_vtables.py --class NnCodecPlayer.
const char* const kNnCodecPlayerSlotNames[kNnCodecPlayerSlots] = {
"(vbase 0)",
"(vbase 1)",
"SetObserver",
"GetObserver",
"SetDataSource",
"SetExtractor",
"CancelSetDataSource",
"GetVolume",
"SetVolume",
"Prepare",
"Start",
"Stop",
"Pause",
"SeekTo",
"GetCurrentPlaybackPosition",
"GetPlaybackDuration",
"GetTrackCount",
"SelectTrack",
"DeSelectTrack",
"GetTrackType",
"GetVideoDimensions",
"Reset",
"SetLooping",
"GetLooping",
"GetPlaybackRate",
"SetPlaybackRate",
"GetState",
"GetTrackInfo",
"AcquireOutputBufferIndex",
"GetOutputBufferSize",
"FillOutputBuffer",
"ReleaseOutputBufferIndex",
"SetOutputBufferAlignment",
"SeekTo",
"GetOutputBufferProperties",
"GetOutputBufferProperties",
"TerminateNetworkConnection",
"RegisterOutputBuffer",
"GetMediaStatistics",
"~NnCodecPlayer (D2)",
"~NnCodecPlayer (D0)",
"SetCacheSize",
"IsCacheFull",
"GetBufferedRange",
"SetObserver",
"GetObserver"
};

// movie::VideoCodecCallback — the decoder-facing callbacks (input/output buffer available, format changed).
const char* const kVideoCodecCallbackSlotNames[kVideoCodecCallbackSlots] = {
    "(vbase 0)", "(vbase 1)", "On", "OnInputBufferAvailable", "OnOutputBufferAvailable",
    "OnOutputFormatChanged", "CodecCallback (base)", "~VideoCodecCallback",
};

// movie::Decoder — the real decode interface (V1.5 §12.3 called these "20-byte stubs"; they are delegators).
const char* const kDecoderSlotNames[kDecoderSlots] = {
    "(vbase 0)", "(vbase 1)", "~Decoder (D2)", "~Decoder (D0)", "Configure(MediaData*)",
    "RegisterClientEvents(DecoderEvents*)", "GetOutputFormat(MediaData*)", "Start()", "Stop()", "Reset()",
    "Flush()", "AcquireInputBufferIndex(int*,int*)", "GetInputBuffer(int,Buffer*)",
    "SendInputBufferForDecode(int,int,int,int64,uint32)", "AcquireOutputBufferIndex(int*,int64*,uint32*,int*)",
    "GetOutputBuffer(int,Buffer*)", "GetOutputBufferSize(int,uint64_t*)", "ReleaseOutputBufferIndex(int)",
    "GetLast(int64*)", "GetOutputBuffer(MediaData*, Buffer*)", "GetOutputBufferFormat(int,MediaData*)",
};

// The message receivers: only slot 8 (onMessageReceived) is a real method; 2..7 are the RefBase/ctor/dtor slots.
const char* const kNnCodecMessageReceiverSlotNames[kNnCodecMessageReceiverSlots] = {
    "(vbase 0)", "(vbase 1)", "~NnCodecMessageReceiver (D2)", "~NnCodecMessageReceiver (D0)",
    "RefBase::onFirstRef", "RefBase::onLastStrongRef", "RefBase::onIncStrongAttempted",
    "RefBase::onLastWeakRef", "onMessageReceived",
};
const char* const kNnCodecVideoOutReceiverSlotNames[kNnCodecVideoOutReceiverSlots] = {
    "(vbase 0)", "(vbase 1)", "~NnCodecVideoOutputReceiver (D2)", "~NnCodecVideoOutputReceiver (D0)",
    "RefBase::onFirstRef", "RefBase::onLastStrongRef", "RefBase::onIncStrongAttempted",
    "RefBase::onLastWeakRef", "onMessageReceived",
};
const char* const kNnCodecAudioOutReceiverSlotNames[kNnCodecAudioOutReceiverSlots] = {
    "(vbase 0)", "(vbase 1)", "~NnCodecAudioOutputReceiver (D2)", "~NnCodecAudioOutputReceiver (D0)",
    "RefBase::onFirstRef", "RefBase::onLastStrongRef", "RefBase::onIncStrongAttempted",
    "RefBase::onLastWeakRef", "onMessageReceived",
};

enum Which {
    kWhichNnCodec = 0,
    kWhichExtractor = 1,
    kWhichNnExtractor = 2,
    kWhichPlayer = 3,
    kWhichVideoCallback = 4,
    kWhichNnCodecMsg = 5,
    kWhichVideoOut = 6,
    kWhichAudioOut = 7,
    kWhichStream = 8,  // the game's ClientStreamReader, vtable discovered at runtime (local symbol, not exported)
    kWhichDecoder = 9,  // _ZTVN5movie7DecoderE: the interface the player's tick dispatches through
    // The movie *controller* (the object main_00CD6318 ticks; its class vtable is a main-local symbol, discovered
    // from the live object the same way): the tick (main+cd6318), the renderer's readiness query (main+cd77e4) and
    // its frame fetch (main+cd786c) are virtual methods of it, so this table is where "the renderer never fetches"
    // becomes a function name and a caller.
    kWhichController = 10,
    kWhichCount = 11,
};
constexpr int kWhichClasses = int(kWhichCount);

// Per-slot state: the game's original function, and how many times the slot has been called.
struct SlotState {
    uint64_t real = 0;
    std::atomic<int> calls{0};
};
SlotState g_slots[kWhichClasses][64];

// Per-object attribution. A class-wide count cannot say which movie a call belongs to: the previous movie's decoder
// and controller objects (and the thread that polls them) outlive their movie while the next one starts, so the
// honest unit is the object. Each class tracks up to 8 distinct `this` values, and the profile printed at every
// movie open/close lists every slot of every object — zeros included — so "the renderer never fetched for ev0005"
// is a line carrying ev0005's own controller address.
constexpr int kTrackedObjects = 8;
constexpr int kTrackedSlots = 24;
struct ObjectSlots {
    std::atomic<uint64_t> obj{0};
    std::atomic<uint32_t> calls[kTrackedSlots];
};
ObjectSlots g_objects[kWhichClasses][kTrackedObjects];

int object_index(int which, uint64_t self) {
    for (int i = 0; i < kTrackedObjects; i++) {
        const uint64_t v = g_objects[which][i].obj.load(std::memory_order_relaxed);
        if (v == self) return i;
        if (v == 0) {
            uint64_t expected = 0;
            if (g_objects[which][i].obj.compare_exchange_strong(expected, self, std::memory_order_relaxed))
                return i;
            if (expected == self) return i;
        }
    }
    return -1;
}

constexpr int kLogPerSlot = 4;

const char* class_name(int which) {
    switch (which) {
        case kWhichNnCodec: return "NnCodec";
        case kWhichExtractor: return "Extractor";
        case kWhichNnExtractor: return "NnExtractor";
        case kWhichPlayer: return "Player";
        case kWhichVideoCallback: return "VideoCb";
        case kWhichNnCodecMsg: return "NnCodecMsg";
        case kWhichVideoOut: return "VideoOutMsg";
        case kWhichAudioOut: return "AudioOutMsg";
        case kWhichStream: return "Stream";
        case kWhichDecoder: return "Decoder";
        case kWhichController: return "Ctl";
        default: return "?";
    }
}

const char* slot_name(int which, int slot) {
    switch (which) {
        case kWhichNnCodec: return slot < kNnCodecSlots ? kNnCodecSlotNames[slot] : "(out of range)";
        case kWhichExtractor: return slot < kExtractorSlots ? kExtractorSlotNames[slot] : "(out of range)";
        case kWhichNnExtractor: return slot < kNnExtractorSlots ? kNnExtractorSlotNames[slot] : "(out of range)";
        case kWhichPlayer: return slot < kNnCodecPlayerSlots ? kNnCodecPlayerSlotNames[slot] : "(out of range)";
        case kWhichVideoCallback:
            return slot < kVideoCodecCallbackSlots ? kVideoCodecCallbackSlotNames[slot] : "(out of range)";
        case kWhichNnCodecMsg:
            return slot < kNnCodecMessageReceiverSlots ? kNnCodecMessageReceiverSlotNames[slot] : "(out of range)";
        case kWhichVideoOut:
            return slot < kNnCodecVideoOutReceiverSlots ? kNnCodecVideoOutReceiverSlotNames[slot] : "(out of range)";
        case kWhichAudioOut:
            return slot < kNnCodecAudioOutReceiverSlots ? kNnCodecAudioOutReceiverSlotNames[slot] : "(out of range)";
        case kWhichStream: return "slot";
        case kWhichDecoder: return slot < kDecoderSlots ? kDecoderSlotNames[slot] : "(out of range)";
        case kWhichController: return "slot";  // named at call time from the function it holds
        default: return "(unknown)";
    }
}

// NX_MOVIE_VTABLE_CAP overrides the per-slot log budget (default 4); 0 logs nothing. NX_MOVIE_VTABLE_BT is how
// many guest call chains to print per slot (default 1, the first call), which is what says *who* is calling and
// from where — the ev0005 question is which layer stops asking, not which slot exists.
int env_int(const char* name, int fallback) {
    const char* e = std::getenv(name);
    if (!e || !*e) return fallback;
    const int v = std::atoi(e);
    return v < 0 ? fallback : v;
}

// ---- ClientStreamReader slot 4: the preload window, and the null release it can walk the game into ---------------
//
// `android::FileSource::FileSource` (subsdk0+0x2bef6c, +0x40) calls ClientStreamReader slot 4 (`main_00CD7BAC`) to
// open the movie file and preload a window of it into one guest-heap buffer. The class is main-local; its vtable was
// recovered from the relocations of `main+0x1c495a0` (slot 2 = reset `main_00CD7B2C`, slot 3 = dtor `main_00CD7B70`,
// slot 4 = this call, slot 5 = readAt `main_00CD7C68`, slot 6 = close `main_00CD7D7C`, slot 7 = getSize
// `main_00CD7DA8`). Slot 4 is, exactly:
//     if (!OpenFile(&[reader+0x10], path, 1)) return 0;              // main_00CBC220
//     if (!GetFileSize(&[reader+0x18], handle)) { CloseFile; return 0; }
//     window = min([reader+0x48], file_size);                        // [reader+0x48] = the preload window
//     buffer = main_00CD39BC([reader+0x28], window, 0, 0);           // from the movie memory resource (below)
//     if (!ReadFile(handle, 0, buffer, window)) { release(buffer); ...; return 0; }   // main_00CBC2A0
//     [reader+0x30] = buffer; [reader+0x38] = 0; [reader+0x40] = window; *out = reader+8; return 1;
//
// The window is a read-ahead *cache* size, not a requirement: slot 5 (the FileSource's readAt, `main_00CD7C68`) uses
// the same field as its capacity and serves any read that does not fit in the window with a direct
// nn::fs::ReadFile at that offset (`cmp x19, x9; b.ls 0xcd7cc8` else ReadFile(handle, offset, dst, size)). The movie
// layer sets it to a fixed 0x200000 (2 MiB) in the controller's load command handler
// (`main_00CD5BF0`, code_269.cpp:9461-9462: `mov w8,#0x200000; str x8,[x19,#0x50]`, x19 = controller = reader-8) and
// clears it again on reset (`main_00CD7B2C`, `main_00CD6204`), so 0 is a state this class already runs in.
//
// The buffer comes from the game's movie memory resource: `[reader+0x28]` is a copy of the global at `main+0x24ac628`
// (code_269.cpp:9458), main_00CD39BC -> main_00CD1F9C creates a node through that resource's create method and then
// through its fallback list, and the payload at [node+0x28] is what the reader keeps at [reader+0x30]. When the
// resource refuses — and by the fourth movie of a run it does: the crash run reached ev0020 after ev0000/ev0005/
// ev0010/ev0011 had each been through the same handlers — main_00CD39BC returns 0 and the cleanup path at 0xcd7c40
// calls the shared-allocation release main_00CD3B20 on that NULL, whose first instruction is `ldur x19, [x0, #-8]`.
// That is the SIGSEGV at data address 0xfffffffffffffff8 (docs/archive/MOVIE-EV0005.md §18.5, §20): a guest-side
// out-of-memory that the guest does not handle, not a bad pointer from the runtime.
//
// The runtime cannot make the guest's pool bigger and cannot patch the generated cleanup, but it can stop the guest
// from asking: this guard sets the window to NX_MOVIE_PRELOAD bytes (default 0) before slot 4 runs. With 0 the
// allocation and the ReadFile are both zero-length — a zero-length ReadFile cannot fail, so the fatal branch is
// unreachable — and every read afterwards takes slot 5's direct-ReadFile path, which is the path the FileSource uses
// for any read outside the window anyway. NX_MOVIE_PRELOAD=<bytes> restores a read-ahead cache for A/B runs, and
// NX_MOVIE_PRELOAD=off (or keep / -1) leaves the guest's own 2 MiB request alone (the pre-fix behaviour).
constexpr uint64_t kStreamWindowOffset = 0x48;  // [reader+0x48] = preload window / readAt cache capacity
constexpr uint64_t kStreamBufferOffset = 0x30;  // [reader+0x30] = the preloaded buffer (a shared allocation)
constexpr uint64_t kStreamSizeOffset = 0x18;    // [reader+0x18] = the file size from nn::fs::GetFileSize
constexpr uint64_t kStreamPreloadDefault = 0;   // no read-ahead cache: the preload's allocation is the crash input
constexpr uint64_t kStreamResourceOffset = 0x28;  // [reader+0x28] = the movie memory resource (holder)
// The resource's own ledger, from the guest objects: `[holder]` is the resource object, every shared allocation it
// handed out is an intrusive-list node with the owning resource at +0x10 and the size it was asked for at +0x38; the
// newest node is [resource+0x68] and the live count is [resource+0x80] (main_00CD1F9C adds, main_00CD3B20 subtracts),
// and a node's +0x20 links to the next older one (the node's +0x18 points the other way, so the head's +0x18 is 0).
// Those offsets, the walk and the size histogram are in movie.h's nx::movie_ledger, because the walk is pure
// bookkeeping and is unit-tested without a game run (tests/movie_ledger_test.cpp). Reading the list turns "the
// allocation failed" into "the pool holds N allocations / M bytes and has R bytes of tail left", which is the
// leak-versus-genuinely-full question (docs/archive/MOVIE-EV0005.md §21).

uint64_t preload_cap() {
    static const uint64_t cap = [] {
        const char* e = std::getenv("NX_MOVIE_PRELOAD");
        if (!e || !*e) return kStreamPreloadDefault;
        if (!std::strcmp(e, "off") || !std::strcmp(e, "keep") || !std::strcmp(e, "-1")) return ~uint64_t(0);
        return uint64_t(std::strtoull(e, nullptr, 0));
    }();
    return cap;
}

// Called BEFORE the guest implementation, because the failure it prevents happens inside that call and is fatal.
// Returns the window the guest asked for, for the report after the call.
uint64_t stream_preload_guard(uint64_t reader) {
    if (!reader || vm_query(reader).state == 0) return 0;
    if (vm_query(reader + kStreamWindowOffset).state == 0) return 0;
    const uint64_t asked = ld<uint64_t>(reader + kStreamWindowOffset);
    const uint64_t cap = preload_cap();
    if (cap == ~uint64_t(0) || asked <= cap) return asked;  // guard disabled, or the request is already small
    if (!(vm_query(reader + kStreamWindowOffset).perm & 2)) return asked;
    st<uint64_t>(reader + kStreamWindowOffset, cap);
    log("movie: read-ahead window %llu -> %llu bytes (reader %llx): no window allocation from the movie memory "
        "resource, so the guest cannot hit the out-of-memory whose cleanup segfaults (MOVIE-EV0005 §20); every read "
        "goes straight to the file through stream slot 5 (NX_MOVIE_PRELOAD=<bytes> restores a window)",
        (unsigned long long)asked, (unsigned long long)cap, (unsigned long long)reader);
    return asked;
}

// One line per movie open and per movie close: what the movie memory resource is holding at that moment, plus one
// more line at open with the pool's tail and the live set's size classes (see movie_ledger in movie.h). Only reads
// guest memory, every pointer is validated, and the walk is capped, so it cannot fault or stall the run.
//
// The pair of lines is the measurement docs/archive/MOVIE-EV0005.md §21 needs: open(N+1) minus close(N) is what the gap
// between two movies allocates, and close(N) minus open(N) is what one movie allocates and does not give back
// before it closes. A live count that only ever goes up across those pairs is retention, not a high-water mark.
void log_resource_ledger(uint64_t reader, const char* when, bool shape) {
    if (!reader || vm_query(reader).state == 0) return;
    const uint64_t holder = ld<uint64_t>(reader + kStreamResourceOffset);
    if (!holder || vm_query(holder).state == 0) return;
    const uint64_t res = ld<uint64_t>(holder);
    if (!res || vm_query(res).state == 0) return;
    const uint32_t count = ld<uint32_t>(res + movie_ledger::kResourceCountOffset);
    const uint64_t head = ld<uint64_t>(res + movie_ledger::kResourceListOffset);
    // Every node lives inside the resource's arena, so one accepted mapped range answers the next few thousand
    // `mapped` questions. Without this the walk takes g_vm_lock once per node — 32768 of them, since the live set
    // has outgrown the old 8192 cap — which is pure lock contention with the guest's own allocator threads. Only a
    // positive answer is cached, and it is cached for the exact range vm_query accepted, so a bad link still stops
    // the walk (the test asserts that the walk never reads an address `mapped` rejected).
    uint64_t ok_lo = 0, ok_hi = 0;
    const movie_ledger::Snapshot snap = movie_ledger::walk(
        head, count, movie_ledger::kWalkCap, [](uint64_t a) { return ld<uint64_t>(a); },
        [&ok_lo, &ok_hi](uint64_t a) {
            if (a >= ok_lo && a < ok_hi) return true;
            const VmInfo v = vm_query(a);
            if (!v.state) return false;
            ok_lo = v.addr;
            ok_hi = v.addr + v.size;
            return true;
        });
    log("movie: movie memory resource%s (reader %llx): %u live allocations, %d walked, %llu bytes requested, largest "
        "%llu B%s", when, (unsigned long long)reader, snap.declared, snap.walked, snap.bytes, snap.largest,
        snap.disagrees() ? " (walk truncated or the count moved: bytes are a lower bound)" : "");
    if (!shape) return;
    // The tail the pool can still hand out (0 = the two fields no longer look like a top/end pair, so nothing is
    // claimed), and the live set by size class: "the growth is a few big buffers" and "the growth is thousands of
    // small ones" are different bugs and the counts/bytes line alone cannot tell them apart.
    unsigned long long room = 0;
    const bool have_room = movie_ledger::pool_room(ld<uint64_t>(res + movie_ledger::kPoolTopOffset),
                                                  ld<uint64_t>(res + movie_ledger::kPoolEndOffset), &room);
    char buckets[192];
    int used = 0;
    buckets[0] = 0;
    for (int i = 0; i < movie_ledger::kBuckets; i++) {
        if (!snap.bucket_count[i]) continue;
        const int left = int(sizeof buckets) - used;
        if (left <= 1) break;
        const int n = std::snprintf(buckets + used, size_t(left), "%s%s %d (%lluK)", used ? " | " : "",
                                    movie_ledger::bucket_label(i), snap.bucket_count[i],
                                    snap.bucket_bytes[i] / 1024ull);
        if (n <= 0 || n >= left) {  // truncated: stop with the buffer still terminated rather than index past it
            used = int(sizeof buckets) - 1;
            buckets[used] = 0;
            break;
        }
        used += n;
    }
    if (have_room)
        log("movie: movie memory shape%s (reader %llx): pool top %llx end %llx, %llu bytes of tail left; live set by "
            "size: %s", when, (unsigned long long)reader,
            (unsigned long long)ld<uint64_t>(res + movie_ledger::kPoolTopOffset),
            (unsigned long long)ld<uint64_t>(res + movie_ledger::kPoolEndOffset), room, buckets);
    else
        log("movie: movie memory shape%s (reader %llx): pool top/end %llx/%llx do not look like an arena pair; live "
            "set by size: %s", when, (unsigned long long)reader,
            (unsigned long long)ld<uint64_t>(res + movie_ledger::kPoolTopOffset),
            (unsigned long long)ld<uint64_t>(res + movie_ledger::kPoolEndOffset), buckets);
}

// Called AFTER the guest implementation: says, per movie, whether the file was opened and how the preload went.
// `ret` is the slot's return value (1 = the FileSource got its source), `[reader+0x30]` is the buffer it was given.
void stream_preload_report(uint64_t reader, uint64_t asked, uint64_t ret) {
    static std::atomic<int> lines{0};
    static std::atomic<int> failures{0};
    if (!reader || vm_query(reader).state == 0) return;
    const uint64_t buffer = ld<uint64_t>(reader + kStreamBufferOffset);
    const uint64_t window = ld<uint64_t>(reader + kStreamWindowOffset);
    const uint64_t size = ld<uint64_t>(reader + kStreamSizeOffset);
    if (ret != 0) {
        if (lines.fetch_add(1) >= 8) return;
        log("movie: source open ok (reader %llx): file %llu B, window %llu B (guest asked %llu B), buffer %llx%s",
            (unsigned long long)reader, (unsigned long long)size, (unsigned long long)window,
            (unsigned long long)asked, (unsigned long long)buffer,
            window ? "" : " (window 0: every read goes straight to the file through stream slot 5)");
        log_resource_ledger(reader, "", true);
        return;
    }
    if (failures.fetch_add(1) >= 8) return;
    log("movie: SOURCE OPEN FAILED (reader %llx): file %llu B, window %llu B (guest asked %llu B), buffer %llx, ret %llu"
        " -- main_00CD39BC gave the reader no buffer and nn::fs::ReadFile(handle,0,NULL,window) is what returned the"
        " error; the guest's cleanup then releases that NULL (main_00CD3B20 `ldur x19,[x0,#-8]`) and segfaults"
        " (MOVIE-EV0005 §20). NX_MOVIE_PRELOAD=0 preloads nothing, which removes the allocation and the read",
        (unsigned long long)reader, (unsigned long long)size, (unsigned long long)window,
        (unsigned long long)asked, (unsigned long long)buffer, (unsigned long long)ret);
    log_resource_ledger(reader, "", true);
}

// The controller: Extractor::SetDataSource's first argument is the ClientStreamReader embedded at controller+8, so
// reader-8 is the controller main_00CD6318 ticks. Remembered so the gate can print its state and the media object.
std::atomic<uint64_t> g_controller{0};

// The out-parameter values of the calls that carry the answer to "why does the player not start". The call
// clobbers x0..x3, so the thunk captures the object, the argument pointers and the caller's frame pointer first; the
// pointers are PODs in the caller's guest stack. The start condition in main_00CD6318 is
//     GetBufferedRange(&start, &end) -> Result == 0 && end >= 0xc350 (50000)
// (main+cd635c..: cmp w0,#0; ccmp x8,#0xc350,#8,eq; b.ge), so `end` is the number to watch.
//
// The caller keeps the movie controller in x19; main_00CD6318's prologue saves it at fp+0x38 (stp x20,x19,[sp,#0x90]
// with fp = sp+0x60), so the controller pointer can be recovered and verified against controller+0x158, which is the
// Extractor the call was made on. Its state fields are what the gate and the pull loop branch on.
void log_out_params(int which, int slot, uint64_t self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                    uint64_t fp) {
    auto i64 = [](uint64_t p) { return p ? (long long)ld<int64_t>(p) : 0LL; };
    auto u64 = [](uint64_t p) { return p ? (unsigned long long)ld<uint64_t>(p) : 0ULL; };
    if ((which == kWhichExtractor && slot == 16) || (which == kWhichNnExtractor && slot == 29)) {
        log("movie-vt:   GetBufferedRange outs: start %lld end %lld", i64(a1), i64(a2));
        const uint64_t ctl = g_controller.load(std::memory_order_relaxed);
        if (ctl && vm_query(ctl).state != 0)
            log("movie-vt:   controller %llx state: flag3a4 %u flag3c4 %u flag380 %u q2a0 %llx q2a8 %llx "
                "q2b0 %llx q2b8 %llx q2c0 %llx q2c8 %llx extractor %llx media %llx", (unsigned long long)ctl,
                ld<uint32_t>(ctl + 0x3a4), ld<uint32_t>(ctl + 0x3c4), ld<uint32_t>(ctl + 0x380),
                u64(ctl + 0x2a0), u64(ctl + 0x2a8), u64(ctl + 0x2b0), u64(ctl + 0x2b8), u64(ctl + 0x2c0),
                u64(ctl + 0x2c8), u64(ctl + 0x158), u64(ctl + 0x160));
        else
            log("movie-vt:   controller unknown (fp %llx)", (unsigned long long)fp);
        // The media object the tick asks first (controller+0x160): name its vtable and the function behind each of
        // the slots the tick uses, so its "no data" decision can be read offline.
        const uint64_t media = ctl ? ld<uint64_t>(ctl + 0x160) : 0;
        if (media && vm_query(media).state != 0) {
            const uint64_t mvptr = ld<uint64_t>(media);
            if (mvptr && module_at(mvptr)) {
                const uint64_t mvt = mvptr - 16;
                const LoadedModule* mod = module_at(mvptr);
                log("movie-vt:   media object %llx vtable %llx (module %s)", (unsigned long long)media,
                    (unsigned long long)mvt, mod->info->name);
                for (int sl = 2; sl <= 14; sl++) {
                    const uint64_t fn = mvt ? ld<uint64_t>(mvt + uint64_t(sl) * 8) : 0;
                    log("movie-vt:     media slot %2d -> %s", sl, describe(fn).c_str());
                }
            }
        }
    } else if ((which == kWhichExtractor && slot == 11) || (which == kWhichNnExtractor && slot == 13)) {
        log("movie-vt:   available/track out: %llu", u64(a1));
    } else if ((which == kWhichExtractor && slot == 14) || (which == kWhichNnExtractor && slot == 14)) {
        log("movie-vt:   sample time out: %lld", i64(a1));
    } else if ((which == kWhichExtractor && slot == 12) || (which == kWhichNnExtractor && slot == 12)) {
        log("movie-vt:   buffer: base %llx capacity %llx offset/size %llx", u64(a1), u64(a1 + 8), u64(a1 + 16));
    } else if (which == kWhichPlayer && slot == 14) {
        log("movie-vt:   playback position out: %lld", i64(a1));
    } else if (which == kWhichDecoder && slot == 11) {
        // AcquireInputBufferIndex(int* index, int* remaining)
        log("movie-vt:   decoder input index/remaining outs: %d %d",
            a1 ? ld<int32_t>(a1) : 0, a2 ? ld<int32_t>(a2) : 0);
    } else if (which == kWhichDecoder && slot == 14) {
        // AcquireOutputBufferIndex(int* index, int64* pts, uint32* flags, int* remaining). The PTS is int64 in this
        // ABI: printing it as int32 (as the first instrument did) truncates any movie time above 35 minutes and made
        // the 0/33000/67000 numbers impossible to trust.
        log("movie-vt:   decoder output index/ps outs: %d %lld flags %u remaining %d",
            a1 ? ld<int32_t>(a1) : 0, a2 ? (long long)ld<int64_t>(a2) : 0LL, a3 ? ld<uint32_t>(a3) : 0u,
            a4 ? ld<int32_t>(a4) : 0);
    } else if (which == kWhichStream && (slot == 11 || slot == 10 || slot == 12)) {
        log("movie-vt:   stream call outs: word1 %llx word2 %llx", u64(a1), u64(a2));
    }
}

// Defined after patch_vtable()/the thunk tables, declared here because dispatch() installs them on first sight.
void patch_stream_reader(uint64_t reader);
// Defined with the controller helpers below; called from the event poll so the timing is a running movie's, not a
// registration's.
int attach_controller(uint64_t ctl);
// The Decoder probe is installed on the first Extractor::SetDataSource (not at boot): its vtable is reached from
// the boot-time NvMMLite path, and the movie's own Configure/Start calls all happen after the file source opens.
void install_decoder_probe();  // plat:: (defined with the other vtable installers)

// MediaData lookups (see the GOT constants above): log the key, tail-call the real imported function from the GOT
// slot saved at install time, then log the answer and the string/int it returned.
uint64_t g_find_string_real = 0, g_find_int32_real = 0;
bool g_media_probes = false;
// The default (NX_MOVIE_VTABLE unset): only movie::Decoder is patched, and only to observe the game's own frame fetch
// (slot 15, movie_frame_source). Nothing in the game's behaviour changes: no other table, no stream preload window,
// no host-sent events, no logging. The full study mode (log/1) patches everything as before. A run with the full mode
// stalled at the Prologue's post-ev0011 load (ev0011 never closed) while the frame-only mode is all the movie path
// needs: the fetch names the film, its timestamp and the two plane buffers, so no film has to be guessed.
bool g_frame_only = false;

std::string guest_string(uint64_t p) {
    if (!p) return {};
    const VmInfo v = vm_query(p);
    if (v.state == 0 || !(v.perm & 1)) return "(unreadable)";
    return std::string(reinterpret_cast<const char*>(p));
}

void media_find_string(CPU& c) {
    const std::string key = guest_string(c.x[1]);
    // The out-parameter pointer has to be taken BEFORE the call: the real FindString writes through it and is free
    // to clobber x2, and reading x2 afterwards reported a stale pointer's string (the empty "mime" value in the old
    // logs may well be that artifact).
    const uint64_t out = c.x[2];
    static std::atomic<int> logged{0};
    const bool want = g_media_probes && logged.fetch_add(1) < 400;
    if (want) log("movie-vt: MediaData::FindString(key %s) lr %s", key.c_str(), describe(c.x[30]).c_str());
    if (g_find_string_real) call(c, g_find_string_real);
    if (want) {
        const bool found = c.x[0] != 0;
        const uint64_t value = found && out ? ld<uint64_t>(out) : 0;
        log("movie-vt:   FindString(%s) -> %s out %llx value \"%s\"", key.c_str(), found ? "found" : "NOT FOUND",
            (unsigned long long)out, guest_string(value).c_str());
    }
}

uint64_t g_try_wait_real = 0, g_clear_event_real = 0;
bool g_shim_event = false;

// A log budget per controller: the previous movie's thread keeps polling its own (now stale) controller and would
// otherwise eat the whole budget (run 9's mistake). Reset again at every movie open (movie_vtable_new_session).
struct EventLogBudget {
    uint64_t ctl = 0;
    int used = 0, seen = 0;
    uint32_t last_sig = ~0u;
    bool take(uint64_t c, int limit) {
        if (c != ctl) {
            ctl = c;
            used = 0;
            seen = 0;
            last_sig = ~0u;
        }
        ++seen;
        if (used >= limit) return false;
        ++used;
        return true;
    }
    // The event phase at 60 fps polls twice a frame, so a plain "first N" window covers a fraction of a second. Log
    // the first 16 polls of a controller, then only when the state word changes, then a heartbeat every 60th poll:
    // that follows a whole movie without flooding the log.
    bool take_on_change(uint64_t c, uint32_t sig, int limit) {
        if (c != ctl) {
            ctl = c;
            used = 0;
            seen = 0;
            last_sig = ~0u;
        }
        ++seen;
        const bool want = used < 16 || sig != last_sig || seen % 60 == 0;
        if (!want || used >= limit) return false;
        ++used;
        last_sig = sig;
        return true;
    }
};
EventLogBudget g_try_budget, g_clear_budget, g_signal_budget, g_shim_budget;

// The renderer does not touch the controller directly: it walks a registered list of movie systems
// (main_00CDACD0: list object at main+0x24ac6b8, nodes with next at +8, the movie system at node+0x18) and asks
// each one's `this+0x330` for a frame -- the controller, through slots 9 (ready) and 11 (fetch). So "the renderer
// painted nothing" can mean "the movie system was never bound to this controller". Read the binding here, read-only.
void log_movie_systems() {
    const uint64_t slot = kModuleBase + 0x24ac6b8ull;  // the list object (main_00CDACD0's adrp/ldr)
    if (!module_at(slot)) {
        log("movie-vt:   movie system list slot %llx is not in a module", (unsigned long long)slot);
        return;
    }
    const uint64_t list = ld<uint64_t>(slot);
    if (!list || vm_query(list).state == 0) {
        log("movie-vt:   no movie system list (%llx)", (unsigned long long)list);
        return;
    }
    const uint64_t sentinel = ld<uint64_t>(list + 0x28);
    if (!sentinel || vm_query(sentinel).state == 0) {
        log("movie-vt:   movie system list %llx has no sentinel", (unsigned long long)list);
        return;
    }
    // Change-triggered: the binding is what matters and it changes a handful of times per movie, so a bounded
    // "first N polls" window (0.3 s at 120 polls a second) would miss it. Print only when it differs.
    uint64_t sig = 0x9e3779b97f4a7c15ull * list;
    uint64_t node = ld<uint64_t>(sentinel + 8);
    struct Row { uint64_t ms, ctl, disp, f148; uint32_t flags, f1c8; };
    Row rows[4];
    int nrow = 0;
    for (int i = 0; i < 4 && node && node != sentinel && vm_query(node).state != 0 && nrow < 4; i++) {
        const uint64_t ms = node + 0x18;
        rows[nrow++] = {ms, ld<uint64_t>(ms + 0x330), ld<uint64_t>(ms + 0x338), ld<uint64_t>(ms + 0x148),
                        ld<uint32_t>(ms + 0x16c), ld<uint32_t>(ms + 0x1c8)};
        sig = (sig ^ (ms + 31 * rows[nrow - 1].flags + 3 * rows[nrow - 1].ctl + rows[nrow - 1].disp)) * 0x100000001b3ull;
        node = ld<uint64_t>(node + 8);
    }
    // The bit-16 (0x10000) setter main_00CDA380 finds the movie system by id in a second array: count at [list+0x38],
    // entries at [list+0x30] with {?, id at +4, movie system pointer at +8}. Dumped here because "bit 16 never set"
    // can mean "never registered in that array".
    const uint32_t count = ld<uint32_t>(list + 0x38);
    const uint64_t array = ld<uint64_t>(list + 0x30);
    if (count && array && count < 64 && vm_query(array).state != 0) {
        for (uint32_t i = 0; i < count && i < 8; i++) {
            const uint64_t e = array + uint64_t(i) * 16;
            sig = (sig ^ (e + ld<uint32_t>(e + 4) + 7 * ld<uint64_t>(e + 8))) * 0x100000001b3ull;
        }
    }
    static uint64_t last_sig = ~0ull;
    if (sig == last_sig) return;
    last_sig = sig;
    if (count && array && count < 64 && vm_query(array).state != 0)
        for (uint32_t i = 0; i < count && i < 8; i++) {
            const uint64_t e = array + uint64_t(i) * 16;
            log("movie-vt:   registry[%u] id %u -> system %llx", i, ld<uint32_t>(e + 4),
                (unsigned long long)ld<uint64_t>(e + 8));
        }
    for (int i = 0; i < nrow; i++)
        log("movie-vt:   movie system %llx: flags16c %x controller330 %llx display338 %llx f148 %llx f1c8 %u",
            (unsigned long long)rows[i].ms, rows[i].flags, (unsigned long long)rows[i].ctl,
            (unsigned long long)rows[i].disp, (unsigned long long)rows[i].f148, rows[i].f1c8);
}

// The pull loop's own state, sampled at the poll: the tick at main+cd6814 only reaches
// main+cd6844 (decoder->AcquireInputBufferIndex) when the +0x168 data event is signalled, so this line has to say
// whether the poll that returned 1 is the tick's, and in which controller state it happened.
void media_try_wait_event(CPU& c) {
    const uint64_t event = c.x[0];
    // The caller's return address, taken before the real call (the guest call clobbers x30): a PLT stub tail-branches,
    // so this is the instruction after the poll. The tick's data event is main+cd6814, its output event main+cd6b88.
    const uint64_t lr = c.x[30];
    if (g_try_wait_real) call(c, g_try_wait_real);
    const uint64_t ctl = g_controller.load(std::memory_order_relaxed);
    // The repair is ON by default (it is the fix, not an experiment; NX_MOVIE_ATTACH=0 disables it for bisecting):
    // one gated delivery attempt per controller, at a frame where playback is prepared.
    static const bool attach_on = [] {
        const char* e = std::getenv("NX_MOVIE_ATTACH");
        return !(e && e[0] == '0');
    }();
    static std::atomic<uint64_t> attached_ctl{0};
    if (attach_on && ctl && attached_ctl.load(std::memory_order_relaxed) != ctl &&
        vm_query(ctl).state != 0 && ld<uint32_t>(ctl + 0x38c) != 0)
        if (attach_controller(ctl))  // 1 = settled (bit set now, or the game had set it); 0 = not bound yet, retry
            attached_ctl.store(ctl, std::memory_order_relaxed);
    // Both movies' ticks poll two events of their controller: +0x168 (input/data available) and +0x190 (output).
    const bool is_movie_event = ctl != 0 && (event == ctl + 0x168 || event == ctl + 0x190);
    if (g_media_probes && ctl != 0) {
        const long long off = (long long)event - (long long)ctl;
        const uint32_t sig = uint32_t(c.x[0] & 1) | (ld<uint32_t>(ctl + 0x320) << 1) | (ld<uint32_t>(ctl + 0x3b0) << 4) |
                             (ld<uint32_t>(ctl + 0x3c4) << 7) | (ld<uint32_t>(ctl + 0x3bc) << 8) |
                             (ld<uint32_t>(ctl + 0x3c0) << 9) | (ld<uint32_t>(ctl + 0x390) << 10) |
                             (ld<uint32_t>(ctl + 0x3a8) << 11);
        if (off >= 0 && off < 0x400 && g_try_budget.take_on_change(ctl, sig, 90)) {
            log("movie-vt: TryWaitEvent(%llx, controller+0x%llx) -> %llu lr %s | 3a4 %u 3b0 %u 3c4 %u 240 %llx "
                "320 %u 374 %u 2b0 %llx 3bc %u 3c0 %u 390 %u 3a8 %u 3b8 %g 360 %lld 38c %u 368 %llx",
                (unsigned long long)event, (unsigned long long)off, (unsigned long long)c.x[0],
                describe(lr).c_str(), ld<uint32_t>(ctl + 0x3a4), ld<uint32_t>(ctl + 0x3b0),
                ld<uint32_t>(ctl + 0x3c4), (unsigned long long)(ctl ? ld<uint64_t>(ctl + 0x240) : 0),
                ld<uint32_t>(ctl + 0x320), ld<uint32_t>(ctl + 0x374),
                (unsigned long long)ld<uint64_t>(ctl + 0x2b0), ld<uint32_t>(ctl + 0x3bc),
                ld<uint32_t>(ctl + 0x3c0), ld<uint32_t>(ctl + 0x390), ld<uint32_t>(ctl + 0x3a8),
                (double)ld<float>(ctl + 0x3b8), (long long)ld<int64_t>(ctl + 0x360), ld<uint32_t>(ctl + 0x38c),
                (unsigned long long)ld<uint64_t>(ctl + 0x368));
            if (off == 0x168) log_movie_systems();
        }
    }
    if (c.x[0] == 0 && g_shim_event && is_movie_event) {
        if (g_shim_budget.take(event, 12))
            log("movie-vt: shim: reporting data available on controller+0x%llx (lr %s)",
                (unsigned long long)(event - ctl), describe(lr).c_str());
        c.x[0] = 1;
    }
}

// nn::os::ClearEvent: the instruction after the tick's successful poll (main+cd6820 is the data event, main+cd6b94 the
// output event). Logged for the same reason as the poll: it pins the call site of everything that follows.
void media_clear_event(CPU& c) {
    const uint64_t event = c.x[0];
    const uint64_t lr = c.x[30];
    if (g_clear_event_real) call(c, g_clear_event_real);
    const uint64_t ctl = g_controller.load(std::memory_order_relaxed);
    if (!g_media_probes || !ctl) return;
    const long long off = (long long)event - (long long)ctl;
    if (off >= 0 && off < 0x400 && g_clear_budget.take(ctl, 40))
        log("movie-vt: ClearEvent(%llx, controller+0x%llx) lr %s", (unsigned long long)event,
            (unsigned long long)off, describe(lr).c_str());
}

uint64_t g_signal_event_real = 0;

// The decoder -> player handshake, mapped: RegisterClientEvents(DecoderEvents*) stores FOUR guest nn::os::EventType
// pointers (0 input available, 1 output available, 2 output format changed, 3 error) and the game may fill them in
// later, so they are read after the real call and remembered here. Every SignalEvent is then labelled with the
// decoder object and which of its four events it is — the B2recomp study (docs/EXTERNAL-REFERENCES.md) shows the
// player advances only once the *format changed* event of every decoder it created has arrived, and that a missing
// one costs exactly a 5 s wait followed by teardown, which is ev0005's shape.
const char* const kClientEventNames[4] = {"input-available", "output-available", "output-format-changed", "error"};
struct ClientEvent {
    std::atomic<uint64_t> addr{0};
    std::atomic<uint64_t> owner{0};  // the movie::Decoder object it belongs to
    std::atomic<int> index{0};
    std::atomic<uint32_t> signals{0};
};
ClientEvent g_client_events[16];

void register_client_events(uint64_t decoder, uint64_t events_struct) {
    if (!events_struct || vm_query(events_struct).state == 0) {
        log("movie-vt: decoder %llx RegisterClientEvents(%llx): unreadable struct", (unsigned long long)decoder,
            (unsigned long long)events_struct);
        return;
    }
    char all[128];
    int k = 0;
    all[0] = 0;
    for (int i = 0; i < 4; i++) {
        const uint64_t addr = ld<uint64_t>(events_struct + uint64_t(i) * 8);
        k += std::snprintf(all + k, unsigned(sizeof all - k), "%s%llx ", i ? " " : "", (unsigned long long)addr);
        if (!addr) continue;
        for (int e = 0; e < 16; e++) {
            if (g_client_events[e].addr.load(std::memory_order_relaxed) == addr) {
                g_client_events[e].owner.store(decoder, std::memory_order_relaxed);
                g_client_events[e].index.store(i, std::memory_order_relaxed);
                break;
            }
            if (g_client_events[e].addr.load(std::memory_order_relaxed) == 0) {
                g_client_events[e].addr.store(addr, std::memory_order_relaxed);
                g_client_events[e].owner.store(decoder, std::memory_order_relaxed);
                g_client_events[e].index.store(i, std::memory_order_relaxed);
                break;
            }
        }
    }
    log("movie-vt:   decoder %llx client events = %s(input, output, format-changed, error)", (unsigned long long)decoder,
        all);
}

const ClientEvent* find_client_event(uint64_t addr) {
    for (int e = 0; e < 16; e++)
        if (g_client_events[e].addr.load(std::memory_order_relaxed) == addr) return &g_client_events[e];
    return nullptr;
}

// NX_MOVIE_SIGNAL_FORMAT=1: deliver a decoder's output-format-changed event from the runtime when the game's own
// decode pipeline does not. Measured (run 8, logs/mac/movie-ev8-game.log): ev0000's decoder signals it once
// (subsdk0+11dd7c, DecoderMessageReceiver::onMessageReceived+490) and plays; ev0005's decoder never does — and the B2
// study of this same library says the player advances only once the format event of every decoder it created has
// arrived, waiting 5 s and tearing the movie down otherwise, which is ev0005's shape. Signalled ON THE GAME THREAD
// from Start (the reference implementation signals it from Configure/Start too), with the CPU context saved and
// restored so the caller's return value is untouched.
void signal_format_event(CPU& c, uint64_t decoder) {
    // The repair is ON by default (it is the fix, not an experiment; NX_MOVIE_SIGNAL_FORMAT=0 disables it for
    // bisecting).
    static const bool signal_on = [] {
        const char* e = std::getenv("NX_MOVIE_SIGNAL_FORMAT");
        return !(e && e[0] == '0');
    }();
    if (!signal_on) return;
    static const uint64_t signal_event = find_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (!signal_event) return;
    for (int e = 0; e < 16; e++) {
        if (g_client_events[e].owner.load(std::memory_order_relaxed) != decoder) continue;
        if (g_client_events[e].index.load(std::memory_order_relaxed) != 2) continue;  // output format changed
        const uint64_t addr = g_client_events[e].addr.load(std::memory_order_relaxed);
        if (!addr || g_client_events[e].signals.load(std::memory_order_relaxed)) return;  // already delivered
        const CPU saved = c;
        c.x[0] = addr;
        call(c, signal_event);
        c = saved;
        g_client_events[e].signals.fetch_add(1, std::memory_order_relaxed);
        log("movie-vt: delivered output-format-changed for decoder %llx from the host (game never sent it)", 
            (unsigned long long)decoder);
        return;
    }
}

void media_signal_event(CPU& c) {
    const uint64_t event = c.x[0];
    const uint64_t lr = c.x[30];
    const uint64_t ctl = g_controller.load(std::memory_order_relaxed);
    ClientEvent* ce = const_cast<ClientEvent*>(find_client_event(event));
    if (ce) {
        const uint32_t n = ce->signals.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || n == 9)
            log("movie-vt: SignalEvent(%llx = decoder %llx event %d %s) #%u from lr %s t%lld", (unsigned long long)event,
                (unsigned long long)ce->owner.load(std::memory_order_relaxed), ce->index.load(),
                kClientEventNames[ce->index.load()], n, describe(lr).c_str(), (long long)plat::current_thread_id());
    } else if (ctl && (event == ctl + 0x168 || event == ctl + 0x190) && g_signal_budget.take(event, 24)) {
        log("movie-vt: SignalEvent(controller+0x%llx) from lr %s (not a registered client event)",
            (unsigned long long)(event - ctl), describe(lr).c_str());
    }
    if (g_signal_event_real) call(c, g_signal_event_real);
}

void media_find_int32(CPU& c) {
    const std::string key = guest_string(c.x[1]);
    const uint64_t out = c.x[2];  // saved before the call clobbers x2, as in media_find_string
    if (g_media_probes && std::strcmp(key.c_str(), "mime") == 0)
        log("movie-vt: MediaData::FindInt32(key %s) lr %s", key.c_str(), describe(c.x[30]).c_str());
    if (g_find_int32_real) call(c, g_find_int32_real);
    if (g_media_probes && std::strcmp(key.c_str(), "mime") == 0) {
        const bool found = c.x[0] != 0;
        log("movie-vt:   FindInt32(mime) -> %s out %llx value %d", found ? "found" : "NOT FOUND",
            (unsigned long long)out, found && out ? ld<int32_t>(out) : 0);
    }
}

// Redirect the media GOT slots (saving the real pointers first). A GOT slot is only patched after its current value
// resolves to the symbol it is supposed to hold. Patching a slot that holds something else turns the runtime into a
// stranger calling convention: a guessed offset for subsdk0's SignalEvent held std::mutex's destructor and hung the
// boot.
//
// NX_MOVIE_MEDIADATA=1 installs all five (they also log). The TryWaitEvent slot is installed UNCONDITIONALLY since
// the gated attach repair lives in its thunk: "the attach is on by default" was only true when the diagnostic
// variable was set, which the first Deck re-verification caught (ev0005's decoder started and the format event was
// delivered, but no attach line and the first plane fill came 75 s late, 1348 of the file's 1457 — the renderer's
// bit-16 gate had not opened). The thunk tails the real function and only adds the attach, so with the probe switch
// off it is behaviourally transparent apart from the repair.
bool got_holds(uint64_t got, const char* expected) {
    const uint64_t real = ld<uint64_t>(got);
    const std::string d = describe(real);
    const bool ok = d.find(expected) != std::string::npos;
    if (!ok)
        log("movie-vt: GOT %llx holds %s (%llx), not %s: leaving it alone", (unsigned long long)got, d.c_str(),
            (unsigned long long)real, expected);
    return ok;
}

// The decoder's own nn::os::SignalEvent calls go through subsdk0's GOT. The slot is resolved from subsdk0's own
// relocation table (reloc -> symbols[sym].name), the same structures the loader links with — not from a parsed index.
// The slot is still only patched after got_holds() confirms what it contains.
uint64_t find_import_slot(const char* mod_name, const char* symbol) {
    const LoadedModule* mod = module_at(kSubsdk0Base);
    if (!mod || !std::strstr(mod->info->name, mod_name)) return 0;
    const ModuleInfo& in = *mod->info;
    int hits = 0;
    uint64_t at = 0;
    for (size_t i = 0; i < in.reloc_count; i++) {
        const Reloc& r = in.relocs[i];
        if (r.type != R_AARCH64_JUMP_SLOT && r.type != R_AARCH64_GLOB_DAT) continue;
        if (r.sym >= in.symbol_count) continue;
        if (std::strcmp(in.symbols[r.sym].name, symbol) != 0) continue;
        ++hits;
        at = mod->base + r.offset;
    }
    if (hits == 1) return at;
    log("movie-vt: %s: %d relocation slots for %s; not patching", mod_name, hits, symbol);
    return 0;
}

void install_media_probes() {
    static bool done = false;
    if (done) return;
    done = true;
    g_media_probes = std::getenv("NX_MOVIE_MEDIADATA") != nullptr;
    const bool shim = std::getenv("NX_MOVIE_SHIM_EVENT") != nullptr;
    g_shim_event = shim;
    struct Got {
        const char* name;
        uint64_t got;
        uint64_t* saved;
        void (*thunk)(CPU&);
        const char* expect;
    } slots[] = {
        {"<movie FindString>", kModuleBase + kMainFindStringGot, &g_find_string_real, media_find_string, "FindString"},
        {"<movie FindInt32>", kModuleBase + kMainFindInt32Got, &g_find_int32_real, media_find_int32, "FindInt32"},
        {"<movie TryWaitEvent>", kModuleBase + kMainTryWaitEventGot, &g_try_wait_real, media_try_wait_event,
         "TryWaitEvent"},
        {"<movie ClearEvent>", kModuleBase + kMainClearEventGot, &g_clear_event_real, media_clear_event, "ClearEvent"},
        {"<movie SignalEvent>", kModuleBase + kMainSignalEventGot, &g_signal_event_real, media_signal_event,
         "SignalEvent"},
    };
    for (Got& g : slots) {
        // media_try_wait_event() is where the gated bit-16 attach runs, so that slot is a repair, not a probe:
        // install it even with NX_MOVIE_MEDIADATA unset (the rest stay behind the switch / the shim).
        const bool attach_hook = std::strcmp(g.expect, "TryWaitEvent") == 0;
        if (!g_media_probes && !shim && !attach_hook) continue;
        if (!module_at(g.got)) {
            log("movie-vt: MediaData probe %s: GOT %llx is not in a module", g.name, (unsigned long long)g.got);
            continue;
        }
        const VmInfo v = vm_query(g.got);
        if (!(v.perm & 2)) {
            log("movie-vt: MediaData probe %s: GOT %llx is not writable", g.name, (unsigned long long)g.got);
            continue;
        }
        if (!got_holds(g.got, g.expect)) continue;
        *g.saved = ld<uint64_t>(g.got);
        st<uint64_t>(g.got, hle_address_or(g.name, g.thunk));
        log("movie-vt: MediaData probe %s: GOT %llx -> %llx (real %llx = %s kept)", g.name, (unsigned long long)g.got,
            (unsigned long long)ld<uint64_t>(g.got), (unsigned long long)*g.saved, describe(*g.saved).c_str());
    }
    // NX_MOVIE_SIGSUBSDK=1: also redirect subsdk0's own nn::os::SignalEvent import, which is where the game's
    // decoder signals the four client events (main's copy is not called). Diagnostic only.
    if (std::getenv("NX_MOVIE_SIGSUBSDK") && g_signal_event_real) {
        const uint64_t got = find_import_slot("subsdk0", "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
        if (got && module_at(got) && (vm_query(got).perm & 2) && got_holds(got, "SignalEvent")) {
            st<uint64_t>(got, hle_address_or("<movie subsdk0 SignalEvent>", media_signal_event));
            log("movie-vt: subsdk0 SignalEvent GOT %llx -> %llx", (unsigned long long)got,
                (unsigned long long)ld<uint64_t>(got));
        }
    }
}

// A thunk for one (class, slot). Logs the caller, the call and its Result, then calls the game's original so
// behaviour is unchanged.
void dispatch(CPU& c, int which, int slot) {
    static const int cap = g_frame_only ? 0 : env_int("NX_MOVIE_VTABLE_CAP", kLogPerSlot);
    static const int bt_cap = g_frame_only ? 0 : env_int("NX_MOVIE_VTABLE_BT", 1);
    SlotState& s = g_slots[which][slot];
    const int n = s.calls.fetch_add(1, std::memory_order_relaxed);
    const char* name = slot_name(which, slot);
    const char* cls = class_name(which);
    {   // attribution first: which object was this call made on (a class-wide count mixes the two movies)
        const uint64_t obj = c.x[0];
        if (obj) {
            const int oi = object_index(which, obj);
            if (oi >= 0 && slot < kTrackedSlots)
                g_objects[which][oi].calls[slot].fetch_add(1, std::memory_order_relaxed);
        }
    }
    // The player's state machine keeps its state at this+0x180 (read/written by SetPlayerState/Prepare/Start in
    // the recompiled image). Logging it around each call turns "the player does not start" into "the state did not
    // advance", without reading guest memory offline.
    const bool player = which == kWhichPlayer;
    // A runtime-discovered table (the controller) has no slot names: the function it holds is the name.
    std::string ctl_name;
    if (which == kWhichController && n < cap) {  // only when the line will be logged: describe() allocates
        ctl_name = s.real ? describe(s.real) : std::string("?");
        name = ctl_name.c_str();
    }
    const uint64_t self = c.x[0];
    // Capture every argument BEFORE the call: the guest call clobbers x0..x18, so reading x3/x4 afterwards
    // dereferences whatever the callee left there (that crashed the first attribution build at address 0x7).
    const uint64_t a1 = c.x[1], a2 = c.x[2], a3 = c.x[3], a4 = c.x[4];
    const uint64_t caller_fp = c.x[29];
    const uint32_t state_before = player && self ? ld<uint32_t>(self + 0x180) : 0;
    const bool logging = n < cap;
    if (logging) {
        log("movie-vt: t%lld %s slot %2d %-38s this %llx a1 %llx a2 %llx a3 %llx a4 %llx lr %s%s", 
            (long long)plat::current_thread_id(), cls, slot, name, (unsigned long long)c.x[0],
            (unsigned long long)c.x[1], (unsigned long long)c.x[2], (unsigned long long)c.x[3],
            (unsigned long long)c.x[4], describe(c.x[30]).c_str(), player ? " (player)" : "");
    }
    if (n < bt_cap) {
        log("movie-vt:   caller chain for %s slot %d (%s):", cls, slot, name);
        backtrace(c);
    }
    // The ClientStreamReader* arrives as the first argument of the two SetDataSource overloads. Its (main-local)
    // vtable is patched BEFORE the call rather than after it, so the FileSource constructor that runs *inside*
    // SetDataSource already dispatches through it: that is what puts stream slot 4 (the preload) behind
    // stream_preload_guard() for the FIRST movie of a process as well as the later ones. Nothing else changes.
    if ((which == kWhichExtractor && slot == 17) || (which == kWhichNnExtractor && slot == 4))
        patch_stream_reader(a1);
    uint64_t ret = 0;
    // Stream slot 4 is the FileSource ctor's "open + preload" call. Its preload window is set to NX_MOVIE_PRELOAD
    // bytes (default 0) BEFORE the guest code runs: that window's allocation is what fails inside the call and the
    // guest's own cleanup of that failure is the SIGSEGV (see the block above preload_cap()).
    uint64_t stream_asked = 0;
    if (which == kWhichStream && slot == 4 && self) stream_asked = stream_preload_guard(self);
    // This verified caller has already selected a frame and prefilled x21's
    // plane addresses. Clear its thread-local pending frame on every attempt,
    // even when the code layout or guest fields fail validation.
    const bool frame_attempt = which == kWhichDecoder && slot == 15 &&
        movie_frame_source::matching_caller(kModuleBase, c.x[30]);
    auto frame_read = [](uint64_t addr, void* out, size_t size) {
        const auto region = vm_query(addr);
        if (!(region.perm & 1) || addr < region.addr || size > region.size ||
            addr - region.addr > region.size - size) return false;
        std::memcpy(out, reinterpret_cast<const void*>(addr), size);
        return true;
    };
    auto frame_publish = [](uint64_t handle, int64_t pts, uint64_t y, uint64_t uv) {
        movie_on_frame(handle, pts, y, uv);
    };
    const auto frame = movie_frame_source::begin(frame_attempt, kModuleBase, c.x[19], self,
                                                  a1, c.x[21], frame_read, frame_publish);
    if (s.real) {
        call(c, s.real);
        ret = c.x[0];
    } else {
        c.x[0] = 0;  // a null slot in the game's table: behave like a no-op returning success
    }
    movie_frame_source::finish(frame, s.real != 0, ret, frame_publish);
    if (which == kWhichStream && slot == 4 && self) stream_preload_report(self, stream_asked, ret);
    // Stream slot 6 is the FileSource's close (main_00CD7D7C): the second half of the per-movie measurement, so the
    // gap between two movies and the retention inside one movie become separate numbers (docs/archive/MOVIE-EV0005.md §21.3).
    // The reader is still alive here — the dtor (slot 3) is where it stops being readable, so that one is left alone.
    if (which == kWhichStream && slot == 6 && self) log_resource_ledger(self, " at close", false);
    if (logging) {
        if (player) {
            const uint32_t state_after = self ? ld<uint32_t>(self + 0x180) : 0;
            log("movie-vt:   %s slot %2d returned %llx (player state %u -> %u)", cls, slot,
                (unsigned long long)ret, state_before, state_after);
        } else {
            log("movie-vt:   %s slot %2d returned %llx", cls, slot, (unsigned long long)ret);
        }
        log_out_params(which, slot, self, a1, a2, a3, a4, caller_fp);
    }
    if (which == kWhichDecoder && slot == 5) register_client_events(self, a1);
    if (which == kWhichDecoder && slot == 7 && !g_frame_only) signal_format_event(c, self);
    if (which == kWhichExtractor && slot == 17) plat::install_decoder_probe();
}

// The thunks. A guest thunk is called with the CPU context only, so the (class, slot) pair has to be baked into a
// distinct function: a 2-D array of function pointers, filled from the NX_VT_THUNK ladder below. hle_address_or()
// turns each one into a guest-callable stub; the name it is registered under is what the log reports.
#define NX_VT_THUNK(which, slot) void vt_##which##_##slot(CPU& c) { dispatch(c, which, slot); }

NX_VT_THUNK(0, 0) NX_VT_THUNK(0, 1) NX_VT_THUNK(0, 2) NX_VT_THUNK(0, 3) NX_VT_THUNK(0, 4) NX_VT_THUNK(0, 5)
NX_VT_THUNK(0, 6) NX_VT_THUNK(0, 7) NX_VT_THUNK(0, 8) NX_VT_THUNK(0, 9) NX_VT_THUNK(0, 10) NX_VT_THUNK(0, 11)
NX_VT_THUNK(0, 12) NX_VT_THUNK(0, 13) NX_VT_THUNK(0, 14) NX_VT_THUNK(0, 15) NX_VT_THUNK(0, 16) NX_VT_THUNK(0, 17)
NX_VT_THUNK(0, 18) NX_VT_THUNK(0, 19) NX_VT_THUNK(0, 20) NX_VT_THUNK(0, 21) NX_VT_THUNK(0, 22) NX_VT_THUNK(0, 23)
NX_VT_THUNK(0, 24) NX_VT_THUNK(0, 25) NX_VT_THUNK(0, 26) NX_VT_THUNK(0, 27)

NX_VT_THUNK(1, 0) NX_VT_THUNK(1, 1) NX_VT_THUNK(1, 2) NX_VT_THUNK(1, 3) NX_VT_THUNK(1, 4) NX_VT_THUNK(1, 5)
NX_VT_THUNK(1, 6) NX_VT_THUNK(1, 7) NX_VT_THUNK(1, 8) NX_VT_THUNK(1, 9) NX_VT_THUNK(1, 10) NX_VT_THUNK(1, 11)
NX_VT_THUNK(1, 12) NX_VT_THUNK(1, 13) NX_VT_THUNK(1, 14) NX_VT_THUNK(1, 15) NX_VT_THUNK(1, 16) NX_VT_THUNK(1, 17)

NX_VT_THUNK(2, 0) NX_VT_THUNK(2, 1) NX_VT_THUNK(2, 2) NX_VT_THUNK(2, 3) NX_VT_THUNK(2, 4) NX_VT_THUNK(2, 5)
NX_VT_THUNK(2, 6) NX_VT_THUNK(2, 7) NX_VT_THUNK(2, 8) NX_VT_THUNK(2, 9) NX_VT_THUNK(2, 10) NX_VT_THUNK(2, 11)
NX_VT_THUNK(2, 12) NX_VT_THUNK(2, 13) NX_VT_THUNK(2, 14) NX_VT_THUNK(2, 15) NX_VT_THUNK(2, 16) NX_VT_THUNK(2, 17)
NX_VT_THUNK(2, 18) NX_VT_THUNK(2, 19) NX_VT_THUNK(2, 20) NX_VT_THUNK(2, 21) NX_VT_THUNK(2, 22) NX_VT_THUNK(2, 23)
NX_VT_THUNK(2, 24) NX_VT_THUNK(2, 25) NX_VT_THUNK(2, 26) NX_VT_THUNK(2, 27) NX_VT_THUNK(2, 28) NX_VT_THUNK(2, 29)
NX_VT_THUNK(2, 30)

NX_VT_THUNK(3, 0) NX_VT_THUNK(3, 1) NX_VT_THUNK(3, 2) NX_VT_THUNK(3, 3) NX_VT_THUNK(3, 4) NX_VT_THUNK(3, 5)
NX_VT_THUNK(3, 6) NX_VT_THUNK(3, 7) NX_VT_THUNK(3, 8) NX_VT_THUNK(3, 9) NX_VT_THUNK(3, 10) NX_VT_THUNK(3, 11)
NX_VT_THUNK(3, 12) NX_VT_THUNK(3, 13) NX_VT_THUNK(3, 14) NX_VT_THUNK(3, 15) NX_VT_THUNK(3, 16) NX_VT_THUNK(3, 17)
NX_VT_THUNK(3, 18) NX_VT_THUNK(3, 19) NX_VT_THUNK(3, 20) NX_VT_THUNK(3, 21) NX_VT_THUNK(3, 22) NX_VT_THUNK(3, 23)
NX_VT_THUNK(3, 24) NX_VT_THUNK(3, 25) NX_VT_THUNK(3, 26) NX_VT_THUNK(3, 27) NX_VT_THUNK(3, 28) NX_VT_THUNK(3, 29)
NX_VT_THUNK(3, 30) NX_VT_THUNK(3, 31) NX_VT_THUNK(3, 32) NX_VT_THUNK(3, 33) NX_VT_THUNK(3, 34) NX_VT_THUNK(3, 35)
NX_VT_THUNK(3, 36) NX_VT_THUNK(3, 37) NX_VT_THUNK(3, 38) NX_VT_THUNK(3, 39) NX_VT_THUNK(3, 40) NX_VT_THUNK(3, 41)
NX_VT_THUNK(3, 42) NX_VT_THUNK(3, 43) NX_VT_THUNK(3, 44) NX_VT_THUNK(3, 45)

NX_VT_THUNK(4, 0) NX_VT_THUNK(4, 1) NX_VT_THUNK(4, 2) NX_VT_THUNK(4, 3) NX_VT_THUNK(4, 4) NX_VT_THUNK(4, 5)
NX_VT_THUNK(4, 6) NX_VT_THUNK(4, 7)

NX_VT_THUNK(5, 0) NX_VT_THUNK(5, 1) NX_VT_THUNK(5, 2) NX_VT_THUNK(5, 3) NX_VT_THUNK(5, 4) NX_VT_THUNK(5, 5)
NX_VT_THUNK(5, 6) NX_VT_THUNK(5, 7) NX_VT_THUNK(5, 8)

NX_VT_THUNK(6, 0) NX_VT_THUNK(6, 1) NX_VT_THUNK(6, 2) NX_VT_THUNK(6, 3) NX_VT_THUNK(6, 4) NX_VT_THUNK(6, 5)
NX_VT_THUNK(6, 6) NX_VT_THUNK(6, 7) NX_VT_THUNK(6, 8)

NX_VT_THUNK(7, 0) NX_VT_THUNK(7, 1) NX_VT_THUNK(7, 2) NX_VT_THUNK(7, 3) NX_VT_THUNK(7, 4) NX_VT_THUNK(7, 5)
NX_VT_THUNK(7, 6) NX_VT_THUNK(7, 7) NX_VT_THUNK(7, 8)

NX_VT_THUNK(8, 0) NX_VT_THUNK(8, 1) NX_VT_THUNK(8, 2) NX_VT_THUNK(8, 3) NX_VT_THUNK(8, 4) NX_VT_THUNK(8, 5)
NX_VT_THUNK(8, 6) NX_VT_THUNK(8, 7) NX_VT_THUNK(8, 8) NX_VT_THUNK(8, 9) NX_VT_THUNK(8, 10) NX_VT_THUNK(8, 11)
NX_VT_THUNK(8, 12) NX_VT_THUNK(8, 13) NX_VT_THUNK(8, 14) NX_VT_THUNK(8, 15) NX_VT_THUNK(8, 16) NX_VT_THUNK(8, 17)
NX_VT_THUNK(8, 18) NX_VT_THUNK(8, 19) NX_VT_THUNK(8, 20) NX_VT_THUNK(8, 21) NX_VT_THUNK(8, 22) NX_VT_THUNK(8, 23)
NX_VT_THUNK(8, 24) NX_VT_THUNK(8, 25) NX_VT_THUNK(8, 26) NX_VT_THUNK(8, 27) NX_VT_THUNK(8, 28) NX_VT_THUNK(8, 29)
NX_VT_THUNK(8, 30) NX_VT_THUNK(8, 31)

NX_VT_THUNK(9, 0) NX_VT_THUNK(9, 1) NX_VT_THUNK(9, 2) NX_VT_THUNK(9, 3) NX_VT_THUNK(9, 4) NX_VT_THUNK(9, 5)
NX_VT_THUNK(9, 6) NX_VT_THUNK(9, 7) NX_VT_THUNK(9, 8) NX_VT_THUNK(9, 9) NX_VT_THUNK(9, 10) NX_VT_THUNK(9, 11)
NX_VT_THUNK(9, 12) NX_VT_THUNK(9, 13) NX_VT_THUNK(9, 14) NX_VT_THUNK(9, 15) NX_VT_THUNK(9, 16) NX_VT_THUNK(9, 17)
NX_VT_THUNK(9, 18) NX_VT_THUNK(9, 19) NX_VT_THUNK(9, 20)

NX_VT_THUNK(10, 0) NX_VT_THUNK(10, 1) NX_VT_THUNK(10, 2) NX_VT_THUNK(10, 3) NX_VT_THUNK(10, 4) NX_VT_THUNK(10, 5)
NX_VT_THUNK(10, 6) NX_VT_THUNK(10, 7) NX_VT_THUNK(10, 8) NX_VT_THUNK(10, 9) NX_VT_THUNK(10, 10) NX_VT_THUNK(10, 11)
NX_VT_THUNK(10, 12) NX_VT_THUNK(10, 13) NX_VT_THUNK(10, 14) NX_VT_THUNK(10, 15) NX_VT_THUNK(10, 16) NX_VT_THUNK(10, 17)
NX_VT_THUNK(10, 18) NX_VT_THUNK(10, 19) NX_VT_THUNK(10, 20) NX_VT_THUNK(10, 21) NX_VT_THUNK(10, 22) NX_VT_THUNK(10, 23)
NX_VT_THUNK(10, 24) NX_VT_THUNK(10, 25) NX_VT_THUNK(10, 26) NX_VT_THUNK(10, 27) NX_VT_THUNK(10, 28) NX_VT_THUNK(10, 29)
NX_VT_THUNK(10, 30) NX_VT_THUNK(10, 31) NX_VT_THUNK(10, 32) NX_VT_THUNK(10, 33) NX_VT_THUNK(10, 34) NX_VT_THUNK(10, 35)
NX_VT_THUNK(10, 36) NX_VT_THUNK(10, 37) NX_VT_THUNK(10, 38) NX_VT_THUNK(10, 39) NX_VT_THUNK(10, 40) NX_VT_THUNK(10, 41)
NX_VT_THUNK(10, 42) NX_VT_THUNK(10, 43) NX_VT_THUNK(10, 44) NX_VT_THUNK(10, 45) NX_VT_THUNK(10, 46) NX_VT_THUNK(10, 47)
NX_VT_THUNK(10, 48) NX_VT_THUNK(10, 49) NX_VT_THUNK(10, 50) NX_VT_THUNK(10, 51) NX_VT_THUNK(10, 52) NX_VT_THUNK(10, 53)
NX_VT_THUNK(10, 54) NX_VT_THUNK(10, 55) NX_VT_THUNK(10, 56) NX_VT_THUNK(10, 57) NX_VT_THUNK(10, 58) NX_VT_THUNK(10, 59)
NX_VT_THUNK(10, 60) NX_VT_THUNK(10, 61) NX_VT_THUNK(10, 62) NX_VT_THUNK(10, 63)

#undef NX_VT_THUNK

using ThunkFn = void (*)(CPU&);
ThunkFn const kNnCodecThunks[kNnCodecSlots] = {
    vt_0_0,  vt_0_1,  vt_0_2,  vt_0_3,  vt_0_4,  vt_0_5,  vt_0_6,  vt_0_7,  vt_0_8,  vt_0_9,
    vt_0_10, vt_0_11, vt_0_12, vt_0_13, vt_0_14, vt_0_15, vt_0_16, vt_0_17, vt_0_18, vt_0_19,
    vt_0_20, vt_0_21, vt_0_22, vt_0_23, vt_0_24, vt_0_25, vt_0_26, vt_0_27,
};
ThunkFn const kExtractorThunks[kExtractorSlots] = {
    vt_1_0, vt_1_1, vt_1_2, vt_1_3, vt_1_4, vt_1_5, vt_1_6, vt_1_7, vt_1_8,
    vt_1_9, vt_1_10, vt_1_11, vt_1_12, vt_1_13, vt_1_14, vt_1_15, vt_1_16, vt_1_17,
};
ThunkFn const kNnCodecPlayerThunks[kNnCodecPlayerSlots] = {
    vt_3_0, vt_3_1, vt_3_2, vt_3_3, vt_3_4, vt_3_5, vt_3_6, vt_3_7, vt_3_8, vt_3_9,
    vt_3_10, vt_3_11, vt_3_12, vt_3_13, vt_3_14, vt_3_15, vt_3_16, vt_3_17, vt_3_18, vt_3_19,
    vt_3_20, vt_3_21, vt_3_22, vt_3_23, vt_3_24, vt_3_25, vt_3_26, vt_3_27, vt_3_28, vt_3_29,
    vt_3_30, vt_3_31, vt_3_32, vt_3_33, vt_3_34, vt_3_35, vt_3_36, vt_3_37, vt_3_38, vt_3_39,
    vt_3_40, vt_3_41, vt_3_42, vt_3_43, vt_3_44, vt_3_45,
};
ThunkFn const kNnExtractorThunks[kNnExtractorSlots] = {
    vt_2_0,  vt_2_1,  vt_2_2,  vt_2_3,  vt_2_4,  vt_2_5,  vt_2_6,  vt_2_7,  vt_2_8,  vt_2_9,
    vt_2_10, vt_2_11, vt_2_12, vt_2_13, vt_2_14, vt_2_15, vt_2_16, vt_2_17, vt_2_18, vt_2_19,
    vt_2_20, vt_2_21, vt_2_22, vt_2_23, vt_2_24, vt_2_25, vt_2_26, vt_2_27, vt_2_28, vt_2_29,
    vt_2_30,
};
ThunkFn const kVideoCallbackThunks[kVideoCodecCallbackSlots] = {
    vt_4_0, vt_4_1, vt_4_2, vt_4_3, vt_4_4, vt_4_5, vt_4_6, vt_4_7,
};
ThunkFn const kNnCodecMsgThunks[kNnCodecMessageReceiverSlots] = {
    vt_5_0, vt_5_1, vt_5_2, vt_5_3, vt_5_4, vt_5_5, vt_5_6, vt_5_7, vt_5_8,
};
ThunkFn const kVideoOutThunks[kNnCodecVideoOutReceiverSlots] = {
    vt_6_0, vt_6_1, vt_6_2, vt_6_3, vt_6_4, vt_6_5, vt_6_6, vt_6_7, vt_6_8,
};
ThunkFn const kAudioOutThunks[kNnCodecAudioOutReceiverSlots] = {
    vt_7_0, vt_7_1, vt_7_2, vt_7_3, vt_7_4, vt_7_5, vt_7_6, vt_7_7, vt_7_8,
};
ThunkFn const kDecoderThunks[kDecoderSlots] = {
    vt_9_0,  vt_9_1,  vt_9_2,  vt_9_3,  vt_9_4,  vt_9_5,  vt_9_6,
    vt_9_7,  vt_9_8,  vt_9_9,  vt_9_10, vt_9_11, vt_9_12, vt_9_13,
    vt_9_14, vt_9_15, vt_9_16, vt_9_17, vt_9_18, vt_9_19, vt_9_20,
};
ThunkFn const kControllerThunks[64] = {
    vt_10_0, vt_10_1, vt_10_2, vt_10_3, vt_10_4, vt_10_5, vt_10_6, vt_10_7,
    vt_10_8, vt_10_9, vt_10_10, vt_10_11, vt_10_12, vt_10_13, vt_10_14, vt_10_15,
    vt_10_16, vt_10_17, vt_10_18, vt_10_19, vt_10_20, vt_10_21, vt_10_22, vt_10_23,
    vt_10_24, vt_10_25, vt_10_26, vt_10_27, vt_10_28, vt_10_29, vt_10_30, vt_10_31,
    vt_10_32, vt_10_33, vt_10_34, vt_10_35, vt_10_36, vt_10_37, vt_10_38, vt_10_39,
    vt_10_40, vt_10_41, vt_10_42, vt_10_43, vt_10_44, vt_10_45, vt_10_46, vt_10_47,
    vt_10_48, vt_10_49, vt_10_50, vt_10_51, vt_10_52, vt_10_53, vt_10_54, vt_10_55,
    vt_10_56, vt_10_57, vt_10_58, vt_10_59, vt_10_60, vt_10_61, vt_10_62, vt_10_63,
};
ThunkFn const kStreamThunks[kStreamReaderMaxSlots] = {
    vt_8_0,  vt_8_1,  vt_8_2,  vt_8_3,  vt_8_4,  vt_8_5,  vt_8_6,  vt_8_7,
    vt_8_8,  vt_8_9,  vt_8_10, vt_8_11, vt_8_12, vt_8_13, vt_8_14, vt_8_15,
    vt_8_16, vt_8_17, vt_8_18, vt_8_19, vt_8_20, vt_8_21, vt_8_22, vt_8_23,
    vt_8_24, vt_8_25, vt_8_26, vt_8_27, vt_8_28, vt_8_29, vt_8_30, vt_8_31,
};

// Which mode the environment asked for: "" (off), "log".
std::string mode() {
    const char* e = std::getenv("NX_MOVIE_VTABLE");
    return e ? std::string(e) : std::string();
}

// Patch one vtable in the loaded image. Returns the number of slots rewritten.
int patch_vtable(uint64_t vtable_guest, int slots, int which, ThunkFn const* thunks, const char* cls,
                 int first_slot = 2, bool skip_invalid = false) {
    uint64_t* vt = reinterpret_cast<uint64_t*>(uintptr_t(vtable_guest));
    int patched = 0;
    for (int i = first_slot; i < slots; i++) {
        const uint64_t original = vt[i];
        // Slots 0/1 are the vtable's offset-to-top and RTTI pointer, not functions: leave them alone.
        if (i < 2) continue;
        // A sanity check that this really is a function table: every method slot must point into a loaded module.
        if (original && !module_at(original)) {
            if (skip_invalid) continue;  // a runtime-discovered table may legitimately have gaps
            log("movie-vt: %s slot %d holds %llx, which is not in any module: leaving the vtable alone", cls, i,
                (unsigned long long)original);
            return 0;
        }
        g_slots[which][i].real = original;
        // hle_address_or() returns the guest stub for this name, creating it on first use; the thunk function is the
        // fallback it dispatches to, so the stub calls our code even though nothing registered the name beforehand.
        char thunk_name[64];
        std::snprintf(thunk_name, sizeof thunk_name, "<movie %d slot %d>", which, i);
        vt[i] = hle_address_or(thunk_name, thunks[i]);
        patched++;
    }
    log("movie-vt: %s vtable at %llx: %d slots redirected (originals kept for tail calls)", cls,
        (unsigned long long)vtable_guest, patched);
    return patched;
}

// NX_MOVIE_CTL=1: patch the movie controller's vtable (main-local symbol, found from the live object). The tick
// (main+cd6318), the renderer's readiness query (main+cd77e4) and its frame fetch (main+cd786c) are virtual methods
// of this class, and for ev0005 the third one is never called -- this says whether the renderer asks at all, and from
// where. Every slot is logged with the function it holds on the first call, so the table maps itself.
void patch_controller(uint64_t ctl) {
    static uint64_t patched = 0;
    if (!ctl || !std::getenv("NX_MOVIE_CTL")) return;
    const uint64_t vptr = ld<uint64_t>(ctl);
    if (!vptr || !module_at(vptr)) return;
    const uint64_t vt = vptr - 16;
    if (vt == patched) return;
    // Walk to the last module-resident method. The entry after it is padding or the next class's offset-to-top,
    // and writing a thunk over either would corrupt a neighbouring table, so trailing nulls are never patched.
    int slots = 2, last = 1, gaps = 0;
    while (slots < 32) {
        const uint64_t fn = ld<uint64_t>(vt + uint64_t(slots) * 8);
        if (fn && module_at(fn)) {
            last = slots;
            gaps = 0;
        } else if (++gaps >= 2) {
            break;
        }
        slots++;
    }
    slots = last + 1;
    const VmInfo wv = vm_query(vt);
    if (slots <= 2 || !(wv.perm & 2)) {
        log("movie-vt: controller vtable at %llx (%d slots): %s; not patched", (unsigned long long)vt, slots,
            slots <= 2 ? "no module-resident slots" : "memory is not writable");
        return;
    }
    for (int i = 2; i < slots; i++)
        log("movie-vt:   controller slot %2d -> %s", i, describe(ld<uint64_t>(vt + uint64_t(i) * 8)).c_str());
    // Slots 7..13: the renderer's 9/10/11/12 plus the command methods 7/8 — slot 8 is main_00CD7388, the one that
    // ORs 0x4000 (bit 14) into the movie system's flags, so seeing whether it runs for a controller matters.
    // The tick (slot 6) is left alone on purpose: it runs every frame and this probe is not worth perturbing it.
    const int first = 7;
    const int n = slots > first ? patch_vtable(vt, slots, kWhichController, kControllerThunks, "Ctl", first, true) : 0;
    if (n) patched = vt;
}

// NX_MOVIE_ATTACH=1: give the movie system that has just been bound to this controller the "attached" flag bit that
// every other movie gets from main_00CDA380 (the registry-id callback: `[x21+0x16c] |= 0x10000`). Without bit 16 the
// controller's command method main_00CD7388 refuses to raise 0x4000 in that same word, and the renderer's gate in
// main_00CD9044 (`tst w8,#0xc000`) skips the movie entirely — which is exactly ev0005 (flags stuck at 1/3 while
// ev0000 reached 0x1014303 and rendered). The registry is [list+0x30] (entries 16 bytes: id at +4, node at +8, the
// movie system at node+0x18), count at [list+0x38] — the same array main_00CDA380 indexes by id.
// GATED attach (NX_MOVIE_ATTACH=1): deliver the "attached and prepared" bit only when BOTH hold —
//   (a) the controller bound to the movie system shows [+0x38c] != 0 (the code_269.cpp:10841 store, which writes
//       0x100000001 into +0x38c/+0x390 when playback is prepared), and
//   (b) the movie system's flags still lack 0x10000 at that same moment (re-checked here, not at registration).
// Why the timing is a hard constraint: run 14 delivered the bit at controller verification and ev0000 never closed
// its movie afterwards — the second cutscene was reached 4/4 runs without the bit and 0/3 with it. So 0x10000 is a
// state the game raises at a specific point in playback, not a registration flag. This is called from the event poll
// (every frame of a running movie), never from SetDataSource.
int attach_controller(uint64_t ctl) {
    const uint64_t slot = kModuleBase + 0x24ac6b8ull;
    if (!module_at(slot)) return 0;
    const uint64_t list = ld<uint64_t>(slot);
    if (!list || vm_query(list).state == 0) return 0;
    const uint32_t count = ld<uint32_t>(list + 0x38);
    const uint64_t array = ld<uint64_t>(list + 0x30);
    if (!count || count >= 64 || !array || vm_query(array).state == 0) return 0;
    int attached = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t node = ld<uint64_t>(array + uint64_t(i) * 16 + 8);
        if (!node || vm_query(node).state == 0) continue;
        const uint64_t ms = node + 0x18;
        if (vm_query(ms).state == 0 || ld<uint64_t>(ms + 0x330) != ctl) continue;
        if (ld<uint32_t>(ms + 0x16c) & 0x10000u) {
            log("movie-vt: movie system %llx (registry[%u]) already carries bit 16 for controller %llx", 
                (unsigned long long)ms, i, (unsigned long long)ctl);
            return 1;  // the game attached it itself
        }
        const uint32_t before = ld<uint32_t>(ms + 0x16c);
        const uint32_t after = before | 0x10000u;
        if (after != before) st<uint32_t>(ms + 0x16c, after);
        log("movie-vt: attached movie system %llx (registry[%u]) to controller %llx: flags16c %x -> %x", 
            (unsigned long long)ms, i, (unsigned long long)ctl, before, after);
        ++attached;
    }
    return attached;
}

// Discover and patch the ClientStreamReader's vtable from a live object: the class is main-module code and its
// vtable symbol is local (not in table.cpp), so the object's vptr is the only way to it. The vptr points at
// vtable+16 (slots 0/1 are offset-to-top and RTTI, as patch_vtable expects). The slot count is discovered by
// walking until a slot no longer points into a loaded module (the object's class has ~11 virtuals).
void patch_stream_reader(uint64_t reader) {
    static std::mutex m;
    static uint64_t patched = 0;
    if (!reader) return;
    std::lock_guard lk(m);
    const VmInfo rv = vm_query(reader);
    if (rv.state == 0 || !(rv.perm & 1)) return;
    const uint64_t vptr = ld<uint64_t>(reader);
    if (!vptr || !module_at(vptr)) return;
    const uint64_t vt = vptr - 16;
    const LoadedModule* mod = module_at(vptr);
    // The reader is embedded at controller+8; the controller the tick uses is reader-8. Verify it through the
    // extractor at controller+0x158 (its first word must be a subsdk0 vtable pointer) and remember it for the gate.
    const uint64_t ctl = reader - 8;
    const VmInfo cv = vm_query(ctl);
    const uint64_t extractor = cv.state != 0 ? ld<uint64_t>(ctl + 0x158) : 0;
    const uint64_t evptr = extractor && vm_query(extractor).state != 0 ? ld<uint64_t>(extractor) : 0;
    const bool verified =
        cv.state != 0 && evptr && module_at(evptr) && std::strstr(module_at(evptr)->info->name, "subsdk0") != nullptr;
    if (verified) {
        g_controller.store(ctl, std::memory_order_relaxed);
        patch_controller(ctl);
    }
    if (vt != patched) {
        int slots = 2, gaps = 0;
        while (slots < kStreamReaderMaxSlots) {
            const uint64_t fn = ld<uint64_t>(vt + uint64_t(slots) * 8);
            gaps = (fn && module_at(fn)) ? 0 : gaps + 1;
            if (gaps >= 4) break;  // four non-function entries in a row: the table ended
            slots++;
        }
        const VmInfo wv = vm_query(vt);
        if (slots <= 2 || !(wv.perm & 2)) {
            log("movie-vt: ClientStreamReader vtable at %llx (%d slots, %s): %s; not patched",
                (unsigned long long)vt, slots, mod->info->name,
                slots <= 2 ? "no module-resident slots" : "memory is not writable");
        } else {
            const int n = patch_vtable(vt, slots, kWhichStream, kStreamThunks, "StreamReader", 2, true);
            if (n) {
                patched = vt;
                log("movie-vt: ClientStreamReader vtable at %llx (%d slots, module %s) patched from reader %llx "
                    "(%d slots redirected)", (unsigned long long)vt, slots, mod->info->name,
                    (unsigned long long)reader, n);
            }
        }
    }
    if (!verified) {
        log("movie-vt: controller candidate %llx not verified (extractor %llx vptr %llx)", (unsigned long long)ctl,
            (unsigned long long)extractor, (unsigned long long)evptr);
        return;
    }
    log("movie-vt: controller %llx (reader-8, extractor %llx verified) state: flag3a4 %u flag3c4 %u flag380 %u "
        "media %llx", (unsigned long long)ctl, (unsigned long long)extractor, ld<uint32_t>(ctl + 0x3a4),
        ld<uint32_t>(ctl + 0x3c4), ld<uint32_t>(ctl + 0x380), (unsigned long long)ld<uint64_t>(ctl + 0x160));
}

}  // namespace

namespace plat {

// Called once after the modules are loaded and relocated (the vtables are data with relocations, so they only hold
// real addresses once load_modules() has run). This build compiles the patch in (NX_MOVIE_VTABLE=ON at configure
// time), so it is installed by DEFAULT: it is the movie fix, not an experiment (NX_MOVIE_VTABLE=0 disables it, =log
// keeps the per-call logging).
void movie_vtable_init() {
    const std::string m = mode();
    log("movie-vt: init called, NX_MOVIE_VTABLE=%s", m.empty() ? "(default: frame source only)" : m.c_str());
    if (m == "0") return;
    if (m.empty()) {
        // movie::Decoder is patched at the first movie open (install_decoder_probe via movie_vtable_late_player), never
        // during boot: the boot-time NvMMLite path must not see a host thunk.
        g_frame_only = true;
        return;
    }
    if (!m.empty() && m != "log" && m != "1") {
        log("movie-vt: NX_MOVIE_VTABLE=%s is not a known mode (use log, 1 or 0); installing the default", m.c_str());
    }
    // kSubsdk0Base comes from runtime.h (NX_BASE + 0x8000000): the same base load_modules() places subsdk0 at.
    const uint64_t nncodec = kSubsdk0Base + kNnCodecVtable;
    const uint64_t extractor = kSubsdk0Base + kExtractorVtable;
    if (!module_at(nncodec)) {
        log("movie-vt: subsdk0 is not loaded at the expected address; not patching");
        return;
    }
    patch_vtable(nncodec, kNnCodecSlots, kWhichNnCodec, kNnCodecThunks, "NnCodec");
    patch_vtable(extractor, kExtractorSlots, kWhichExtractor, kExtractorThunks, "Extractor");
    // The demuxer the game really drives (§15.1): NnExtractor derives from Extractor.
    patch_vtable(kSubsdk0Base + kNnExtractorVtable, kNnExtractorSlots, kWhichNnExtractor, kNnExtractorThunks,
                 "NnExtractor");
    // The player: `NX_MOVIE_VTABLE_PLAYER=1` patches it here; `=late` waits for the first cutscene open (see below)
    // because one of its slots is dispatched during boot by something that does not yet tolerate a host thunk (§17),
    // and this is exactly the one bisect that §17 could not finish.
    install_media_probes();
    const char* also = std::getenv("NX_MOVIE_VTABLE_PLAYER");
    if (also && *also && *also != '0' && std::string(also) != "late")
        patch_vtable(kSubsdk0Base + kNnCodecPlayerVtable, kNnCodecPlayerSlots, kWhichPlayer, kNnCodecPlayerThunks,
                     "Player");
}

// Applied on the first cutscene file open when NX_MOVIE_VTABLE_PLAYER=late. Every player call from that point on
// (Prepare/Start/GetState/FillOutputBuffer/...) is logged with its caller, so the class's own state machine says
// where ev0005 stops instead of being inferred from the demuxer side. Objects constructed before the patch use the
// same static table, so they are covered too.
void movie_vtable_late_player() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    if (g_frame_only) {
        install_decoder_probe();
        return;
    }
    install_media_probes();
    const char* also = std::getenv("NX_MOVIE_VTABLE_PLAYER");
    if (!also || std::string(also) != "late") return;
    const std::string m = mode();
    if (m.empty() || m == "0" || (m != "log" && m != "1")) return;
    log("movie-vt: first cutscene open: patching the Player and its event plumbing now (late)");
    patch_vtable(kSubsdk0Base + kNnCodecPlayerVtable, kNnCodecPlayerSlots, kWhichPlayer, kNnCodecPlayerThunks,
                 "Player");
    // The decoder -> player direction: are input/output buffers ever announced, does a format change arrive, does an
    // error arrive? This is the half of the handshake that ev0005 never shows.
    patch_vtable(kSubsdk0Base + kVideoCodecCallbackVtable, kVideoCodecCallbackSlots, kWhichVideoCallback,
                 kVideoCallbackThunks, "VideoCb");
    // The player's message handlers: patching only slot 8 leaves the RefBase/dtor slots alone.
    patch_vtable(kSubsdk0Base + kNnCodecMessageReceiverVtable, kNnCodecMessageReceiverSlots, kWhichNnCodecMsg,
                 kNnCodecMsgThunks, "NnCodecMsg", 8);
    patch_vtable(kSubsdk0Base + kNnCodecVideoOutReceiverVtable, kNnCodecVideoOutReceiverSlots, kWhichVideoOut,
                 kVideoOutThunks, "VideoOutMsg", 8);
    patch_vtable(kSubsdk0Base + kNnCodecAudioOutReceiverVtable, kNnCodecAudioOutReceiverSlots, kWhichAudioOut,
                 kAudioOutThunks, "AudioOutMsg", 8);
}

// Installed late (first cutscene open), so the boot-time NvMMLite path never sees a host thunk on this table.
void install_decoder_probe() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    log("movie-vt: installing the Decoder vtable probe (late)");
    patch_vtable(kSubsdk0Base + kDecoderVtable, kDecoderSlots, kWhichDecoder, kDecoderThunks, "Decoder");
}

// One line per slot that has ever been entered, logged at each movie open/close. The per-slot log above is capped;
// this is the complete call profile, which is what says "the player never reached Start" without a second run.
void movie_vtable_counts(const char* why) {
    const std::string m = mode();
    if (m.empty() || m == "0") return;
    {
        // Which playback is current at this instant: the controller the tick polls, and the decoder object it holds
        // at +0x160. Every line below can then be attributed to a movie by object address.
        const uint64_t ctl = g_controller.load(std::memory_order_relaxed);
        const uint64_t dec = ctl && vm_query(ctl).state != 0 ? ld<uint64_t>(ctl + 0x160) : 0;
        log("movie-vt: call counts at %s (current controller %llx, its decoder %llx):", why, (unsigned long long)ctl,
            (unsigned long long)dec);
        for (int e = 0; e < 16; e++) {
            const uint64_t addr = g_client_events[e].addr.load(std::memory_order_relaxed);
            if (!addr) continue;
            log("movie-vt:   client event %llx owner %llx %-22s signals %u", (unsigned long long)addr,
                (unsigned long long)g_client_events[e].owner.load(std::memory_order_relaxed),
                kClientEventNames[g_client_events[e].index.load(std::memory_order_relaxed)],
                g_client_events[e].signals.load(std::memory_order_relaxed));
        }
        for (int w = 0; w < kWhichClasses; w++) {
            for (int o = 0; o < kTrackedObjects; o++) {
                const uint64_t obj = g_objects[w][o].obj.load(std::memory_order_relaxed);
                if (!obj) continue;
                char slots[512];
                int k = 0;
                slots[0] = 0;
                for (int i = 2; i < kTrackedSlots && k < int(sizeof slots) - 12; i++)
                    k += std::snprintf(slots + k, unsigned(sizeof slots - k), "%d=%u ", i,
                                       g_objects[w][o].calls[i].load(std::memory_order_relaxed));
                log("movie-vt:   object %s %llx: %s", class_name(w), (unsigned long long)obj, slots);
            }
        }
    }
    for (int w = 0; w < kWhichClasses; w++) {
        for (int i = 2; i < 64; i++) {
            const int n = g_slots[w][i].calls.load(std::memory_order_relaxed);
            if (!n) continue;
            log("movie-vt:   %s slot %2d %-38s %d", class_name(w), i, slot_name(w, i), n);
        }
    }
}

// Called at every movie open, after that movie's predecessor has been profiled: the per-slot counters and the
// per-event log budgets restart, so `movie_vtable_counts("movie close")` reports THAT movie's own calls. Without
// this, one movie's thousands of AcquireInputBufferIndex calls spend the whole per-slot log budget (NX_MOVIE_VTABLE_CAP
// is per class+slot, not per movie) and the second movie's decoder traffic is invisible — which is exactly the
// run-10/11 reading this replaces.
void movie_vtable_new_session() {
    const std::string m = mode();
    if (m.empty() || m == "0") return;
    for (int w = 0; w < kWhichClasses; w++)
        for (int i = 0; i < 64; i++) g_slots[w][i].calls.store(0, std::memory_order_relaxed);
    g_try_budget = EventLogBudget{};
    g_clear_budget = EventLogBudget{};
    g_signal_budget = EventLogBudget{};
    g_shim_budget = EventLogBudget{};
}

}  // namespace plat
}  // namespace nx
