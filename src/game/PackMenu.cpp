#include "game/PackMenu.h"
#include "game/Engine.h"

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/deps/core/resource/PackType.h"
#include "mc/deps/core/resource/ResourcePackStackType.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/json/Value.h"
#include "mc/network/LoopbackPacketSender.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/packet/ActorEventPacket.h"
#include "mc/network/packet/AvailableCommandsPacket.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/network/packet/ModalFormRequestPacket.h"
#include "mc/network/packet/ModalFormResponsePacket.h"
#include "mc/platform/UUID.h"
#include "mc/resources/IResourcePackRepository.h"
#include "mc/resources/Pack.h"
#include "mc/resources/PackInstance.h"
#include "mc/resources/ResourcePack.h"
#include "mc/resources/ResourcePackLoader.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/resources/ResourcePackStack.h"
#include "mc/scripting/modules/PackManifest.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tessera::game::packmenu {
namespace {

ll::io::Logger* gLog = nullptr;

// ---------------------------------------------------------------------------
// Packs
// ---------------------------------------------------------------------------
struct Entry {
    std::string id;        // UUID text
    int         version[3] = {0, 0, 0};
    std::string subpack;   // as stored in global_resource_packs.json
    std::string name;      // shown in the menu
};

std::vector<Entry> gActive;    // pending global stack, top (highest priority) first
std::vector<Entry> gInactive;  // the user's other resource packs
std::string        gOriginal;  // global_resource_packs.json as it was when the menu opened
std::vector<std::string> gOriginalOrder; // pack ids in that file, top first
std::filesystem::path gStackFile;

std::string serialise(std::vector<Entry> const& packs) {
    std::string out = "[\n";
    for (size_t i = 0; i < packs.size(); ++i) {
        auto const& e = packs[i];
        out += std::format(
            "\t{{\n\t\t\"pack_id\" : \"{}\",\n\t\t\"version\" : [ {}, {}, {} ]{}\n\t}}{}\n",
            e.id, e.version[0], e.version[1], e.version[2],
            e.subpack.empty() ? std::string{} : std::format(",\n\t\t\"subpack\" : \"{}\"", e.subpack),
            i + 1 < packs.size() ? "," : ""
        );
    }
    return out + "]\n";
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(uchar(c)));
    return s;
}

PackManifest const* manifestOf(ResourcePack const& pack) {
    try {
        // TypedStorage::get() gives the stored smart pointer; -> goes through it.
        return pack.mImpl.get()->mPack.get()->mManifest.get();
    } catch (...) {
        return nullptr;
    }
}

std::string displayName(PackManifest const& m) {
    std::string name = *m.mName;
    if (name.empty() || name.starts_with("pack.")) { // localisation key: use the folder name instead
        std::string path = m.mLocation->mPath->value;
        while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
        if (auto const cut = path.find_last_of("/\\"); cut != std::string::npos) path = path.substr(cut + 1);
        if (!path.empty()) name = path;
    }
    return name;
}

