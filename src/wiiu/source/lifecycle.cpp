#include "lifecycle.h"

namespace wiiu {

void ExitRequest::request(std::uint64_t titleId, void (*launchMenu)(), void (*stopLauncher)()) {
    if (requested_) return;
    requested_ = true;
    const bool launcher = titleId == 0x0005000013374842ULL ||
        titleId == 0x000500101004A000ULL || titleId == 0x000500101004A100ULL ||
        titleId == 0x000500101004A200ULL;
    if (launcher) stopLauncher();
    else launchMenu();
}

} // namespace wiiu
