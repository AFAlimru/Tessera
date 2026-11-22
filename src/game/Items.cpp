#include "game/Items.h"
#include "game/Engine.h"
#include "game/JavaPacks.h"

#include "core/item/Cit.h"
#include "core/item/ItemDef.h"
#include "core/java/JavaPack.h"

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Signature.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/TessellatorQuadInfo.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/texture/TextureTessellator.h"
#include "mc/deps/minecraft_renderer/renderer/MeshData.h"

#include "mc/deps/core/math/Color.h"
#include "mc/world/item/ItemDescriptor.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/world/level/block/BlockShape.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/world/level/block/Block.h"
#include "mc/client/gui/controls/renderers/InventoryItemRenderer.h"
#include "mc/deps/core/utility/PropertyBag.h"
#include "mc/deps/json/Value.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/controllers/ContainerScreenController.h"
#include "mc/client/renderer/actor/ItemRenderChunkType.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ItemRenderer.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/file/Path.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/PlayerUIContainer.h"
#include "mc/resources/PackInstance.h"
#include "mc/resources/ResourcePack.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/world/item/BlockItem.h"
#include "mc/world/item/HumanoidArmorItem.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/item/ResolvedItemIconInfo.h"
#include "mc/world/item/enchanting/Enchant.h"
#include "mc/world/item/enchanting/EnchantUtils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <utility>
#include <cstring>
#include <array>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>