// Reads the global stack from global_resource_packs.json and the user's packs from
// the repository. The file is the game's own record of the global stack.
bool loadPacks(IMinecraftGame& game) {
    gActive.clear();
    gInactive.clear();
    auto& repo = game.getResourcePackRepository();

    std::string skins = repo.getCustomSkinDirectoryPath().value; // …/com.mojang/custom_skins
    while (!skins.empty() && (skins.back() == '/' || skins.back() == '\\')) skins.pop_back();
    std::filesystem::path base{std::u8string(skins.begin(), skins.end())};
    gStackFile = base.parent_path() / "minecraftpe" / "global_resource_packs.json";
    std::ifstream in(gStackFile, std::ios::binary);
    if (!in) {
        if (gLog) gLog->warn("pack menu: cannot read {}", gStackFile.string());
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    gOriginal = text.str();

    std::regex const object(R"(\{[^{}]*\})");
    std::regex const idRe(R"re("pack_id"\s*:\s*"([^"]+)")re");
    std::regex const verRe(R"re("version"\s*:\s*\[\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\])re");
    std::regex const subRe(R"re("subpack"\s*:\s*"([^"]*)")re");
    for (auto it = std::sregex_iterator(gOriginal.begin(), gOriginal.end(), object); it != std::sregex_iterator(); ++it) {
        std::string const obj = it->str();
        std::smatch       m;
        if (!std::regex_search(obj, m, idRe)) continue;
        Entry e;
        e.id = lower(m[1].str());
        if (std::regex_search(obj, m, verRe))
            for (int k = 0; k < 3; ++k) e.version[k] = std::stoi(m[k + 1].str());
        if (std::regex_search(obj, m, subRe)) e.subpack = m[1].str();
        e.name = e.id;
        gActive.push_back(std::move(e));
    }

    for (auto* pack : repo.getPacksByType(PackType::Resources)) {
        if (!pack) continue;
        auto const* m = manifestOf(*pack);
        if (!m || m->mIsHidden) continue;
        auto const origin = PackOrigin(m->mPackOrigin);
        if (origin != PackOrigin::User && origin != PackOrigin::Dev) continue;
        Entry e;
        e.id         = lower(m->mIdentity->mId->asString());
        auto const& v = *m->mIdentity->mVersion;
        e.version[0]  = v.mMajor;
        e.version[1]  = v.mMinor;
        e.version[2]  = v.mPatch;
        e.name        = displayName(*m);
        auto active   = std::find_if(gActive.begin(), gActive.end(), [&](Entry const& a) { return a.id == e.id; });
        if (active != gActive.end()) active->name = e.name;
        else if (std::none_of(gInactive.begin(), gInactive.end(), [&](Entry const& a) { return a.id == e.id; }))
            gInactive.push_back(std::move(e));
    }
    std::sort(gInactive.begin(), gInactive.end(), [](Entry const& a, Entry const& b) { return a.name < b.name; });
    gOriginalOrder.clear();
    for (auto const& e : gActive) gOriginalOrder.push_back(e.id);
    return true;
}

std::string describe(ResourcePackStack const* stack) {
    if (!stack) return "(none)";
    std::string out;
    for (auto const& instance : *stack->mStack) {
        ResourcePack const& pack = *instance.mPack;
        auto const*         m    = manifestOf(pack);
        out += (out.empty() ? "" : ", ") + (m ? displayName(*m) : std::string("?"));
    }
    return out.empty() ? "(empty)" : out;
}

void logStacks(ResourcePackManager& mgr, char const* when) {
    if (!gLog) return;
    std::string full;
    try {
        mgr.iteratePacks([&](PackInstance const& instance) {
            ResourcePack const& pack = *instance.mPack;
            auto const*         m    = manifestOf(pack);
            full += (full.empty() ? "" : ", ") + (m ? displayName(*m) : std::string("?"));
        });
    } catch (...) {
        full = "(failed)";
    }
    gLog->info(
        "pack stacks [{}]: global=[{}] level=[{}] useGlobal={} pendingRestack={} full=[{}]", when,
        describe(mgr.mGlobalStack.get()), describe(mgr.mLevelStack.get()), bool(mgr.mUseGlobalPackStack),
        bool(mgr.mPendingRestack), full
    );
}

// What ResourcePackManager::load serves for the rules file right now: the stack the
// game reads from, as opposed to the component stacks iteratePacks walks.
long long probe(ResourcePackManager& mgr, char const* when) {
    std::string            text;
    ResourceLocation const where{Core::PathView{"state_textures.json"}, ResourceFileSystem::UserPackage};
    bool const             found = mgr.load(where, text);
    void* const            first = *reinterpret_cast<void* const*>(std::addressof(static_cast<ResourcePackLoader&>(mgr).mUnkf914db));
    if (gLog)
        gLog->info(
            "pack probe [{}]: state_textures.json {} ({} bytes); initializing={} gameplayLoaded={} pendingRestack={} loader[0]={}",
            when, found ? "found" : "not found", text.size(), bool(mgr.mInitializing), bool(mgr.mGameplayResourcesLoaded),
            bool(mgr.mPendingRestack), first
        );
    return found ? (long long)text.size() : -1;
}

bool apply(IMinecraftGame& game) {
    std::string const json = serialise(gActive);
    auto&             repo = game.getResourcePackRepository();
    auto&             mgr  = game.getResourcePackManager();
    Bedrock::NotNullNonOwnerPtr<IResourcePackRepository const> repoPtr{
        Bedrock::NonOwnerPointer<IResourcePackRepository const>(repo)
    };
    auto stack = ResourcePackStack::deserialize(json, repoPtr, std::nullopt);
    if (!stack) {
        if (gLog) gLog->error("pack menu: the game could not build the new stack");
        return false;
    }
    size_t const    count  = stack->mStack->size();
    logStacks(mgr, "before");
    long long const served = probe(mgr, "before");
    // With a world open the game only marks the change as pending and keeps serving
    // the stack it composed before. Try, in turn, ways of getting it composed now;
    // stop at the first after which the served rules file changes.
    auto changed = [&](char const* when) { return probe(mgr, when) != served; };
    auto fresh   = [&] { return ResourcePackStack::deserialize(json, repoPtr, std::nullopt); };
    bool const loaded       = mgr.mGameplayResourcesLoaded;
    bool const initializing = mgr.mInitializing;
    bool const set          = mgr.setStack(std::move(stack), ResourcePackStackType::Global);
    if (gLog) gLog->info("pack menu: setStack returned {} ({} pack(s) chosen, {} resolved)", set, gActive.size(), count);
    logStacks(mgr, "after setStack");
    int way = 0;
    if (changed("1 setStack")) way = 1;
    if (!way) {
        mgr.mGameplayResourcesLoaded = false;
        mgr.mInitializing            = false;
        if (auto again = fresh()) mgr.setStack(std::move(again), ResourcePackStackType::Global);
        mgr.mGameplayResourcesLoaded = loaded;
        mgr.mInitializing            = initializing;
        if (changed("2 setStack, gameplay off")) way = 2;
    }
    if (!way) {
        mgr.mGameplayResourcesLoaded = false;
        mgr.mInitializing            = false;
        mgr.mPendingRestack          = false;
        mgr._composeFullStack();
        mgr.mGameplayResourcesLoaded = loaded;
        mgr.mInitializing            = initializing;
        if (changed("3 compose, gameplay off, not pending")) way = 3;
    }
    if (!way) {
        mgr.mGameplayResourcesLoaded = false;
        mgr.mInitializing            = false;
        mgr.mPendingRestack          = true;
        mgr._composeFullStack();
        mgr.mGameplayResourcesLoaded = loaded;
        mgr.mInitializing            = initializing;
        if (changed("4 compose, gameplay off, pending")) way = 4;
    }
    if (gLog) gLog->info("pack menu: served stack {}", way ? std::format("changed by way {}", way) : std::string("did not change"));
    try { // keep it for the next start, as the settings screen would
        std::ofstream out(gStackFile, std::ios::binary | std::ios::trunc);
        out << json;
    } catch (...) {
        if (gLog) gLog->warn("pack menu: could not save {}", gStackFile.string());
    }
    requestHotReload();
    return true;
}

// ---------------------------------------------------------------------------
// Forms
// ---------------------------------------------------------------------------
std::atomic<ClientNetworkHandler*> gHandler{nullptr}; // captured from packets it handles
// The server's NetworkIdentifier from the same packets, copied as bytes: its
// constructors are not exported, and the handler only passes it along.
alignas(NetworkIdentifier) unsigned char gSource[sizeof(NetworkIdentifier)] = {};
std::atomic<bool>                        gHaveSource{false};

void capture(ClientNetworkHandler* handler, NetworkIdentifier const& source) {
    gHandler.store(handler); // network packets are handled on the main thread, as forms are shown
    std::memcpy(gSource, &source, sizeof(gSource));
    gHaveSource.store(true);
}

constexpr uint kFormBase = 0x7E550000u;
uint           gNextFormId = kFormBase;
uint           gShownForm  = 0;                        // id of the form on screen, 0 = none
std::function<void()>               gPending;           // a form to show a few frames from now
int                                 gPendingDelay = 0;

std::string escape(std::string const& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out;
}

// What to do with the answer to the form on screen: the JSON answer, or null when
// the form was closed without submitting.
std::function<void(Json::Value const*)> gOnAnswer;

void show(std::string const& json, std::function<void(Json::Value const*)> onAnswer) {
    auto* handler = gHandler.load();
    auto  client  = ll::service::getClientInstance();
    if (!handler || !gHaveSource.load() || !client || !client->getLevel()) {
        chat("Tessera: the pack menu works inside a world");
        return;
    }
    gOnAnswer  = std::move(onAnswer);
    gShownForm = ++gNextFormId;
    ModalFormRequestPacket packet(gShownForm, json);
    handler->handle(*reinterpret_cast<NetworkIdentifier const*>(gSource), packet);
}

// One window: a switch per pack and, with more than one pack, an order slider
// (1 = top). Ties keep newly switched-on packs above the ones already on, as Java
// puts a newly enabled pack on top.
struct Row {
    Entry entry;
    bool  wasOn;
    int   shownAt; // its place in the window
};
std::vector<Row> gRows;

void showMain() {
    gRows.clear();
    for (auto const& e : gActive) gRows.push_back({e, true, int(gRows.size())});
    for (auto const& e : gInactive) gRows.push_back({e, false, int(gRows.size())});
    int const total = int(gRows.size());

    std::string json = std::format(
        R"({{"type":"custom_form","title":"{}","content":[{{"type":"label","text":"{}"}})", escape("Tessera 材質包"),
        escape("打開開關就會啟用。「順序」1 是最優先，數字相同時，剛打開的排在前面。\n"
               "按「送出」套用並重新載入，關閉視窗則不變更。")
    );
    for (size_t i = 0; i < gRows.size(); ++i) {
        auto const& r = gRows[i];
        json += std::format(
            R"(,{{"type":"toggle","text":"{}","default":{}}})", escape(std::format("§l{}§r", r.entry.name)),
            r.wasOn ? "true" : "false"
        );
        if (total > 1)
            json += std::format(
                R"(,{{"type":"slider","text":"{}","min":1,"max":{},"step":1,"default":{}}})", escape("順序"), total,
                r.wasOn ? int(i) + 1 : 1
            );
    }
    json += "]}";

    show(json, [total](Json::Value const* answer) {
        if (gLog) gLog->info("pack menu answer: {}", answer ? answer->toStyledString() : std::string("(closed)"));
        if (!answer) return; // closed: nothing changes
        struct Pick {
            Entry entry;
            int   order;
            bool  wasOn;
            int   shownAt;
        };
        std::vector<Pick>  on;
        std::vector<Entry> off;
        // The answer holds only nulls (labels), booleans (switches) and numbers
        // (sliders). Read them by kind, in order, so it does not matter whether the
        // label is counted.
        std::vector<bool>  switches;
        std::vector<int>   numbers;
        std::string const  text = answer->toStyledString();
        std::regex const   token(R"(true|false|-?\d+(?:\.\d+)?)");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), token); it != std::sregex_iterator(); ++it) {
            std::string const t = it->str();
            if (t == "true" || t == "false") switches.push_back(t == "true");
            else numbers.push_back(int(std::lround(std::stod(t))));
        }
        if (switches.size() != gRows.size()) {
            if (gLog) gLog->warn("pack menu: {} switch(es) in the answer for {} pack(s); ignored", switches.size(), gRows.size());
            chat("Tessera: could not read the menu answer");
            return;
        }
        for (size_t i = 0; i < gRows.size(); ++i) {
            bool const enabled = switches[i];
            int const  order   = i < numbers.size() ? numbers[i] : int(i) + 1;
            if (enabled) on.push_back({gRows[i].entry, order, gRows[i].wasOn, gRows[i].shownAt});
            else off.push_back(gRows[i].entry);
        }
        std::stable_sort(on.begin(), on.end(), [](Pick const& a, Pick const& b) {
            if (a.order != b.order) return a.order < b.order;
            if (a.wasOn != b.wasOn) return !a.wasOn; // newly enabled first
            return a.shownAt < b.shownAt;
        });
        gActive.clear();
        for (auto& p : on) gActive.push_back(std::move(p.entry));
        gInactive = std::move(off);
        std::vector<std::string> order;
        for (auto const& e : gActive) order.push_back(e.id);
        if (order == gOriginalOrder) {
            chat("Tessera: no changes");
            return;
        }
        if (auto client = ll::service::getClientInstance()) apply(client->getMinecraftGame());
    });
}

void open() {
    auto client = ll::service::getClientInstance();
    if (!client || !client->getLevel()) return;
    if (!loadPacks(client->getMinecraftGame())) {
        chat("Tessera: could not read the resource pack list");
        return;
    }
    if (gLog) gLog->info("pack menu: {} active, {} inactive pack(s)", gActive.size(), gInactive.size());
    showMain();
}

// The answer to one of our forms: run the chosen button. Returns true when the
// packet is ours (it must then not reach the server, which never asked).
bool consume(Packet& packet) {
    if (packet.getId() != MinecraftPacketIds::ModalFormResponse) return false;
    auto& response = static_cast<ModalFormResponsePacket&>(packet);
    uint const id  = response.mFormId;
    if (id <= kFormBase || id > gNextFormId) return false;
    if (id == gShownForm) {
        gShownForm   = 0;
        auto handler = std::move(gOnAnswer);
        gOnAnswer    = nullptr;
        if (handler) {
            Json::Value const* answer = nullptr;
            if (auto const& json = *response.mJSONResponse;
                json.has_value() && !json->toStyledString().starts_with("null"))
                answer = &*json;
            handler(answer);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
LL_TYPE_INSTANCE_HOOK(
    CaptureByCommands, ll::memory::HookPriority::Normal, ClientNetworkHandler,
    static_cast<void (ClientNetworkHandler::*)(NetworkIdentifier const&, AvailableCommandsPacket const&)>(
        &ClientNetworkHandler::$handle
    ),
    void, NetworkIdentifier const& source, AvailableCommandsPacket const& packet
) {
    capture(this, source);
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    CaptureByLevelEvent, ll::memory::HookPriority::Normal, ClientNetworkHandler,
    static_cast<void (ClientNetworkHandler::*)(NetworkIdentifier const&, LevelEventPacket const&)>(
        &ClientNetworkHandler::$handle
    ),
    void, NetworkIdentifier const& source, LevelEventPacket const& packet
) {
    capture(this, source);
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    CaptureByActorEvent, ll::memory::HookPriority::Normal, ClientNetworkHandler,
    static_cast<void (ClientNetworkHandler::*)(NetworkIdentifier const&, ActorEventPacket const&)>(
        &ClientNetworkHandler::$handle
    ),
    void, NetworkIdentifier const& source, ActorEventPacket const& packet
) {
    capture(this, source);
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    SendHook, ll::memory::HookPriority::Normal, LoopbackPacketSender, &LoopbackPacketSender::$send, void, Packet& packet
) {
    bool ours = false;
    try {
        ours = consume(packet);
    } catch (...) {}
    if (!ours) origin(packet);
}

LL_TYPE_INSTANCE_HOOK(
    SendToServerHook, ll::memory::HookPriority::Normal, LoopbackPacketSender, &LoopbackPacketSender::$sendToServer, void,
    Packet& packet
) {
    bool ours = false;
    try {
        ours = consume(packet);
    } catch (...) {}
    if (!ours) origin(packet);
}

bool gInstalled = false;

} // namespace

void logStacks(char const* when) {
    try {
        if (auto client = ll::service::getClientInstance()) logStacks(client->getMinecraftGame().getResourcePackManager(), when);
    } catch (...) {}
}

void frame() {
    if (gPending && --gPendingDelay <= 0) {
        auto form = std::move(gPending);
        gPending  = nullptr;
        try {
            form();
        } catch (...) {
            if (gLog) gLog->error("pack menu: showing a form threw");
        }
    }
#ifdef _WIN32
    static bool wasDown = false;
    bool        down    = false;
    DWORD       pid     = 0;
    if (HWND const fg = GetForegroundWindow(); fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId())
        down = (GetAsyncKeyState(VK_F3) & 0x8000) && (GetAsyncKeyState('P') & 0x8000);
    bool const pressed = down && !wasDown;
    wasDown            = down;
    if (pressed) {
        try {
            open();
        } catch (...) {
            if (gLog) gLog->error("pack menu: opening threw");
        }
    }
#endif
}

bool install(ll::io::Logger& logger) {
    if (gInstalled) return true;
    gLog         = &logger;
    int failures = 0;
    failures += CaptureByCommands::hook() != 0;
    failures += CaptureByLevelEvent::hook() != 0;
    failures += CaptureByActorEvent::hook() != 0;
    failures += SendHook::hook() != 0;
    failures += SendToServerHook::hook() != 0;
    if (failures) logger.error("pack menu: {} hook(s) failed to install", failures);
    gInstalled = true;
    return failures == 0;
}

void uninstall() {
    if (!gInstalled) return;
    SendToServerHook::unhook();
    SendHook::unhook();
    CaptureByActorEvent::unhook();
    CaptureByLevelEvent::unhook();
    CaptureByCommands::unhook();
    gHandler.store(nullptr);
    gInstalled = false;
}

} // namespace tessera::game::packmenu
