// Checks the public API end to end, the way another mod would use it: through
// include/Tessera/Api.h's Client (GetModuleHandle + GetProcAddress on Tessera.dll).
//
// F3+K: on the block under the player, cycle "tesseratest:level" 1 -> 2 -> 3 -> cleared.
// A resource pack rule can then react to it (see packs/TesseraApiTest).
#include "game/ApiSelfTest.h"
#include "game/Engine.h"

#include "Tessera/Api.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/item/ItemStack.h"

#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <map>
#include <tuple>

namespace tessera::game::apitest {

// A provider, registered the way another mod would: a checkerboard by position.
bool parity(int, int x, int, int z, char const*, void*, tessera_value* out) {
    out->kind = TESSERA_VALUE_BOOL;
    out->i    = ((x + z) & 1) != 0;
    return true;
}

// Only with data/api_test.txt next to Tessera.dll (a test, not for players).
bool enabled() {
#ifdef _WIN32
    static bool const on = [] {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&enabled), &self))
            return false;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(self, path, MAX_PATH)) return false;
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::path(path).parent_path() / "data" / "api_test.txt", ec);
    }();
    return on;
#else
    return false;
#endif
}

void frame() {
    if (!enabled()) return;
    static bool registered = false;
    if (!registered) {
        registered = true;
        api::Client c;
        bool const ok = c.registerProvider("tesseratest:parity", &parity);
        chat(std::format("API test: provider tesseratest:parity {}", ok ? "registered" : "NOT registered"));
    }
    // API 3: the held item's model, reported when the held item changes.
    if (auto client = ll::service::getClientInstance(); client)
        if (auto* player = client->getLocalPlayer()) {
            static std::string lastHeld;
            ItemStack const&   held = player->getSelectedItem();
            std::string const  name = held.isNull() ? std::string{} : held.getTypeName() + ":" + std::to_string(held.getAuxValue());
            if (name != lastHeld) {
                lastHeld = name;
                static api::Client models;
                tessera_quad       first{};
                int const          n = name.empty() ? 0 : models.itemModel(&held, &first, 1);
                if (n > 0)
                    chat(std::format("API test: item model {} = {} quads, first corner ({:.3f},{:.3f},{:.3f})", name, n, first.pos[0], first.pos[1],
                                     first.pos[2]));
            }
        }
#ifdef _WIN32
    static bool wasDown = false;
    bool        down    = false;
    DWORD       pid     = 0;
    if (HWND const fg = GetForegroundWindow(); fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId())
        down = (GetAsyncKeyState(VK_F3) & 0x8000) && (GetAsyncKeyState('K') & 0x8000);
    bool const pressed = down && !wasDown;
    wasDown            = down;
    if (!pressed) return;

    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    auto* region = client ? client->getRegion() : nullptr;
    if (!player || !region) return;
    auto const feet = player->getFeetPos();
    int const  x = int(std::floor(feet.x)), y = int(std::floor(feet.y)) - 1, z = int(std::floor(feet.z));
    int const  dim = int(region->getDimensionId());

    static api::Client                                   tessera;
    static std::map<std::tuple<int, int, int, int>, int> levels; // what this test set
    if (!tessera.available()) {
        chat("API test: Tessera.dll not found through the public header");
        return;
    }
    int& level = levels[{dim, x, y, z}];
    level      = (level + 1) % 4;
    bool ok    = false;
    if (level == 0) ok = tessera.clear(dim, x, y, z, "tesseratest:level");
    else ok = tessera.setInt(dim, x, y, z, "tesseratest:level", level);
    chat(std::format("API test: ({},{},{}) tesseratest:level = {} -> {}", x, y, z,
                     level ? std::to_string(level) : std::string("cleared"), ok ? "accepted" : "refused"));
#endif
}

} // namespace tessera::game::apitest