namespace tessera::game::items {
namespace {

ll::io::Logger* gLog = nullptr;

constexpr std::string_view kSpriteDir = "textures/tessera_java/";

struct Live {
    cit::Rule   rule;
    std::string sprite; // item atlas texture name
};
struct Snapshot {
    std::vector<Live>     rules; // weight first, then file order
    std::set<std::string> items;
    // Java 1.21.4+ item model definitions (items/<id>.json) and the sprite of each model set.
    std::unordered_map<std::string, itemdef::Definition> defs;
    std::map<itemdef::ModelSet, std::string>             defSprites;
    // Sprites drawn from models with elements (a picture of a 3D model).
    std::set<std::string>                                solidSprites;
    // Sprites with see-through pixels (stained glass panes): in a slot the game draws a
    // stand-in sprite solid, so blocks keep their own see-through icon.
    std::set<std::string>                                seeThroughSprites;
    // Those models as held-item meshes, by sprite.
    std::map<std::string, std::shared_ptr<java::HandMesh const>> handMeshes;
    // Items (Bedrock names, no namespace) Java draws flat in the hand: every model of their
    // definition is a flat item model.
    std::set<std::string> flatInJava;
};
std::mutex                      gMutex;
std::shared_ptr<Snapshot const> gSnap;

std::shared_ptr<Snapshot const> snapshot() {
    std::lock_guard lock(gMutex);
    return gSnap;
}

uint64_t fnv(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

// Java enchantment ids in Bedrock's Enchant::Type order.
constexpr char const* kEnchants[] = {
    "protection", "fire_protection", "feather_falling", "blast_protection", "projectile_protection", "thorns",
    "respiration", "depth_strider", "aqua_affinity", "sharpness", "smite", "bane_of_arthropods", "knockback",
    "fire_aspect", "looting", "efficiency", "silk_touch", "unbreaking", "fortune", "power", "punch", "flame",
    "infinity", "luck_of_the_sea", "lure", "frost_walker", "mending", "binding_curse", "vanishing_curse", "impaling",
    "riptide", "loyalty", "channeling", "multishot", "piercing", "quick_charge", "soul_speed", "swift_sneak",
    "wind_burst", "density", "breach", "lunge",
};

// Java potion ids by Bedrock's potion aux value.
constexpr char const* kPotions[] = {
    "water", "mundane", "mundane", "thick", "awkward", "night_vision", "long_night_vision", "invisibility",
    "long_invisibility", "leaping", "long_leaping", "strong_leaping", "fire_resistance", "long_fire_resistance",
    "swiftness", "long_swiftness", "strong_swiftness", "slowness", "long_slowness", "water_breathing",
    "long_water_breathing", "healing", "strong_healing", "harming", "strong_harming", "poison", "long_poison",
    "strong_poison", "regeneration", "long_regeneration", "strong_regeneration", "strength", "long_strength",
    "strong_strength", "weakness", "long_weakness", "wither", "turtle_master", "long_turtle_master",
    "strong_turtle_master", "slow_falling", "long_slow_falling", "strong_slowness", "wind_charged", "weaving",
    "oozing", "infested",
};
// Java effect ids by Bedrock's suspicious stew aux value.
constexpr char const* kStewEffects[] = {
    "night_vision", "jump_boost", "weakness", "blindness", "poison", "saturation", "saturation",
    "fire_resistance", "regeneration", "wither", "night_vision", "blindness", "nausea",
};

int enchantIndex(std::string const& id) {
    for (int i = 0; i < int(std::size(kEnchants)); ++i)
        if (id == kEnchants[i]) return i;
    return -1;
}

// Diagnostics: which icon calls arrive, flushed per line (data/cit_diag.log).
std::mutex            gDiagMutex;
std::filesystem::path gDiagPath;
std::ofstream         gDiagFile;
std::atomic<int>      gDiagBudget{600};

std::set<std::string> gDiagSeen;

void diag(std::string const& line) {
    std::lock_guard lock(gDiagMutex);
    if (!gDiagSeen.insert(line).second) return; // each distinct line once
    if (gDiagBudget.fetch_sub(1) <= 0) return;
    if (!gDiagFile.is_open()) {
        if (gDiagPath.empty()) return;
        gDiagFile.open(gDiagPath, std::ios::out | std::ios::trunc);
    }
    gDiagFile << line << std::endl;
    gDiagFile.flush();
}

// The CIT sprite (item atlas texture name) for a stack, or empty when no rule matches.
// nativeBlock: the game draws this item as a block; pictures of 3D models are then left out
// (the block's own shape, with its item-form rules, is the real thing).
std::string matchSprite(ItemStackBase const& stack, std::string* facts = nullptr, bool gui = true, bool nativeBlock = false) {
    auto const snap = snapshot();
    if (!snap || !stack.mItem) return {};
    std::string id = stack.getTypeName();
    if (id.starts_with("minecraft:")) id = id.substr(10);
    if (!snap->items.contains(id)) return {};
    cit::Facts f;
    f.id        = id;
    f.damage    = stack.getDamageValue();
    f.maxDamage = stack.mItem->getMaxDamage();
    f.count     = stack.mCount;
    if (auto name = stack.getCustomName(); !name.empty()) f.name = std::move(name);
    if (facts)
        *facts = std::format("{} name {} damage {}/{} count {}", id, f.name ? "\"" + *f.name + "\"" : std::string("-"), f.damage,
                             f.maxDamage, f.count);
    f.enchantLevel = [&](std::string const& java) {
        int const i = enchantIndex(java);
        return i < 0 ? 0 : EnchantUtils::getEnchantLevel(Enchant::Type(i), stack);
    };
    int const aux = stack.getAuxValue();
    if (id == "potion" || id == "splash_potion" || id == "lingering_potion")
        f.potion = [aux]() -> std::optional<std::string> {
            if (aux < 0 || aux >= int(std::size(kPotions))) return std::nullopt;
            return std::string(kPotions[aux]);
        };
    else if (id == "suspicious_stew" && aux >= 0 && aux < int(std::size(kStewEffects))) f.stewEffect = kStewEffects[aux];
    for (auto const& live : snap->rules)
        if (cit::matches(live.rule, f)) return live.sprite;
    // No CIT rule: the item's 1.21.4 definition, if the pack has one.
    if (auto d = snap->defs.find(id); d != snap->defs.end()) {
        itemdef::Context ctx;
        ctx.facts          = &f;
        ctx.displayContext = gui ? "gui" : "firstperson_righthand";
        ctx.maxStack       = std::max(1, int(stack.getMaxStackSize()));
        ctx.anyEnchantment = stack.isEnchanted();
        if (auto set = itemdef::pick(d->second, ctx))
            if (auto sp = snap->defSprites.find(*set); sp != snap->defSprites.end())
                if (!(nativeBlock && snap->solidSprites.contains(sp->second))) return sp->second;
    }
    return {};
}

std::string standInSprite(ItemStackBase const& s);
// Items whose own icon is not a plain sprite (compass, clock: frames of their own atlas).
std::mutex       gAtlasIconMutex;
std::set<short>  gAtlasIconItems;
bool atlasIconItem(short id) {
    std::lock_guard lock(gAtlasIconMutex);
    return gAtlasIconItems.contains(id);
}
// Some classes (armour) hand back their icon already looked up: its place in the atlas and its
// size. With another sprite named, those are dropped so it is looked up by name (left as they
// were, a renderer reading them draws the new sprite at the old one's size).
void clearResolved(ResolvedItemIconInfo& icon) {
    icon.mU0 = icon.mV0 = icon.mU1 = icon.mV1 = 0.f;
    icon.mTexSizeW = icon.mTexSizeH = 0;
}

// Inside ItemRenderer::renderGuiItemNew (the stack on the mouse cursor, item pop-ups): it
// draws a block item shown flat (rails, flowers) from the block atlas whatever atlas the icon
// names, so a Tessera icon (in the item atlas) would come out as a patch of block textures.
// There the block keeps its own icon.
thread_local int tGuiItemNew = 0;

void apply(Item const&, ItemStackBase const& stack, ResolvedItemIconInfo& icon, char const* via, int frame, bool pane) {
    if (auto forced = standInSprite(stack); !forced.empty()) {
        icon.mIconName  = forced;
        icon.mIconFrame = 0;
        icon.mIconType  = ItemIconInfoType::Icon;
        clearResolved(icon);
        return;
    }
    if (tGuiItemNew && stack.mBlock) return;
    if (icon.mIconType != ItemIconInfoType::Icon && stack.mItem) {
        std::lock_guard lock(gAtlasIconMutex);
        gAtlasIconItems.insert(stack.getId());
    }
    std::string facts;
    auto const  sprite = matchSprite(stack, &facts, pane);
    if (facts.empty()) return;
    diag(std::format("{} via {} frame {} pane {} icon {} -> {}", facts, via, frame, pane, std::string(icon.mIconName),
                     sprite.empty() ? "-" : sprite));
    if (sprite.empty()) return;
    icon.mIconName  = sprite;
    icon.mIconFrame = 0;
    icon.mIconType  = ItemIconInfoType::Icon;
    clearResolved(icon);
}

// A slot renderer keeps a bare copy of its item (id and aux only: no name, no enchantments),
// so CIT rules cannot see what is really in the slot. For the player's own slots the real
// stack is copied in, by the slot's collection name and index.
ItemStack const* playerSlot(std::string const& collection, int index) {
    auto client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player || (index < 0 && !collection.empty())) return nullptr;
    if (collection == "hotbar_items" && index < 9) return &player->getInventory().getItem(index);
    if (collection == "inventory_items" && index < 27) return &player->getInventory().getItem(index + 9);
    if (collection == "offhand_items" && index == 0) return &player->getOffhandSlot();
    if (collection.empty()) return &player->mPlayerUIContainer.get().getItem(0); // the stack on the cursor
    return nullptr;
}

// Other screens' slots (chests, furnaces...): the stacks their controller hands out, by
// collection name and index, kept only for items some CIT rule names.
std::mutex                                 gSlotMutex;
std::unordered_map<std::string, ItemStack> gSlotStacks;

std::string slotKey(std::string const& collection, int index) { return collection + "#" + std::to_string(index); }

void recordSlot(std::string const& collection, int index, ItemStackBase const& stack) {
    auto const snap = snapshot();
    if (!snap) return;
    bool wanted = false;
    if (!stack.isNull() && stack.mItem) {
        std::string id = stack.getTypeName();
        if (id.starts_with("minecraft:")) id = id.substr(10);
        wanted = snap->items.contains(id);
    }
    std::lock_guard lock(gSlotMutex);
    auto const      key = slotKey(collection, index);
    if (wanted) static_cast<ItemStackBase&>(gSlotStacks[key]) = stack;
    else gSlotStacks.erase(key);
}

LL_TYPE_INSTANCE_HOOK(
    ContainerStackHook, ll::memory::HookPriority::Normal, ContainerScreenController,
    &ContainerScreenController::_getVisualItemStack, ItemStackBase const&, std::string const& collectionName,
    int collectionIndex
) {
    auto const& stack = origin(collectionName, collectionIndex);
    try {
        recordSlot(collectionName, collectionIndex, stack);
    } catch (...) {}
    return stack;
}

// The real stack behind a slot (player slots directly, other screens as recorded).
ItemStack const* slotStack(PropertyBag const& bagIn, std::optional<ItemStack>& holder) {
    auto const& bag   = bagIn.mJsonValue.get();
    auto const  name  = bag["#collection_name"].asString("");
    int const   index = bag["#collection_index"].asInt(-1);
    int const idAux = bag["#item_id_aux"].asInt(-1);
    auto      same  = [&](ItemStack const* p) { return p && !p->isNull() && (idAux >> 16) == int(p->getId()); };
    if (ItemStack const* p = playerSlot(name, index)) return same(p) ? p : nullptr;
    std::lock_guard lock(gSlotMutex);
    auto            it = gSlotStacks.find(slotKey(name, index));
    if (it == gSlotStacks.end()) return nullptr;
    holder = it->second;
    return same(&*holder) ? &*holder : nullptr;
}

// Block items draw their slot icon as a block model and never ask getIconInfo, so a matched
// one is shown as a stand-in flat item (a diamond sword: its icon call is hooked) whose icon
// call answers with the CIT sprite. Entries expire when their slot stops updating.
struct StandIn {
    std::string                           sprite;
    std::chrono::steady_clock::time_point seen;
};
std::mutex                                         gStandInMutex;
std::unordered_map<ItemStackBase const*, StandIn> gStandIns;

int standInIdAux() {
    static int const v = [] {
        ItemStack s;
        s.reinit("minecraft:diamond_sword", 1, 0);
        return s.isNull() ? 0 : int(s.getId()) << 16;
    }();
    return v;
}
bool isStandIn(ItemStackBase const& s) {
    std::lock_guard lock(gStandInMutex);
    return gStandIns.contains(&s);
}
void markStandIn(ItemStackBase const& s, std::string const& sprite) {
    std::lock_guard lock(gStandInMutex);
    gStandIns[&s] = {sprite, std::chrono::steady_clock::now()};
}
void forgetStandIn(ItemStackBase const& s) {
    std::lock_guard lock(gStandInMutex);
    gStandIns.erase(&s);
}
std::string standInSprite(ItemStackBase const& s) {
    std::lock_guard lock(gStandInMutex);
    if (gStandIns.empty()) return {};
    auto it = gStandIns.find(&s);
    if (it == gStandIns.end()) return {};
    if (std::chrono::steady_clock::now() - it->second.seen > std::chrono::seconds(2)) {
        gStandIns.erase(it);
        return {};
    }
    return it->second.sprite;
}

std::string blockItemSprite(InventoryItemRenderer::UpdateData const& data) {
    if (!data.mOwnerPropertyBag || !snapshot()) return {};
    std::optional<ItemStack> holder;
    ItemStack const*         real = slotStack(*data.mOwnerPropertyBag, holder);
    if (!real || real->isNull()) return {};
    // Render type 0: a real block mesh. 1: a block shown as a flat icon. 2: an item icon.
    bool const drawnAsBlock = real->mBlock && int(InventoryItemRenderer::getRenderTypeFromItem(*real)) == 0;
    auto       sprite       = matchSprite(*real, nullptr, true, drawnAsBlock);
    if (sprite.empty()) return {};
    if (real->mBlock && snapshot()->seeThroughSprites.contains(sprite)) return {};
    // Plain flat items draw the sprite themselves; blocks and items with their own icon atlas
    // (compass, clock...) need the stand-in.
    if (real->mBlock || atlasIconItem(real->getId()) || int(InventoryItemRenderer::getRenderTypeFromItem(*real)) != 2)
        return sprite;
    return {};
}

thread_local int tSlotUpdate = 0; // inside InventoryItemRenderer::_update

// A block the game shows as a flat icon (lantern, amethyst cluster...) is drawn as a real
// block mesh instead when a pack gives it a 3D item model: the mesh is that model.
LL_STATIC_HOOK(
    RenderTypeHook, ll::memory::HookPriority::Normal, &InventoryItemRenderer::getRenderTypeFromItem, ItemRenderChunkType,
    ItemStack const& item
) {
    auto const type = origin(item);
    try {
        // As the heavy core does: drawn as a block mesh in the hand and in slots alike, which
        // then takes the pack's item shape.
        if (int(type) == 1 && item.mBlock && blockHasItemShape(item.mBlock->getTypeName()))
            return ItemRenderChunkType(0);
    } catch (...) {}
    return type;
}

// In a slot, a block whose shape the slot cannot draw (a lantern's, a torch's) is skipped
// even as a block mesh. While such an item is drawn, the game is handed a copy of its block
// graphics shaped as a plain block (the shared one is untouched: chunks build meanwhile), so
// it goes through renderGuiBlock like the heavy core does, where the item shape replaces it.
thread_local Block const* tSlotBlock = nullptr;
alignas(BlockGraphics) thread_local unsigned char tGraphicsCopy[sizeof(BlockGraphics)];

LL_TYPE_INSTANCE_HOOK(
    SlotItemHook, ll::memory::HookPriority::Normal, ItemRenderer, &ItemRenderer::renderGuiItemInChunk, void,
    BaseActorRenderContext& renderContext, ItemRenderChunkType type, ItemStack const& item, float x, float y,
    float lightMultiplier, float alphaMultiplier, float scale, int frame, bool allowAnimation, int zOrder,
    std::optional<TextureUVCoordinateSet> const& customIconUVCoords
) {
    Block const* const outer = tSlotBlock;
    try {
        tSlotBlock = int(type) == 0 && item.mBlock && blockHasItemShape(item.mBlock->getTypeName()) ? static_cast<Block const*>(item.mBlock) : nullptr;
    } catch (...) {
        tSlotBlock = nullptr;
    }
    origin(renderContext, type, item, x, y, lightMultiplier, alphaMultiplier, scale, frame, allowAnimation, zOrder, customIconUVCoords);
    tSlotBlock = outer;
}

thread_local ItemStack const* tHandStack = nullptr; // inside ItemInHandRenderer::renderItem

bool heldFlat(ItemStack const& stack, Block const& block) {
    try {
        if (!stack.mBlock || &stack.mBlock->getBlockType() != &block.getBlockType()) return false;
        auto const snap = snapshot();
        if (!snap || snap->flatInJava.empty()) return false;
        std::string id = stack.getTypeName();
        if (id.starts_with("minecraft:")) id = id.substr(10);
        return snap->flatInJava.contains(id) && blockHasRules(block.getTypeName()) && !blockHasItemShape(block.getTypeName());
    } catch (...) {
        return false;
    }
}

bool heldShaped(ItemStack const& stack, Block const& block) {
    try {
        return stack.mBlock && &stack.mBlock->getBlockType() == &block.getBlockType() && blockHasItemShape(block.getTypeName());
    } catch (...) {
        return false;
    }
}

LL_STATIC_HOOK(
    GraphicsForBlockHook, ll::memory::HookPriority::Normal, &BlockGraphics::getForBlock, BlockGraphics const*, Block const& block
) {
    BlockGraphics const* graphics = origin(block);
    // Held: a block with a 3D item shape the game would hold by its own odd shape (a lantern's:
    // scaled up and low, mostly off screen) is held as a plain block, like a block in a slot;
    // the item shape replaces the cube.
    if (tHandStack && graphics && !tSlotBlock && graphics->mBlockShape != BlockShape::Block && heldShaped(*tHandStack, block)) {
        std::memcpy(tGraphicsCopy, static_cast<void const*>(graphics), sizeof(BlockGraphics));
        auto* copy        = reinterpret_cast<BlockGraphics*>(tGraphicsCopy);
        copy->mBlockShape = BlockShape::Block;
        return copy;
    }
    // Held: a block a pack reshapes whose item Java holds as a flat picture (a chain) is held
    // flat too, as the game holds a cross-shaped block (a flower): from its icon.
    if (tHandStack && graphics && !tSlotBlock && graphics->mBlockShape != BlockShape::CrossTexture && heldFlat(*tHandStack, block)) {
        std::memcpy(tGraphicsCopy, static_cast<void const*>(graphics), sizeof(BlockGraphics));
        auto* copy        = reinterpret_cast<BlockGraphics*>(tGraphicsCopy);
        copy->mBlockShape = BlockShape::CrossTexture;
        return copy;
    }
    if (!tSlotBlock || !graphics || &block != tSlotBlock || graphics->mBlockShape == BlockShape::Block) return graphics;
    std::memcpy(tGraphicsCopy, static_cast<void const*>(graphics), sizeof(BlockGraphics));
    auto* copy        = reinterpret_cast<BlockGraphics*>(tGraphicsCopy);
    copy->mBlockShape = BlockShape::Block;
    return copy;
}

// The item atlas texture, as a slot last drew a Tessera icon from it (render thread only).
// Icons Bedrock draws with its own colouring (leather armour, firework stars...) multiply the
// item's colour into the icon. A Tessera icon is already the finished picture (Java's tints
// baked in): it is drawn as it is.
LL_TYPE_INSTANCE_HOOK(
    IconBlitHook, ll::memory::HookPriority::Normal, ItemRenderer, &ItemRenderer::iconBlit, void,
    BaseActorRenderContext& renderContext, mce::TexturePtr const& texture, float x, float y, float z,
    TextureUVCoordinateSet const& coords, float w, float h, float lightMultiplier, float alphaMultiplier, int colorMultiplier,
    int secondaryColorMultiplier, float xscale, float yscale, IconBlitGlint const glint, bool const multiColor
) {
    bool ours = false;
    try {
        ours = coords.sourceFileLocation->mPath->value.find("tessera_cit_") != std::string::npos;
    } catch (...) {}
    // The pick-up "pop" (scale above 1) can stay on for a Tessera icon in the creative grid,
    // leaving a full-size picture (leather armour) spilling over its neighbours: drawn at 1.
    if (ours) xscale = std::min(xscale, 1.f), yscale = std::min(yscale, 1.f);
    if (ours)
        return origin(renderContext, texture, x, y, z, coords, w, h, lightMultiplier, alphaMultiplier, -1, -1, xscale, yscale, glint,
                      false);
    origin(renderContext, texture, x, y, z, coords, w, h, lightMultiplier, alphaMultiplier, colorMultiplier, secondaryColorMultiplier,
           xscale, yscale, glint, multiColor);
}

void realStack(InventoryItemRenderer& renderer, InventoryItemRenderer::UpdateData const& data) {
    if (!data.mOwnerPropertyBag) return;
    auto const snap = snapshot();
    if (!snap) return;
    ItemStack& shown = renderer.mItemInstance.get();
    if (shown.isNull() || !shown.mItem) return;
    std::string id = shown.getTypeName();
    if (id.starts_with("minecraft:")) id = id.substr(10);
    if (!snap->items.contains(id)) return;
    std::optional<ItemStack> recorded;
    ItemStack const*         real = slotStack(*data.mOwnerPropertyBag, recorded);
    if (!real || real->isNull() || real->getId() != shown.getId()) return;
    if (!real->hasSameUserData(shown) || real->mCount != shown.mCount) shown = *real;
}

LL_TYPE_INSTANCE_HOOK(
    InvUpdateHook, ll::memory::HookPriority::Normal, InventoryItemRenderer, &InventoryItemRenderer::_update, bool,
    InventoryItemRenderer::UpdateData const& data
) {
    struct InSlot {
        InSlot() { ++tSlotUpdate; }
        ~InSlot() { --tSlotUpdate; }
    } inSlot;
    // Matched block item: the slot is built as a flat stand-in item from the start (its bag
    // briefly names the stand-in), so the renderer sets up a flat item-atlas icon.
    std::string sprite;
    PropertyBag* bag = nullptr;
    std::optional<Json::Value> savedIdAux;
    try {
        sprite = blockItemSprite(data);
        bool const was = isStandIn(this->mItemInstance.get());
        if (!sprite.empty() && standInIdAux() != 0) {
            bag = const_cast<PropertyBag*>(data.mOwnerPropertyBag);
            auto& json = bag->mJsonValue.get();
            savedIdAux = json["#item_id_aux"];
            json["#item_id_aux"] = Json::Value(standInIdAux());
            if (!was) this->mCachedProperties.get().mCacheVersion = -0x7151;
        } else if (was) {
            forgetStandIn(this->mItemInstance.get());
            this->mCachedProperties.get().mCacheVersion = -0x7151;
        }
    } catch (...) {
        sprite.clear();
    }
    auto r = origin(data);
    try {
        if (bag && savedIdAux) {
            bag->mJsonValue.get()["#item_id_aux"] = *savedIdAux;
            markStandIn(this->mItemInstance.get(), sprite);
        } else realStack(*this, data);
    } catch (...) {}
    return r;
}

LL_TYPE_INSTANCE_HOOK(
    GuiItemNewHook, ll::memory::HookPriority::Normal, ItemRenderer, &ItemRenderer::renderGuiItemNew, void,
    BaseActorRenderContext& renderContext, ItemStack const& item, int frame, float x, float y, bool renderEnchantmentFoil,
    float transparency, float lightMultiplier, float scale, int zOrder
) {
    ++tGuiItemNew;
    try {
        origin(renderContext, item, frame, x, y, renderEnchantmentFoil, transparency, lightMultiplier, scale, zOrder);
    } catch (...) {
        --tGuiItemNew;
        throw;
    }
    --tGuiItemNew;
}

LL_TYPE_INSTANCE_HOOK(
    ItemIconHook, ll::memory::HookPriority::Normal, Item, &Item::$getIconInfo, ResolvedItemIconInfo,
    ItemStackBase const& stack, int frame, bool inInventoryPane
) {
    auto icon = origin(stack, frame, inInventoryPane);
    try {
        apply(*this, stack, icon, "Item", frame, inInventoryPane);
    } catch (...) {}
    return icon;
}

LL_TYPE_INSTANCE_HOOK(
    BlockItemIconHook, ll::memory::HookPriority::Normal, BlockItem, &BlockItem::$getIconInfo, ResolvedItemIconInfo,
    ItemStackBase const& stack, int frame, bool inInventoryPane
) {
    auto icon = origin(stack, frame, inInventoryPane);
    try {
        apply(*this, stack, icon, "BlockItem", frame, inInventoryPane);
    } catch (...) {}
    return icon;
}

LL_TYPE_INSTANCE_HOOK(
    ArmorIconHook, ll::memory::HookPriority::Normal, HumanoidArmorItem, &HumanoidArmorItem::$getIconInfo,
    ResolvedItemIconInfo, ItemStackBase const& stack, int frame, bool inInventoryPane
) {
    auto icon = origin(stack, frame, inInventoryPane);
    try {
        apply(*this, stack, icon, "Armor", frame, inInventoryPane);
    } catch (...) {}
    return icon;
}

// Item classes that override getIconInfo with an unexported function (potions, bows,
// compasses, data-driven items...): their override is found in the item's vtable and hooked
// directly. The slot index comes from the game's own virtual call (offset 0x3a8).
constexpr size_t kIconSlot = 0x3a8 / sizeof(void*);
constexpr int    kThunks   = 48;
void*            gIconOrigins[kThunks]{};
void*            gIconTargets[kThunks]{};
int              gIconUsed = 0;
std::mutex       gIconMutex;
std::set<void*>  gIconCovered; // getIconInfo implementations already hooked

struct IconThunk {
    using Fn = ResolvedItemIconInfo (IconThunk::*)(ItemStackBase const&, int, bool) const;
    template <int N>
    ResolvedItemIconInfo call(ItemStackBase const& stack, int frame, bool pane) const {
        Fn   fn;
        void* o = gIconOrigins[N];
        static_assert(sizeof(Fn) == sizeof(void*));
        std::memcpy(&fn, &o, sizeof(o));
        auto icon = (this->*fn)(stack, frame, pane);
        try {
            apply(*reinterpret_cast<Item const*>(this), stack, icon, "vtable", frame, pane);
        } catch (...) {}
        return icon;
    }
};

template <int... N>
std::array<void*, sizeof...(N)> makeThunks(std::integer_sequence<int, N...>) {
    std::array<void*, sizeof...(N)> out{};
    int                             i = 0;
    ((void)[&] {
        auto  fn = &IconThunk::call<N>;
        void* p;
        std::memcpy(&p, &fn, sizeof(p));
        out[size_t(i++)] = p;
    }(), ...);
    return out;
}
std::array<void*, kThunks> const& thunks() {
    static auto const t = makeThunks(std::make_integer_sequence<int, kThunks>{});
    return t;
}

// The function an import thunk (jmp [rip+0]; dq target) leads to.
void* follow(void* p) {
    auto const* b = static_cast<unsigned char const*>(p);
    if (b && b[0] == 0xFF && b[1] == 0x25 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0) {
        void* t;
        std::memcpy(&t, b + 6, sizeof(t));
        return t;
    }
    return p;
}

void coverItem(Item const& item, std::string const& name) {
    void* const* vt = *reinterpret_cast<void* const* const*>(&item);
    void*        fn = vt[kIconSlot];
    std::lock_guard lock(gIconMutex);
    if (gIconCovered.empty()) { // the ones hooked by name
        gIconCovered.insert(follow(ll::memory::resolveIdentifier(&Item::$getIconInfo)));
        gIconCovered.insert(follow(ll::memory::resolveIdentifier(&BlockItem::$getIconInfo)));
        gIconCovered.insert(follow(ll::memory::resolveIdentifier(&HumanoidArmorItem::$getIconInfo)));
    }
    if (gIconCovered.contains(fn)) return;
    if (gIconUsed >= kThunks) {
        if (gLog) gLog->warn("CIT: no room to hook the icon function of {}", name);
        return;
    }
    int const n = gIconUsed;
    if (ll::memory::hook(fn, thunks()[size_t(n)], &gIconOrigins[n], ll::memory::HookPriority::Normal) != 0) {
        if (gLog) gLog->warn("CIT: hooking the icon function of {} failed", name);
        gIconCovered.insert(fn); // do not retry
        return;
    }
    gIconTargets[n] = fn;
    ++gIconUsed;
    gIconCovered.insert(fn);
    if (gLog) gLog->info("CIT: {} draws its icon with its own function; hooked it", name);
}

void uncoverAll() {
    std::lock_guard lock(gIconMutex);
    for (int i = 0; i < gIconUsed; ++i) ll::memory::unhook(gIconTargets[i], thunks()[size_t(i)]);
    gIconUsed = 0;
    gIconCovered.clear();
}

// The colour Bedrock multiplies into a held item (leather armour's dye, a firework star's
// colour): Item::getColor, unexported, overridden per class. A held item shown with a Tessera
// icon (a finished picture) is drawn uncoloured; worn armour keeps its colour (only while
// ItemInHandRenderer::renderItem runs).
constexpr size_t kColorSlot   = 73;
constexpr int    kColorThunks = 24;
void*            gColorOrigins[kColorThunks]{};
void*            gColorTargets[kColorThunks]{};
int              gColorUsed = 0;
std::set<void*>  gColorCovered;

struct ColorThunk {
    using Fn = mce::Color (ColorThunk::*)(CompoundTag const*, ItemDescriptor const&) const;
    template <int N>
    mce::Color call(CompoundTag const* userData, ItemDescriptor const& descriptor) const {
        Fn    fn;
        void* o = gColorOrigins[N];
        std::memcpy(&fn, &o, sizeof(o));
        try {
            if (tHandStack && !matchSprite(*tHandStack).empty()) return mce::Color(1.f, 1.f, 1.f, 1.f);
        } catch (...) {}
        return (this->*fn)(userData, descriptor);
    }
};
template <int... N>
std::array<void*, sizeof...(N)> makeColorThunks(std::integer_sequence<int, N...>) {
    std::array<void*, sizeof...(N)> out{};
    int                             i = 0;
    ((void)[&] {
        auto  fn = &ColorThunk::call<N>;
        void* p;
        std::memcpy(&p, &fn, sizeof(p));
        out[size_t(i++)] = p;
    }(), ...);
    return out;
}
std::array<void*, kColorThunks> const& colorThunks() {
    static auto const t = makeColorThunks(std::make_integer_sequence<int, kColorThunks>{});
    return t;
}
// The slot is checked once against the exported base implementation on a plain item.
bool colorSlotChecked() {
    static int const ok = [] {
        try {
            void* const base = follow(ll::memory::resolveIdentifier(&Item::$getColor));
            for (char const* name : {"minecraft:diamond", "minecraft:stick", "minecraft:iron_ingot", "minecraft:flint"}) {
                ItemStack probe;
                probe.reinit(name, 1, 0);
                if (probe.isNull() || !probe.mItem) continue;
                void* const* vt = *reinterpret_cast<void* const* const*>(probe.mItem.get());
                if (follow(vt[kColorSlot]) == base) return 1;
            }
        } catch (...) {}
        if (gLog) gLog->warn("CIT: Item::getColor slot not confirmed; held items keep the game's colouring");
        return 0;
    }();
    return ok != 0;
}

void coverColor(Item const& item) {
    if (!colorSlotChecked()) return;
    void* const* vt = *reinterpret_cast<void* const* const*>(&item);
    void*        fn = vt[kColorSlot];
    std::lock_guard lock(gIconMutex);
    if (gColorCovered.contains(fn) || gColorUsed >= kColorThunks) return;
    gColorCovered.insert(fn);
    int const n = gColorUsed;
    if (ll::memory::hook(fn, colorThunks()[size_t(n)], &gColorOrigins[n], ll::memory::HookPriority::Normal) != 0) return;
    gColorTargets[n] = fn;
    ++gColorUsed;
}
void uncoverColors() {
    std::lock_guard lock(gIconMutex);
    for (int i = 0; i < gColorUsed; ++i) ll::memory::unhook(gColorTargets[i], colorThunks()[size_t(i)]);
    gColorUsed = 0;
    gColorCovered.clear();
}

LL_TYPE_INSTANCE_HOOK(
    HandItemHook, ll::memory::HookPriority::Normal, ItemInHandRenderer, &ItemInHandRenderer::renderItem, void,
    BaseActorRenderContext& renderContext, Actor& entity, ItemStack const& item, bool posAndRotSetByJSON, ItemContextFlags itemFlags,
    bool useMatrixAsIs, bool renderingMainHand
) {
    ItemStack const* const outer = tHandStack;
    tHandStack                   = &item;
    origin(renderContext, entity, item, posAndRotSetByJSON, itemFlags, useMatrixAsIs, renderingMainHand);
    tHandStack = outer;
}

// Held items: the game builds an item's held mesh from its icon picture, a box per pixel
// (ItemInHandRenderer::_tessellateTextureItem -> TextureTessellator::tessellate, both unexported,
// found by their first 64 bytes in 1.26.60). For an icon drawn from a model with elements the
// boxes are swapped for the model itself, cut the same way (one colour per texel quad, the
// game's own normals and lighting), so it is held, dropped and framed as a real 3D model.
constexpr std::string_view kTexItemSig =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC D8 01 00 00 48 8D AC 24 80 00 00 00 44 0F 29 85 40 01 00 00 "
    "0F 29 BD 30 01 00 00 0F 29 B5 20 01 00 00 48 C7 85 18 01 00 00 FE FF FF FF 4C 89 CF 48";
constexpr std::string_view kTexTessSig =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 02 00 00 48 8D AC 24 80 00 00 00 44 0F 29 BD B0 01 00 00 "
    "44 0F 29 B5 A0 01 00 00 44 0F 29 AD 90 01 00 00 66 44 0F 7F A5 80 01 00 00 66 44 0F 7F";
using TexItemFn = void (*)(BaseActorRenderContext&, TextureTessellator&, Mob*, ItemStack const&, int, ushort&, ushort&);
using TexTessFn = void (*)(TextureTessellator*, Tessellator&, uchar const*, uchar const*, uchar const*, ushort, ushort, void const*);
TexItemFn gTexItemOrigin = nullptr;
TexTessFn gTexTessOrigin = nullptr;
void*     gTexItemTarget = nullptr;
void*     gTexTessTarget = nullptr;
thread_local java::HandMesh const* tHandMesh = nullptr; // the mesh for the item being built
thread_local bool                  tFirstPerson = false;

// The held mesh for a stack (render thread: reads the snapshot only).
std::shared_ptr<java::HandMesh const> handMeshFor(ItemStack const& item, std::shared_ptr<Snapshot const> const& snap) {
    if (!snap || snap->handMeshes.empty()) return nullptr;
    try {
        if (auto sprite = matchSprite(item, nullptr, false); !sprite.empty())
            if (auto it = snap->handMeshes.find(sprite); it != snap->handMeshes.end()) return it->second;
    } catch (...) {}
    return nullptr;
}

void texItemDetour(BaseActorRenderContext& ctx, TextureTessellator& tt, Mob* mob, ItemStack const& item, int frame, ushort& h, ushort& w) {
    auto const mesh = handMeshFor(item, snapshot());
    tHandMesh = mesh.get();
    gTexItemOrigin(ctx, tt, mob, item, frame, h, w);
    tHandMesh = nullptr;
}

void texTessDetour(TextureTessellator* self, Tessellator& tess, uchar const* color, uchar const* mers, uchar const* normals, ushort width,
                   ushort height, void const* params) {
    gTexTessOrigin(self, tess, color, mers, normals, width, height, params); // begins the tessellator (inlined)
    if (!tHandMesh) return;
    // Take back the boxes it just built (it started from an empty tessellator) and put the
    // model's quads in their place, in the same form.
    auto&        md    = tess.mMeshData.get();
    size_t const n     = md.mPositions->size();
    bool const   plain = tess.mTessellating && tess.mCount == n && md.mIndices->empty() && md.mColors->size() == n && md.mNormals->size() == n &&
                       md.mTextureUVs[0]->size() == n && md.mTextureUVs[1]->empty() && md.mTextureUVs[2]->empty() && md.mPBRTextureIndices->empty() &&
                       md.mMERS->empty() && md.mTangents->empty() && md.mBoneId0s->empty() && md.mGeoType->empty() && tess.mCurQuadVertex == 0;
    if (!plain) { // not the form expected: keep the game's
        static std::atomic<bool> told{false};
        if (!told.exchange(true) && gLog) gLog->warn("CIT: held-item mesh not in the expected form; 3D item models are held flat");
        return;
    }
    md.mPositions->clear();
    md.mColors->clear();
    md.mNormals->clear();
    md.mTextureUVs[0]->clear();
    tess.mQuadInfoList->clear();
    tess.mFaceCenterAccumulator.get() = Vec3{0.f, 0.f, 0.f};
    tess.mCount = 0;
    // The game's units are the icon's pixels; the mesh is made for 16.
    float const k = float(std::max(width, height)) / 16.f;
    for (auto const& q : tHandMesh->quads)
        for (size_t c = 0; c < 4; ++c) {
            tess.mNextColor.get()  = q.color;
            tess.mNextNormal.get() = glm::vec4(q.normal[0], q.normal[1], q.normal[2], 0.f);
            tess.mNextUV[0].get()  = glm::vec2(0.f, 0.f);
            tess.vertex(q.pos[c * 3] * k, q.pos[c * 3 + 1] * k, q.pos[c * 3 + 2] * k);
        }
}

// First person moves a held 3D model from where third person has it (HandMesh); told apart by
// ItemInHandRenderer::renderFirstPerson.
LL_TYPE_INSTANCE_HOOK(
    FirstPersonHook, ll::memory::HookPriority::Normal, ItemInHandRenderer, &ItemInHandRenderer::renderFirstPerson, void,
    BaseActorRenderContext& renderContext, Matrix const& prevProj, ItemContextFlags itemFlags
) {
    bool const outer = tFirstPerson;
    tFirstPerson     = true;
    origin(renderContext, prevProj, itemFlags);
    tFirstPerson = outer;
}

LL_TYPE_INSTANCE_HOOK(
    ItemPoseHook, ll::memory::HookPriority::Normal, ItemInHandRenderer, &ItemInHandRenderer::_applyDefaultItemTransforms, void,
    MatrixStack::MatrixStackRef& worldMatrix, ItemStack const& item, bool isInHandItem, BlockType const* blockType, BlockShape blockShape,
    ItemRenderCall const* renderObjectCall, float heldItemScale, bool posAndRotSetByJSON
) {
    origin(worldMatrix, item, isInHandItem, blockType, blockShape, renderObjectCall, heldItemScale, posAndRotSetByJSON);
    if (!isInHandItem) return;
    if (blockType) return;
    if (!tFirstPerson) return;
    auto const mesh = handMeshFor(item, snapshot());
    if (!mesh) return;
    // Here the mesh is still in the icon's own pixels; the call keeps 16 / icon size (the
    // scale _rebuildItem stores at +0x2a8 for a mesh built from a picture).
    float scale = 0.f;
    if (renderObjectCall) std::memcpy(&scale, reinterpret_cast<char const*>(renderObjectCall) + 0x2a8, sizeof(scale));
    if (!(scale > 1.f / 256 && scale <= 16.f)) return;
    glm::mat4 m;
    std::memcpy(&m, mesh->firstPerson.data(), sizeof(m));
    m[3] = glm::vec4(glm::vec3(m[3]) / scale, 1.f);
    Matrix* const wm = worldMatrix.mat;
    wm->_m.get() = wm->_m.get() * glm::mat4x4(m);
}

bool gInstalled = false;

} // namespace

std::shared_ptr<java::HandMesh const> itemModel(ItemStack const& stack) {
    try {
        return handMeshFor(stack, snapshot());
    } catch (...) {
        return nullptr;
    }
}

// One model of an item definition drawn as an icon: the pack's model, or for "item/x" with no
// model in the pack, the pack's texture item/x as a flat item (as Java's default models do).
std::optional<java::Image> definitionModelIcon(javapacks::ItemSession& session, std::string const& spec) {
    auto const sp = itemdef::splitSpec(spec);
    // Flat models as their layers, models with elements as Java's inventory view of them.
    if (session.info(sp.model).found) return session.icon(sp.model, sp.tints, false);
    std::string ns = "minecraft", path = sp.model;
    if (auto c = path.find(':'); c != std::string::npos) ns = path.substr(0, c), path = path.substr(c + 1);
    std::string const tex = "assets/" + ns + "/textures/" + path + ".png";
    if (!session.read(tex)) return std::nullopt;
    return session.texture(tex);
}

// A vanilla definition's outcome is drawn only when the pack changes how it looks: every
// model flat (or a 3D item model the pack itself ships), and at least one of them touched.
bool vanillaOutcomeTouched(javapacks::ItemSession& session, itemdef::ModelSet const& set) {
    bool touched = false;
    for (auto const& spec : set) {
        auto        model = itemdef::splitSpec(spec).model;
        auto const  info  = session.info(model);
        if (!info.found) return false;
        if (model.starts_with("minecraft:")) model = model.substr(10);
        if (!info.flat && !(info.ownModel && model.starts_with("item/"))) return false;
        touched |= info.touched;
    }
    return touched;
}

java::Image stackLayers(std::vector<java::Image> const& layers) {
    int size = 16;
    for (auto const& l : layers) size = std::max(size, l.width);
    java::Image out;
    out.width = out.height = size;
    out.pixels.assign(size_t(size) * size_t(size), 0u);
    for (auto const& l : layers) {
        int const sc = std::max(1, size / std::max(1, l.width));
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                int const sx = x / sc, sy = y / sc;
                if (sx >= l.width || sy >= l.height) continue;
                uint32_t const src = l.at(sx, sy);
                uint32_t const a   = src >> 24;
                if (a == 0) continue;
                uint32_t& dst = out.pixels[size_t(y) * size_t(size) + size_t(x)];
                if (a == 255 || (dst >> 24) == 0) dst = src;
                else {
                    auto mix = [&](int sh) {
                        uint32_t const sv = (src >> sh) & 0xFF, dv = (dst >> sh) & 0xFF;
                        return ((sv * a + dv * (255 - a)) / 255) << sh;
                    };
                    dst = mix(0) | mix(8) | mix(16) | (std::max(a, dst >> 24) << 24);
                }
            }
    }
    return out;
}

