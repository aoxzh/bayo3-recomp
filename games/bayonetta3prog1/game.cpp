// Bayonetta 3's second program (title 01004a4010fea001, ProgramIndex 1 of 01004a4010fea000).
//
// The bar hub's "Records of Time" book does not load a chapter in-process: it calls
// IApplicationFunctions::ExecuteProgram with ProgramSpecifyKind 0 (ProgramIndex) and value 1, i.e. the console
// starts ApplicationId+1 and hands it the user-channel storage the game pushed just before (see
// docs/CHAPTER1-AUTODRIVE.md §12.6-12.8). This folder builds that program: same runtime, two modules (main + sdk;
// it ships no subsdk0), its own title id and data root. The launch parameter the console passes is served by the
// runtime from <data root>/user_channel.bin.
#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"bayonetta3prog1", "Bayonetta 3", 0x01004a4010fea001ull,
                                "io.github.bayonetta3recomp", "private/run-prog1",
                                0 /* main.npdm META system resource size */};

void game::after_init() {}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
