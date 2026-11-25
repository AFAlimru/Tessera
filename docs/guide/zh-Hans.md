# Tessera 说明

[繁體中文](zh-Hant.md) · 简体中文 · [English](en.md)

Tessera 是 Minecraft 基岩版的客户端模组（Windows、LeviLamina）。装了它，资源包就能用 **Java 版资源包的格式**做方块和物品：方块状态、3D 模型、连接纹理、自定义物品纹理等。资源包原本的基岩版内容全部照常生效，再加上 Tessera 读取的 Java 格式文件。

目录：[能做什么](#能做什么) · [怎么运作](#怎么运作) · [制作资源包](#制作资源包) · [tessera-tool](#tessera-tool) · [给模组的 API](#给模组的-api) · [限制](#限制)

## 能做什么

### 方块
- **按方块状态换模型。** `assets/minecraft/blockstates/*.json` 写法和 Java 版一样：`variants`、`multipart`、`x`／`y` 旋转、加权随机模型（`uvlock` 还没支持）。多个资源包有同一个方块状态文件时，按 Java 版的做法逐状态合并。
- **3D 模型。** 带 `elements` 的 Java 方块模型（Blockbench 的“Java 方块／物品”模型）：元素旋转与 `rescale`、`cullface`、`shade`、`tintindex`（草、树叶、红石）、半透明与镂空贴图。
- **手上和物品栏也一样。** 方块的 Java 物品模型拿在手上、放在格子里都是真正的立体方块（按 `display.gui` 摆放）。
- **基岩版没有的状态。** Tessera 在客户端按 Java 版的规则算出来，所以为 Java 写的方块状态文件可以直接用：
  - 音符盒的 `note`／`instrument`、唱片机的 `has_record`、比较器输出、活塞的 `extended`；
  - 红石粉、绊线、玻璃板的连接；
  - 门上下半的 `open`／`hinge`、树叶的 `distance`（枯萎显示）；
  - 任何方块推算出的 `powered`／`power`，以及标靶、幽匿感测体的 `signal`；
  - 被通电激活铁轨锁住的漏斗矿车，车上的漏斗显示为锁住（`enabled=false`）。
- **连接纹理**（OptiFine CTM 格式，`optifine/ctm/**.properties`）：`ctm_compact`、`fixed`、`random`、`repeat`、`top`、`overlay`、`overlay_fixed`、`overlay_random`、`overlay_repeat`，支持 `matchTiles`／`matchBlocks`、`connect`、`faces`、`heights`、`innerSeams`、`tintIndex`／`tintBlock`。3D 模型的面也会连接（无缝玻璃板）。
- **流体。** Java 版的水、熔岩贴图取代基岩版的。流动贴图的 `.mcmeta` 写 `"tessera": {"flow_sides": "middle"}` 时，侧面和顶面显示同一块中间部分（箭头、标记在每一面都居中）。

### 物品
- **1.21.4 起的物品模型定义**（`assets/minecraft/items/*.json`）：`model`、`composite`、`condition`、`select`、`range_dispatch`，属性如 `display_context`、`damage`、`count`、`custom_name`、`component`（药水内容、附魔、附魔书、迷之炖菜）。
- **OptiFine CIT**（`optifine/cit/**.properties`，`type=item`）：`items`、`damage`、`stackSize`、`enchantmentIDs`／`enchantmentLevels`、`nbt.display.Name`（完全匹配、通配符、正则表达式）、药水。
- 图标在所有地方都会换：快捷栏、物品栏、容器、鼠标拿着的物品。
- **3D 手持物品。** 带 `elements` 的物品模型，拿在手上、丢在地上、放进物品展示框都是立体的。

### 光影与 PBR
- Tessera 画的每张贴图，它的 **labPBR 贴图**（`x_n.png`、`x_s.png`）都会变成基岩版的纹理集。Vibrant Visuals 和光影模组（例如 Arke）能读到法线、粗糙度、金属度、发光。

### 基岩版资源包加上 Tessera 效果
- 资源包的基岩版文件全部照常生效。
- Java 模型用到的贴图，资源包只有基岩版版本时（`block/stone` → `textures/blocks/stone.png`），会用那张基岩版贴图，不必另外复制一份到 Java 文件夹。

### 给玩家和作者
- **F3+R**：不离开世界重新加载资源包（实验性）。
- **示例资源包：**[发布页面](https://github.com/AFAlimru/Tessera/releases/latest)的 `TesseraRedstoneInfo.mcpack`，用像素数字显示红石信号、音高、各种等级、附魔与药水等级，加上液体流向箭头、无缝玻璃、3D 工作台和灯笼。内容全部由 Tessera 自己的脚本（`tools/make_info_pack.py`）画出。
- **tessera-tool**：不用开游戏就能检查、预览、打包资源包（[见下方](#tessera-tool)）。

## 怎么运作

1. **读取。** Tessera 通过游戏自己的资源读取器读 Java 文件，所以看到的就是“全局资源”里启用的资源包和它们的顺序。资源包依赖的原版 Java 模型和贴图，从玩家自己电脑上的 Java 版客户端 jar 读取（`%APPDATA%\.minecraft\versions`，只读取、不复制）。
2. **烘焙。** 资源包动到的每个方块，它的每个 Java 方块状态，都会对应到基岩版的方块和状态（用 Chunker 的 Java→基岩版对照表）。每个状态的模型会被“烘焙”：
   - 普通的方块变成六个面的图；
   - 其他形状变成一组带自己图片的四边形。
   图片加入游戏的地形图集。结果存在 `mods/Tessera/data/java_bake.cache`，读过的文件没变就直接沿用。
3. **绘制。** 游戏构建区块时，Tessera 拦截方块的绘制，找出方块当前状态（含计算状态）对应的规则，然后二选一：
   - 把面的 UV 换到烘焙好的图；
   - 或者把方块原本的四边形收起来，画烘焙好的四边形，光照沿用方块原本的面。
   连接纹理也在这时按邻居决定。
4. **计算状态。** 绘制时读不到的值（方块实体、树叶距离），在主线程读进快照；值一变，就重建那个区块。
5. **物品。** 游戏要物品图标时换成资源包的图标；3D 手持物品取代游戏从图标建出的网格。

磁盘上不会转换任何东西：资源包始终是“基岩版资源包里放着 Java 格式文件”。

## 制作资源包

推荐的做法：**从你的基岩版资源包开始，基岩版做不到的部分再加 Java 格式的文件。** 这些文件用你熟悉的 Java 版工具制作，Tessera 原样读取。

### 1. 准备文件夹
```
tessera-tool init MyPack
```
这会在基岩版资源包（有 `manifest.json` 的文件夹）里加上下列文件夹，并写一份 `assets/TESSERA.txt`：
```
MyPack/
  manifest.json、pack_icon.png、textures/...     ← 原本的基岩版资源包，不变
  assets/minecraft/
    blockstates/       方块状态 → 模型（Java 格式）
    models/block/      方块模型（Blockbench：“Java 方块／物品”）
    models/item/       物品模型
    items/             1.21.4 起的物品模型定义
    textures/block/    贴图（＋ labPBR 的 _n／_s、.png.mcmeta）
    textures/item/
    optifine/ctm/      连接纹理
    optifine/cit/      自定义物品纹理
```

### 2. 用 Java 版工具制作
- **模型：** Blockbench 的“Java 方块／物品”格式，导出到 `models/block` 或 `models/item`；贴图用 Java 名称（`block/oak_planks`、`block/my_texture`）。
- **方块状态：** 按 Java 版写 `blockstates/<方块>.json`（方块用 Java 名称，例如 `note_block`、`redstone_wire`）。
- **贴图：** 任何绘图软件。基岩版那边已经有的贴图，不必再放一份 Java 版的。
- **连接纹理、CIT：** OptiFine 的格式（[OptiFine 文档](https://github.com/sp614x/optifine/tree/master/OptiFineDoc/doc)，或能生成这种文件的工具）。
- **PBR：** labPBR 的 `_n`／`_s` 放在贴图旁边。

模型里的 `block/x` 会用哪张图（先找到的为准）：
1. 任何启用中的资源包里的 Java 贴图 `assets/minecraft/textures/block/x.png`；
2. 资源包里同一方块的基岩版贴图（`textures/blocks/…`，用基岩版名称：`block/oak_planks` → `planks_oak`）；
3. 玩家 jar 里的原版 Java 贴图；
4. 基岩版内置的贴图。

### 3. 检查与预览
```
tessera-tool check MyPack       → tessera-check/report.html
tessera-tool preview MyPack     → tessera-preview/preview.html
```
报告会列出哪些地方不会生效、为什么；预览会画出每个方块状态、连接纹理的示例墙、物品图标，和 Tessera 在游戏里显示的一样。

### 4. 进游戏试
把资源包文件夹放进 `resource_packs`，在“全局资源”启用，进入世界。改了文件后，在世界里按 **F3+R** 重新加载。

### 5. 发布
```
tessera-tool pack MyPack -o MyPack.mcpack
```
玩家要装 Tessera 模组才看得到 Java 格式的部分；没装的话，仍然是你原本的基岩版资源包。

只发布自己的作品。不要附带原版 Java 文件或别人的资源包。

之后可能会做我们自己的绘制工具；在那之前，Java 生态的工具就是编辑器。

## tessera-tool

构建：`xmake build tessera_tool` → `build/windows/x64/release/tessera-tool.exe`。每个命令都可以加 `--lang en | zh-Hans | zh-Hant`（默认跟随系统语言）。

| 命令 | 作用 |
|---|---|
| `init <资源包文件夹>` | 在基岩版资源包加上 Java 格式的文件夹和 `assets/TESSERA.txt`。 |
| `check <pack> [-o 文件夹] [--vanilla jar\|off]` | 写出 `report.html`，列出这些项目：<br>・每个方块的规则数；<br>・基岩版没有的方块；<br>・缺少的方块状态、模型、parent、贴图；<br>・Tessera 不处理的方块；<br>・取自基岩版部分的贴图；<br>・动态贴图；<br>・CTM、CIT、物品定义的问题；<br>・Tessera 不读的文件。<br>有错误时退出码为 1。 |
| `preview <pack> [-o 文件夹] [--block 名称] [--max 数量]` | 写出 `preview.html`：每个方块状态从东南方看的样子（相同外观只画一次）、每个连接纹理规则一面示例墙、物品图标。 |
| `pack <资源包文件夹> [-o 文件.mcpack] [--name 名称]` | 打包成 zip。没有 `manifest.json` 时生成一份（新的 UUID，也另存在输出文件旁边：放回资源包文件夹保留），没有 `pack_icon.png` 时用 `pack.png`。 |

`<pack>` 可以是文件夹、`.zip` 或 `.mcpack`。

## 给模组的 API

只有头文件，不用链接：`include/Tessera/Api.h` 在运行时找 `Tessera.dll`。没装 Tessera 时，每个调用都返回 false（或 0）。完整说明（繁体中文）：[docs/api.md](../api.md)。

**状态值**（API 1）：给任何位置一个值，资源包规则可以拿来判断。
```cpp
#include "Tessera/Api.h"
tessera::api::Client tessera;
tessera.setInt(0, x, y, z, "mymod:charge", 3);   // 维度：0 主世界、1 下界、2 末地
tessera.setBool(0, x, y, z, "mymod:active", true);
tessera.setString(0, x, y, z, "mymod:mode", "fast");
tessera.clear(0, x, y, z, "mymod:charge");
```

**计算函数**（API 2）：画到“规则用到这个键”的方块时，Tessera 会调用你的函数。
```cpp
bool charge(int dim, int x, int y, int z, char const* block, void* user, tessera_value* out) {
    out->kind = TESSERA_VALUE_INT;
    out->i    = myCharge(dim, x, y, z);   // 只读自己的数据：在构建区块的线程上运行
    return true;
}
tessera.registerProvider("mymod:charge", &charge);
tessera.markDirty(dim, x, y, z);           // 这一格的答案变了
```

**物品模型**（API 3）：资源包给物品的 3D 模型，给自己画物品的模组用（掉落物物理、展示架）。
```cpp
std::vector<tessera_quad> quads(4096);
int n = tessera.itemModel(&itemStack, quads.data(), int(quads.size()));
// n == 0：没有 3D 模型。坐标是方块单位、Java 的轴向（x 东、y 上、z 南），
// 按像素切开，每块一个 RGBA 颜色；cross(p1 - p0, p3 - p0) 朝外。
```

键必须带命名空间；`tessera:` 和 `minecraft:` 保留。所有调用都是线程安全的。C 导出函数：`tessera_api_version`（3）、`tessera_set_state_int/bool/string`、`tessera_clear_state`、`tessera_register_provider`、`tessera_unregister_provider`、`tessera_mark_dirty`、`tessera_item_model`。

## 限制

- 栅栏、墙、铁栏杆、紫颂植株保留基岩版的网格。它们的形状靠邻居决定，Tessera 还没算；连接纹理照常生效。
- 动态贴图只显示第一帧（水和熔岩用基岩版的动画）。
- 实体、界面、粒子、天空、字体、音效、CEM 的 Java 文件 Tessera 不读；这些用资源包的基岩版部分做。
- 连接纹理的 `ctm`（47 格）、`horizontal`、`vertical` 及组合、`overlay_ctm`、`biomes` 还没支持。
- 扔出去的药水由游戏画成平面图。
- 目前只有 Windows；Android 之后做。