// A model set as one held-item mesh, when every model in it has elements (flat ones are left to
// the game, which builds them from the icon as it does for its own items).
std::shared_ptr<java::HandMesh const> handMeshOf(javapacks::ItemSession& session, itemdef::ModelSet const& set) {
    auto out = std::make_shared<java::HandMesh>();
    bool first = true;
    for (auto const& spec : set) {
        auto const sp = itemdef::splitSpec(spec);
        auto       m  = session.handMesh(sp.model, sp.tints);
        if (!m) return nullptr;
        if (first) *out = std::move(*m); // the first model's poses
        else {
            out->quads.insert(out->quads.end(), m->quads.begin(), m->quads.end());
            out->model.insert(out->model.end(), m->model.begin(), m->model.end());
        }
        first = false;
    }
    if (out->quads.empty()) return nullptr;
    return out;
}

void collectDefinitions(ResourcePackManager& packs, javapacks::ItemSession& session, Snapshot& snap, std::vector<ctm::Sprite>& sprites) {
    std::set<std::string> files;
    try {
        packs.iteratePacks([&](PackInstance const& instance) {
            ResourcePack const& pack = *instance.mPack;
            pack.forEachIn(
                Core::Path{std::string("assets/minecraft/items")},
                [&](Core::Path const& file) {
                    std::string path = file.value;
                    std::replace(path.begin(), path.end(), '\\', '/');
                    auto const at = path.find("items/");
                    if (at == std::string::npos || !path.ends_with(".json")) return;
                    files.insert("assets/minecraft/" + path.substr(at));
                },
                instance.mSubpackIndex, true
            );
        });
    } catch (...) {
        if (gLog) gLog->warn("item definitions: listing pack files failed");
    }
    int drawn = 0, used = 0, vanillaUsed = 0;
    std::map<std::string, std::optional<java::Image>> icons; // model spec -> icon (cached)
    auto add = [&](std::string const& path, std::string const& textOf, bool vanilla) {
        std::string const* text = &textOf;
        std::string error;
        auto        def = itemdef::parse(path, *text, error);
        if (!def) {
            if (gLog && !vanilla) gLog->warn("item definitions: {}: {}", path, error);
            return;
        }
        bool any = false;
        for (auto const& set : itemdef::outcomes(*def)) {
            if (vanilla && !vanillaOutcomeTouched(session, set)) continue;
            if (snap.defSprites.contains(set)) {
                any = true;
                continue;
            }
            std::vector<java::Image> layers;
            bool                     ok = true, solid = false;
            for (auto const& m : set) {
                auto const info = session.info(itemdef::splitSpec(m).model);
                solid |= info.found && !info.flat;
                auto it = icons.find(m);
                if (it == icons.end()) it = icons.emplace(m, definitionModelIcon(session, m)).first;
                if (!it->second) {
                    ok = false;
                    break;
                }
                layers.push_back(*it->second);
            }
            if (!ok) continue; // that outcome keeps the game's icon
            std::string key;
            for (auto const& m : set) key += m + "|";
            std::string const name = std::format("tessera_cit_d{:015x}", fnv(key) & 0xFFFFFFFFFFFFFFFull);
            java::Image const icon = stackLayers(layers);
            sprites.push_back({name, std::string(kSpriteDir) + name, java::encodePng(icon)});
            // Shader maps (labPBR) of a flat one-model icon, for shader mods and the game's
            // texture sets (as blocks have).
            bool const seeThrough = std::any_of(icon.pixels.begin(), icon.pixels.end(), [](uint32_t p) {
                uint32_t const a = p >> 24;
                return a > 0 && a < 255;
            });
            if (set.size() == 1 && !solid)
                if (auto pbr = session.flatPbr(itemdef::splitSpec(set.front()).model)) {
                    std::string const dir = std::string(kSpriteDir);
                    sprites.push_back({name + "_n", dir + name + "_n", java::encodePng(pbr->first)});
                    sprites.push_back({name + "_s", dir + name + "_s", java::encodePng(pbr->second)});
                    sprites.push_back({name + ".texture_set.json", dir + name + ".texture_set.json",
                                       std::format(R"({{"format_version":"1.16.100","minecraft:texture_set":{{"color":"{0}","normal":"{0}_normal","metalness_emissive_roughness":"{0}_mers"}}}})", name)});
                }
            snap.defSprites.emplace(set, name);
            if (solid) snap.solidSprites.insert(name);
            if (seeThrough) snap.seeThroughSprites.insert(name);
            if (auto mesh = handMeshOf(session, set)) snap.handMeshes.emplace(name, std::move(mesh));
            ++drawn;
            any = true;
        }
        if (!vanilla)
            for (auto const& w : def->warnings)
                if (gLog) gLog->warn("item definitions: {}: {}", path, w);
        if (!any) return; // nothing of it can be drawn from this pack stack
        snap.items.insert(def->item);
        std::string const item = def->item;
        snap.defs.emplace(item, std::move(*def));
        ++(vanilla ? vanillaUsed : used);
    };
    for (auto const& path : files)
        if (auto text = session.read(path)) add(path, *text, false);
    // Which items Java holds flat (by their definition, the pack's or Java's own).
    auto noteFlat = [&](std::string const& path, std::string const& text) {
        std::string error;
        auto        def = itemdef::parse(path, text, error);
        if (!def) return;
        for (auto const& set : itemdef::outcomes(*def))
            for (auto const& spec : set) {
                auto const info = session.info(itemdef::splitSpec(spec).model);
                if (!info.found || !info.flat) return;
            }
        std::string id = def->item;
        if (id.starts_with("minecraft:")) id = id.substr(10);
        snap.flatInJava.insert(id);
        // Its Bedrock name, where the block was renamed (Java chain -> Bedrock iron_chain).
        auto const& table = java::mappings();
        if (auto t = table.find(id); t != table.end() && !t->second.empty()) snap.flatInJava.insert(t->second.begin()->second.name);
    };
    for (auto const& path : files)
        if (auto text = session.read(path)) noteFlat(path, *text);
    // Java's own definitions, for items whose textures or item models the packs replace.
    size_t vanillaCount = 0;
    try {
        for (auto const& [path, text] : session.vanillaDefinitions()) {
            ++vanillaCount;
            if (!files.contains(path)) {
                add(path, text, true);
                noteFlat(path, text);
            }
        }
    } catch (...) {
        if (gLog) gLog->warn("item definitions: reading the Java client's definitions failed");
    }
    if (gLog && (!files.empty() || vanillaUsed))
        gLog->info("item definitions: {} of {} pack file(s) and {} of {} vanilla one(s) in use, {} icon(s)", used, files.size(),
                   vanillaUsed, vanillaCount, drawn);
}

