#pragma once

namespace checkpoint {
// Called at a stopped game boundary after native framebuffer work completes.
void render_state(bool restore);
}
