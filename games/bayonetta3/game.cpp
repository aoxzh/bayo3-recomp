#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"bayonetta3", "Bayonetta 3", 0x01004a4010fea000ull,
                                "io.github.bayonetta3recomp", "private/run-bayonetta3",
                                0 /* main.npdm 1.2.0 META system resource size */};

void game::after_init() {}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
