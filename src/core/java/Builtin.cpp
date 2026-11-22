// Stand-ins for Java vanilla files a pack relies on without shipping them. Written
// for Tessera from what the vanilla files do (shapes and which texture goes where),
// not copied from the game.
#include "core/java/JavaPack.h"

#include <format>
#include <map>
#include <string_view>

namespace tessera::java {
namespace {

std::string cube(std::string_view down, std::string_view up, std::string_view north, std::string_view south,
                 std::string_view west, std::string_view east, std::string_view particle) {
    return std::format(
        R"({{"parent":"block/cube","textures":{{"particle":"{}","down":"{}","up":"{}","north":"{}","south":"{}","west":"{}","east":"{}"}}}})",
        particle, down, up, north, south, west, east
    );
}

std::string box(double x0, double y0, double z0, double x1, double y1, double z1, std::string_view texture) {
    std::string faces;
    for (auto const* f : {"down", "up", "north", "south", "west", "east"})
        faces += std::format(R"({}"{}":{{"texture":"{}"}})", faces.empty() ? "" : ",", f, texture);
    return std::format(
        R"({{"parent":"block/block","elements":[{{"from":[{},{},{}],"to":[{},{},{}],"faces":{{{}}}}}]}})", x0, y0, z0, x1, y1,
        z1, faces
    );
}

std::map<std::string, std::string> const& templates() {
    static std::map<std::string, std::string> const t = [] {
        std::map<std::string, std::string> m;
        m["block/block"]      = "{}";
        // Item models: flat icons from layer0, layer1, ...
        m["item/generated"] = R"({"parent":"builtin/generated"})";
        m["item/handheld"]  = R"({"parent":"item/generated"})";
        m["item/handheld_rod"] = R"({"parent":"item/handheld"})";
        m["block/thin_block"] = R"({"parent":"block/block"})";
        m["block/cube"] =
            R"({"parent":"block/block","elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{)"
            R"("down":{"texture":"#down","cullface":"down"},"up":{"texture":"#up","cullface":"up"},)"
            R"("north":{"texture":"#north","cullface":"north"},"south":{"texture":"#south","cullface":"south"},)"
            R"("west":{"texture":"#west","cullface":"west"},"east":{"texture":"#east","cullface":"east"}}}]})";
        m["block/cube_all"]               = cube("#all", "#all", "#all", "#all", "#all", "#all", "#all");
        m["block/cube_mirrored_all"]      = cube("#all", "#all", "#all", "#all", "#all", "#all", "#all");
        m["block/cube_column"]            = cube("#end", "#end", "#side", "#side", "#side", "#side", "#side");
        m["block/cube_column_horizontal"] = cube("#side", "#side", "#end", "#end", "#side", "#side", "#side");
        m["block/cube_bottom_top"]        = cube("#bottom", "#top", "#side", "#side", "#side", "#side", "#side");
        m["block/cube_top"]               = cube("#side", "#top", "#side", "#side", "#side", "#side", "#side");
        m["block/orientable"]             = cube("#top", "#top", "#front", "#side", "#side", "#side", "#front");
        m["block/orientable_with_bottom"] = cube("#bottom", "#top", "#front", "#side", "#side", "#side", "#front");
        m["block/orientable_vertical"]    = cube("#side", "#front", "#side", "#side", "#side", "#side", "#side");
        // A button: 6x2x4 pixels on the floor, 1 pixel high while pressed.
        // Two crossed planes (flowers, saplings, amethyst buds...), as vanilla's block/cross.
        auto cross = [](char const* tex, char const* tint) {
            std::string const face = std::format(R"("uv":[0,0,16,16],"texture":"{}"{})", tex, tint);
            return std::format(
                R"({{"ambientocclusion":false,"textures":{{"particle":"{}"}},"elements":[)"
                R"({{"from":[0.8,0,8],"to":[15.2,16,8],"rotation":{{"origin":[8,8,8],"axis":"y","angle":45,"rescale":true}},"shade":false,"faces":{{"north":{{{}}},"south":{{{}}}}}}},)"
                R"({{"from":[8,0,0.8],"to":[8,16,15.2],"rotation":{{"origin":[8,8,8],"axis":"y","angle":45,"rescale":true}},"shade":false,"faces":{{"west":{{{}}},"east":{{{}}}}}}}]}})",
                tex, face, face, face, face);
        };
        m["block/cross"]        = cross("#cross", "");
        m["block/tinted_cross"] = cross("#cross", R"(,"tintindex":0)");
        m["block/button"]         = box(5, 0, 6, 11, 2, 10, "#texture");
        m["block/button_pressed"] = box(5, 0, 6, 11, 1, 10, "#texture");
        for (auto const* kind : {"beehive", "bee_nest"}) {
            bool const        hive   = std::string_view(kind) == "beehive";
            std::string const side   = std::format("block/{}_side", kind);
            std::string const top    = hive ? "block/beehive_end" : "block/bee_nest_top";
            std::string const bottom = hive ? "block/beehive_end" : "block/bee_nest_bottom";
            for (auto const* fill : {"empty", "honey"}) {
                std::string const front =
                    std::format("block/{}_front{}", kind, std::string_view(fill) == "honey" ? "_honey" : "");
                m[std::format("block/{}_{}", kind, fill)] = cube(bottom, top, front, side, side, side, side);
            }
        }
        return m;
    }();
    return t;
}

} // namespace

std::optional<std::string> builtinModel(std::string const& name) {
    if (auto it = templates().find(name); it != templates().end()) return it->second;
    // <material>_button[_pressed]: the button shape in the material's texture.
    std::string_view n = name;
    if (!n.starts_with("block/")) return std::nullopt;
    n.remove_prefix(6);
    bool const pressed = n.ends_with("_button_pressed");
    if (!pressed && !n.ends_with("_button")) return std::nullopt;
    std::string const material(n.substr(0, n.size() - (pressed ? 15 : 7)));
    if (material.empty()) return std::nullopt;
    bool const        stone   = material == "stone" || material == "polished_blackstone";
    std::string const texture = "block/" + material + (stone ? "" : "_planks");
    return std::format(R"({{"parent":"block/button{}","textures":{{"particle":"{}","texture":"{}"}}}})",
                       pressed ? "_pressed" : "", texture, texture);
}

std::vector<std::string> bedrockTextureNames(std::string const& javaTexture) {
    static std::map<std::string, std::string> const renamed = {
        {"note_block", "noteblock"},
        {"comparator", "comparator_off"},
        {"repeater", "repeater_off"},
        {"redstone_torch", "redstone_torch_on"},
        {"smooth_stone", "stone_slab_top"},
        {"smooth_stone_slab_side", "stone_slab_side"},
        {"redstone_lamp", "redstone_lamp_off"},
        {"dispenser_front", "dispenser_front_horizontal"},
        {"dropper_front", "dropper_front_horizontal"},
        {"piston_top", "piston_top_normal"},
        {"observer_back_on", "observer_back_lit"},
        {"beehive_end", "beehive_top"},
        {"oak_planks", "planks_oak"},
        {"spruce_planks", "planks_spruce"},
        {"birch_planks", "planks_birch"},
        {"jungle_planks", "planks_jungle"},
        {"acacia_planks", "planks_acacia"},
        {"dark_oak_planks", "planks_big_oak"},
        {"crimson_planks", "huge_fungus/crimson_planks"},
        {"warped_planks", "huge_fungus/warped_planks"},
        {"bamboo_stalk", "bamboo_stem"},
        {"oak_sapling", "sapling_oak"},
        {"spruce_sapling", "sapling_spruce"},
        {"birch_sapling", "sapling_birch"},
        {"jungle_sapling", "sapling_jungle"},
        {"acacia_sapling", "sapling_acacia"},
        {"dark_oak_sapling", "sapling_roofed_oak"},
        {"redstone_dust_dot", "redstone_dust_cross"},
        {"redstone_dust_line0", "redstone_dust_line"},
        {"redstone_dust_line1", "redstone_dust_line"},
        {"sunflower_bottom", "double_plant_sunflower_bottom"},
        {"sunflower_top", "double_plant_sunflower_top"},
        {"sunflower_front", "double_plant_sunflower_front"},
        {"sunflower_back", "double_plant_sunflower_back"},
        {"tripwire_hook", "trip_wire_source"},
        {"tripwire", "trip_wire"},
    };
    std::string base = javaTexture;
    if (auto slash = base.rfind('/'); slash != std::string::npos) base = base.substr(slash + 1);
    std::vector<std::string> out;
    if (auto it = renamed.find(base); it != renamed.end()) out.push_back(it->second);
    // Logs and stems: Java <wood>_log / <fungus>_stem.
    static std::map<std::string, std::vector<std::string>> const logs = {
        {"oak_log", {"log_oak"}},           {"spruce_log", {"log_spruce"}},       {"birch_log", {"log_birch"}},
        {"jungle_log", {"log_jungle"}},     {"acacia_log", {"log_acacia"}},       {"dark_oak_log", {"log_big_oak"}},
        {"mangrove_log", {"mangrove_log_side"}}, {"cherry_log", {"cherry_log_side"}}, {"pale_oak_log", {"pale_oak_log_side"}},
        {"crimson_stem", {"crimson_log_side"}},  {"warped_stem", {"warped_log_side"}},
    };
    if (auto it = logs.find(base); it != logs.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    // Stained glass: Java <colour>_stained_glass[_pane_top] -> Bedrock glass_<colour> / glass_pane_top_<colour>.
    for (std::string_view suffix : {std::string_view("_stained_glass"), std::string_view("_stained_glass_pane_top")}) {
        if (!std::string_view(base).ends_with(suffix)) continue;
        std::string colour = base.substr(0, base.size() - suffix.size());
        if (colour == "light_gray") colour = "silver";
        out.push_back((suffix == "_stained_glass" ? "glass_" : "glass_pane_top_") + colour);
    }
    out.push_back(base);
    return out;
}

} // namespace tessera::java
