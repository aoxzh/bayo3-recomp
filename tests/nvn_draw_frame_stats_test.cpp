#include "../runtime/src/nvn/nvn_draw_frame_stats.h"
#include <cassert>
#include <cstdio>

using namespace nx::nvn::draw_frame_stats;

int main() {
    uint64_t previous = 0;
    for (uint64_t present = 1; present <= 900; ++present) {
        const uint64_t window = log_window(present);
        if (present % window != 0) continue;
        assert(present - previous == window);
        previous = present;
    }
    assert(previous == 900);
    Tracker t;
    t.draw(Kind::Arrays, 0);
    t.call(10, 0);
    t.draw(Kind::Elements, 1);
    t.call(20, 1);
    t.draw(Kind::Indirect, 2);
    t.call(10, 0);
    t.draw(Kind::Elements, 1);
    t.call(30, 1); // a new child beneath a repeated parent inherits the repeat flag
    t.draw(Kind::MultiIndirect, 2);
    t.call(40, 0); // leaving that branch clears the repeat flag for its sibling
    t.draw(Kind::Elements, 1);
    Sample a = t.present(4, 6, 5);
    assert(a.first && a.ordered && a.sequence == 1);
    assert(a.recorded == 4 && a.replayed == 6 && a.emitted == 5);
    assert(a.kinds[0] == 1 && a.kinds[1] == 3 && a.kinds[2] == 1 && a.kinds[3] == 1);
    assert(a.nested_draws == 5 && a.repeated_branch_draws == 2);
    assert(a.calls == 5 && a.repeated_calls == 1);
    t.call(10, 0); // call histories reset at each present
    t.draw(Kind::Elements, 1);
    Sample b = t.present(6, 7, 6);
    assert(!b.first && b.ordered && b.sequence == 2);
    assert(b.recorded == 2 && b.replayed == 1 && b.emitted == 1);
    assert(b.calls == 1 && b.repeated_calls == 0 && b.repeated_branch_draws == 0);
    Sample c = t.present(5, 8, 7); // a late guest sample must not discard replay deltas
    assert(!c.ordered && c.recorded == 0 && c.replayed == 1 && c.emitted == 1);
    Sample d = t.present(8, 10, 8); // guest resumes from the earlier high-water mark of six
    assert(d.ordered && d.recorded == 2 && d.replayed == 2 && d.emitted == 1);
    std::puts("PASS per-present draw accounting and repeated list classification");
}
