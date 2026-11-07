#pragma once

namespace nx::nvn::replay {
// Caller holds gpu_lock and has fully retired all submits, and must have
// successfully waited for the queue to become idle. Safe to call repeatedly.
void written_kept_shutdown();
}