std::vector<ctm::Sprite> collect(ResourcePackManager& packs) {
    std::set<std::string> files;
    try {
        packs.iteratePacks([&](PackInstance const& instance) {
            ResourcePack const& pack = *instance.mPack;
            for (std::string_view dir : {"assets/minecraft/optifine/cit", "assets/minecraft/mcpatcher/cit"})
                pack.forEachIn(
                    Core::Path{std::string(dir)},
                    [&](Core::Path const& file) {
                        std::string path = file.value;
                        std::replace(path.begin(), path.end(), '\\', '/');
                        auto const at = path.find(std::string(dir.substr(17)) + "/"); // "optifine/cit/"
                        if (at == std::string::npos || !path.ends_with(".properties")) return;
                        files.insert("assets/minecraft/" + path.substr(at));
                    },
                    instance.mSubpackIndex, true
                );
        });
    } catch (...) {
        if (gLog) gLog->warn("CIT: listing pack files failed");
    }

    auto                     snap = std::make_shared<Snapshot>();
    std::vector<ctm::Sprite> sprites;
    javapacks::ItemSession   session(packs);
    std::map<std::string, std::string> spriteOf; // icon source -> texture name
    int                      skipped = 0;
    for (auto const& path : files) {
        auto text = javapacks::readPackFile(packs, path);
        if (!text) continue;
        std::string error;
        auto        rule = cit::parse(path, *text, error);
        if (!rule) {
            ++skipped;
            if (gLog && skipped <= 10) gLog->warn("CIT: {}: {}", path, error);
            continue;
        }
        std::string const source = rule->model.empty() ? "tex:" + rule->texture : "model:" + rule->model;
        auto              found  = spriteOf.find(source);
        if (found == spriteOf.end()) {
            auto image = rule->model.empty() ? javapacks::loadPackTexture(packs, rule->texture)
                                             : session.icon(rule->model, {}, false);
            if (!image) {
                if (gLog) gLog->warn("CIT: {}: icon {} could not be drawn", path, rule->model.empty() ? rule->texture : rule->model);
                continue;
            }
            std::string const name = std::format("tessera_cit_{:016x}", fnv(source));
            sprites.push_back({name, std::string(kSpriteDir) + name, java::encodePng(*image)});
            if (!rule->model.empty())
                if (auto mesh = session.handMesh(rule->model, {}))
                    snap->handMeshes.emplace(name, std::make_shared<java::HandMesh const>(std::move(*mesh)));
            found = spriteOf.emplace(source, name).first;
        }
        for (auto const& w : rule->warnings)
            if (gLog) gLog->warn("CIT: {}: {}", path, w);
        for (auto const& item : rule->items) snap->items.insert(item);
        snap->rules.push_back({std::move(*rule), found->second});
    }
    collectDefinitions(packs, session, *snap, sprites);

    // Items whose class draws its icon with an unexported override get that override hooked.
    for (auto const& name : snap->items) {
        try {
            ItemStack probe;
            probe.reinit(name.find(':') == std::string::npos ? "minecraft:" + name : name, 1, 0);
            if (Item const* item = probe.isNull() ? nullptr : probe.mItem.get()) {
                coverItem(*item, name);
                coverColor(*item);
            }
        } catch (...) {}
    }
    std::stable_sort(snap->rules.begin(), snap->rules.end(),
                     [](Live const& a, Live const& b) { return a.rule.weight > b.rule.weight; });
    if (gLog && (!files.empty()))
        gLog->info("CIT: {} rule(s) for {} item(s), {} icon(s) from {} file(s), {} skipped", snap->rules.size(),
                   snap->items.size(), sprites.size(), files.size(), skipped);
    {
        std::lock_guard lock(gMutex);
        gSnap = snap->rules.empty() && snap->defs.empty() ? nullptr : std::shared_ptr<Snapshot const>(snap);
    }
    return sprites;
}

