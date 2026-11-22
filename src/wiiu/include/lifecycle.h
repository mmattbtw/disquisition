#pragma once

#include <cstdint>

namespace wiiu {

// Aroma titles must request a menu transition and continue pumping ProcUI.
// libwhb's legacy HBL/Mii Maker path instead stops its loop and relaunches HBL
// in WHBProcShutdown. Keep the title IDs in sync with libwhb's proc.c.
class ExitRequest {
public:
    void request(std::uint64_t titleId, void (*launchMenu)(), void (*stopLauncher)());
    bool requested() const { return requested_; }
private:
    bool requested_ = false;
};

} // namespace wiiu
