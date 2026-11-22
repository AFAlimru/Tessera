// Tessera - Java resource pack baking (platform independent, no game headers).
//
// Reads a Java-layout pack (assets/minecraft/blockstates, models, textures), takes
// every Java block state from the Chunker Java->Bedrock table, renders each of the
// six faces of the state's models flat ("bakes" them) and produces Tessera rules in
// the state_textures.json format plus the images those rules point at.
// This is the C++ form of tools/convert_java_pack.py.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tessera::java {

struct Image {
    int                   width  = 0;
    int                   height = 0;
    std::vector<uint32_t> pixels; // RGBA, R in the low byte (little-endian bytes R,G,B,A)

    bool     empty() const { return width <= 0 || height <= 0; }
    uint32_t at(int x, int y) const { return pixels[size_t(y) * size_t(width) + size_t(x)]; }
    bool     anyOpaque() const;
};

// Decodes a PNG (any colour type) to RGBA. nullopt when it is not a readable image.
std::optional<Image> decodePng(std::string const& bytes);
// Encodes RGBA to PNG bytes.
std::string encodePng(Image const& image);
// Gives fully transparent pixels the colour of their nearest opaque neighbours (alpha stays 0),
// so mipmaps and filtering do not pull a stray colour (often white) into cut-out edges.
void bleedTransparent(Image& image, int passes = 16);

// Where pack files come from. Paths are relative to the pack root, "/"-separated,
// e.g. "assets/minecraft/models/block/lever.json".
class Assets {
public:
    virtual ~Assets() = default;
    virtual std::optional<std::string> read(std::string const& path) const = 0;
    // Every copy of the file, highest priority first (a stack of packs can hold several;
    // Java merges blockstate files across packs).
    virtual std::vector<std::string> readAll(std::string const& path) const {
        if (auto d = read(path)) return {*d};
        return {};
    }
    // Every file whose path starts with prefix.
    virtual std::vector<std::string> list(std::string const& prefix) const = 0;
};

struct BakeOptions {
    // The pack itself (always read first).
    Assets const* pack = nullptr;
    // Optional vanilla Java assets, read when the pack lacks a file. Blocks whose
    // vanilla models or textures the pack overrides are then baked as well.
    Assets const* vanilla = nullptr;
    // Optional: the pack's own Bedrock texture for a Java texture name ("block/stone" ->
    // textures/blocks/stone.png of a Bedrock pack), used before the vanilla Java texture, so a
    // Bedrock pack that adds Java models keeps its own pictures. nullopt when the pack has none.
    std::function<std::optional<Image>(std::string const&)> packBedrockTexture;
    // Optional last resort for textures neither source has (in game: the Bedrock
    // texture of the same name). Gets the Java texture name, e.g. "block/stone".
    std::function<std::optional<Image>(std::string const&)> fallbackTexture;
    // Prefix of the generated texture names (they become "<prefix><hash>").
    std::string texturePrefix = "jt_";
};

struct BakeResult {
    std::string                  rulesJson;                // {"format_version":1,"rules":[...]}
    std::map<std::string, Image> textures;                 // name -> image
    // name -> the Java textures drawn into it ("block/glass"), for connected-texture
    // rules that name a texture a pack's model uses.
    std::map<std::string, std::vector<std::string>> textureSources;
    std::map<std::string, int>   rulesPerBlock;            // Java block -> rules made
    std::vector<std::string>     problems;                 // things that could not be baked
    size_t                       ruleCount = 0;
    size_t                       itemShapes = 0;           // rules that shape a block drawn as an item
    // Shader maps of baked images, from the packs' labPBR textures (x_n.png, x_s.png), laid out
    // like the image and kept in labPBR form: "<name>_n" and "<name>_s". Only for images some
    // source texture has maps for.
    std::map<std::string, Image> pbrTextures;
};

BakeResult bake(BakeOptions const& options);
// Why the baker leaves a Java block (no namespace) to the game on purpose, or nullopt.
std::optional<std::string> blockSkipReason(std::string const& javaBlock);