bool install(ll::io::Logger& logger, std::filesystem::path const& dataDir) {
    if (gInstalled) return true;
    gLog         = &logger;
    gDiagPath    = dataDir / "cit_diag.log";
    std::error_code ec;
    std::filesystem::create_directories(dataDir, ec);
    int failures = 0;
    std::string off; // test switch: data/cit_off.txt lists hooks to leave out
    {
        std::ifstream in(dataDir / "cit_off.txt");
        std::getline(in, off);
    }
    auto on = [&](char const* n) {
        bool const skip = off.find(n) != std::string::npos;
        if (skip) logger.warn("CIT: hook {} left out (cit_off.txt)", n);
        return !skip;
    };
    if (on("icon")) failures += ItemIconHook::hook() != 0;
    if (on("blockicon")) failures += BlockItemIconHook::hook() != 0;
    if (on("guinew")) failures += GuiItemNewHook::hook() != 0;
    if (on("armoricon")) failures += ArmorIconHook::hook() != 0;
    if (on("update")) failures += InvUpdateHook::hook() != 0;
    if (on("container")) failures += ContainerStackHook::hook() != 0;
    if (on("rendertype")) failures += RenderTypeHook::hook() != 0;
    if (on("iconblit")) failures += IconBlitHook::hook() != 0;
    if (on("handcolor")) failures += HandItemHook::hook() != 0;
    if (on("slotblock")) {
        failures += SlotItemHook::hook() != 0;
        failures += GraphicsForBlockHook::hook() != 0;
    }
    if (failures) logger.error("CIT: {} hook(s) failed to install", failures);
    if (on("handmodel")) {
        failures += FirstPersonHook::hook() != 0;
        failures += ItemPoseHook::hook() != 0;
        void* const item = ll::memory::SignatureView(ll::memory::Signature::parse(kTexItemSig)).resolve(true);
        void* const tess = ll::memory::SignatureView(ll::memory::Signature::parse(kTexTessSig)).resolve(true);
        if (!item || !tess) logger.warn("CIT: held-item mesh functions not found; 3D item models are held flat");
        else if (ll::memory::hook(item, reinterpret_cast<void*>(&texItemDetour), reinterpret_cast<void**>(&gTexItemOrigin), ll::memory::HookPriority::Normal) != 0)
            ++failures;
        else if (gTexItemTarget = item; ll::memory::hook(tess, reinterpret_cast<void*>(&texTessDetour), reinterpret_cast<void**>(&gTexTessOrigin), ll::memory::HookPriority::Normal) != 0)
            ++failures;
        else gTexTessTarget = tess;
    }

    gInstalled = true;
    return failures == 0;
}

void uninstall() {
    if (!gInstalled) return;
    uncoverAll();
    HandItemHook::unhook();
    ItemPoseHook::unhook();
    FirstPersonHook::unhook();
    if (gTexTessTarget) ll::memory::unhook(gTexTessTarget, reinterpret_cast<void*>(&texTessDetour));
    if (gTexItemTarget) ll::memory::unhook(gTexItemTarget, reinterpret_cast<void*>(&texItemDetour));
    gTexItemTarget = gTexTessTarget = nullptr;
    uncoverColors();
    IconBlitHook::unhook();
    GraphicsForBlockHook::unhook();
    SlotItemHook::unhook();
    RenderTypeHook::unhook();
    ContainerStackHook::unhook();
    InvUpdateHook::unhook();
    ArmorIconHook::unhook();
    BlockItemIconHook::unhook();
    GuiItemNewHook::unhook();
    ItemIconHook::unhook();
    gInstalled = false;
}

} // namespace tessera::game::items
