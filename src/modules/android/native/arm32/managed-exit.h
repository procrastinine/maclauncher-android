#pragma once

namespace maclauncher::arm32 {
// Internal control transfer after an emulated import has returned. It never
// crosses Dynarmic execution, a Java frame, or a native callback ABI boundary.
struct ManagedThreadExit {};
}
