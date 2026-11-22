// Tessera - block-state dependent textures for Minecraft Bedrock (LeviLamina client mod).
#include "game/Engine.h"

#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"

namespace tessera {

class TesseraMod {
public:
    static TesseraMod& getInstance() {
        static TesseraMod instance;
        return instance;
    }

    TesseraMod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    // Hooks go in load(): BlockGraphics::initBlocks runs during game start-up.
    bool load() { return game::install(mSelf.getLogger(), mSelf.getDataDir()); }
    bool enable() { return true; }
    bool disable() { return true; }
    bool unload() {
        game::uninstall();
        return true;
    }

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace tessera

LL_REGISTER_MOD(tessera::TesseraMod, tessera::TesseraMod::getInstance());