// An item's inventory icon from a Java item model ("item/diamond_sword", or a pack path
// ending in .json): the "layer0", "layer1"... textures stacked (item/generated), or for a
// model with elements its south face, flattened. nullopt when nothing could be drawn.
// flatOnly: a model with elements (a 3D block or item model) gives nullopt instead of its front.
std::optional<Image> renderItemIcon(BakeOptions const& options, std::string const& model, bool flatOnly = false);

// One quad of a held item's mesh, cut per texel as Bedrock builds held items from their icon,
// in the icon's pixels (x right, y through it, z down the picture; a flat item is 0..16 x 0..1 x
// 0..16). Corners go round with cross(p1 - p0, p3 - p0) facing out; color is one texel (RGBA,
// R in the low byte) times its tint.
struct HandQuad {
    std::array<float, 12> pos{};
    std::array<float, 3>  normal{};
    uint32_t              color = 0;
};
// A model with elements as a held item: the model placed as Java places it relative to a flat
// item when held (third person), dropped or framed; and from there to where first person puts
// it, as a 4x4 matrix (column major) on the icon's pixels.
struct HandMesh {
    std::vector<HandQuad> quads;
    // The same quads in the model's own space: block units (pixels / 16) on Java's axes
    // (x east, y up, z south), element rotations applied, no display transform.
    std::vector<HandQuad> model;
    std::array<float, 16> firstPerson{};
};

// Item icons drawn from one pack stack (keeps what it loaded between calls).
class ItemIcons {
public:
    explicit ItemIcons(BakeOptions const& options); // options must outlive this
    ~ItemIcons();
    struct Info {
        bool found    = false; // the model exists (pack, vanilla or built in)
        bool flat     = false; // item/generated layers (no elements)
        bool ownModel = false; // the pack itself holds this model file
        bool touched  = false; // ... or one of the textures it draws
    };
    Info info(std::string const& model);
    // tints: RGB per layer / tintindex. A model with elements is drawn in Java's inventory
    // view (its display.gui transform), unless flatOnly.
    std::optional<Image> icon(std::string const& model, std::vector<uint32_t> const& tints, bool flatOnly);
    // A model with elements as Bedrock's held-item mesh (see HandQuad); nullopt for flat models.
    std::optional<HandMesh> handMesh(std::string const& model, std::vector<uint32_t> const& tints);
    // A flat item model's labPBR maps (normal "_n", specular "_s"), laid out like its icon;
    // nullopt when none of its layers has any.
    std::optional<std::pair<Image, Image>> flatPbr(std::string const& model);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
// A texture by pack path ("assets/minecraft/textures/item/x.png") or Java name ("item/x").
std::optional<Image> loadTexture(BakeOptions const& options, std::string const& pathOrName);

// labPBR maps (Java shader packs) in Bedrock's texture-set form, pixel by pixel: the normal map
// (green the DirectX way round) and metalness / emissive / roughness / subsurface.
uint32_t labToBedrockNormal(uint32_t labNormal);
uint32_t labToBedrockMers(uint32_t labSpecular);

// Identifies this build of the baker (cached bakes from another build are not reused).
std::string bakerVersion();

// Stand-ins for vanilla Java models packs inherit from without shipping them
// ("block/cube_all", "block/oak_button", ...). Used when neither source has the model.
std::optional<std::string> builtinModel(std::string const& name);
// Bedrock texture names (under textures/blocks/) to try for a Java texture.
std::vector<std::string> bedrockTextureNames(std::string const& javaTexture);

// Java block name (no namespace) -> its Java states -> Bedrock name and states.
struct BedrockState {
    int         dataVersion = 0;
    std::string name;   // Bedrock block name; "minecraft:" stripped
    std::string states; // "key=value,key=value" as in the table
};
using MappingTable = std::map<std::string, std::map<std::string, BedrockState>>;
MappingTable const& mappings(); // decompressed once, on first use

} // namespace tessera::java
