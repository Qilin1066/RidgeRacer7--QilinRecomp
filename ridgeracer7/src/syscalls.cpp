// The system calls this runtime provides, by number (the names are
// syscall_names.cpp's). As with the imports: one for each the game makes,
// added as the game reaches it; any other stops the game and names itself.
#include "machine.hpp"

namespace rr7 {

std::span<const NativeSystemCall> native_system_calls() { return {}; }

}  // namespace rr7
