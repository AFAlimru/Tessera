# Tessera 說明

繁體中文 · [简体中文](zh-Hans.md) · [English](en.md)

Tessera 是 Minecraft 基岩版的用戶端模組（Windows、LeviLamina）。裝了它，材質包就能用 **Java 版材質包的格式**做方塊和物品：方塊狀態、3D 模型、連接材質、自訂物品材質等。材質包原本的基岩版內容全部照常作用，再加上 Tessera 讀的 Java 格式檔案。

目錄：[能做什麼](#能做什麼) · [怎麼運作](#怎麼運作) · [製作材質包](#製作材質包) · [tessera-tool](#tessera-tool) · [給模組的 API](#給模組的-api) · [限制](#限制)

## 能做什麼

### 方塊
- **依方塊狀態換模型。** `assets/minecraft/blockstates/*.json` 寫法和 Java 版一樣：`variants`、`multipart`、`x`／`y` 旋轉、加權隨機模型（`uvlock` 還沒支援）。多個材質包有同一個方塊狀態檔時，照 Java 版的做法逐狀態合併。
- **3D 模型。** 有 `elements` 的 Java 方塊模型（Blockbench 的「Java 方塊／物品」模型）：元素旋轉與 `rescale`、`cullface`、`shade`、`tintindex`（草、樹葉、紅石）、半透明與鏤空貼圖。
- **手上和物品欄也一樣。** 方塊的 Java 物品模型拿在手上、放在格子裡都是真的立體方塊（依 `display.gui` 擺放）。
- **基岩版沒有的狀態。** Tessera 在用戶端照 Java 版的規則算出來，所以為 Java 寫的方塊狀態檔可以直接用：
  - 音符盒的 `note`／`instrument`、唱片機的 `has_record`、比較器輸出、活塞的 `extended`；
  - 紅石線、絆線、玻璃片的相連；
  - 門上下半的 `open`／`hinge`、樹葉的 `distance`（枯萎顯示）；
  - 任何方塊推算出的 `powered`／`power`，以及標靶、幽匿感測體的 `signal`。
- **連接材質**（OptiFine CTM 格式，`optifine/ctm/**.properties`）：`ctm_compact`、`fixed`、`random`、`repeat`、`top`、`overlay`、`overlay_fixed`、`overlay_random`、`overlay_repeat`，支援 `matchTiles`／`matchBlocks`、`connect`、`faces`、`heights`、`innerSeams`、`tintIndex`／`tintBlock`。3D 模型的面也會連接（無縫玻璃片）。
- **流體。** Java 版的水、熔岩貼圖取代基岩版的。

### 物品
- **1.21.4 起的物品模型定義**（`assets/minecraft/items/*.json`）：`model`、`composite`、`condition`、`select`、`range_dispatch`，屬性如 `display_context`、`damage`、`count`、`custom_name`、`component`（藥水內容、附魔、附魔書、可疑的燉菜）。
- **OptiFine CIT**（`optifine/cit/**.properties`，`type=item`）：`items`、`damage`、`stackSize`、`enchantmentIDs`／`enchantmentLevels`、`nbt.display.Name`（完全符合、萬用字元、正規表達式）、藥水。
- 圖示在所有地方都會換：快捷欄、物品欄、容器、滑鼠拿著的物品。
- **3D 手持物品。** 有 `elements` 的物品模型，拿在手上、丟在地上、放進物品展示框都是立體的。

### 光影與 PBR
- Tessera 畫的每張貼圖，它的 **labPBR 貼圖**（`x_n.png`、`x_s.png`）都會變成基岩版的材質集。Vibrant Visuals 和光影模組（例如 Arke）能讀到法線、粗糙度、金屬度、發光。

### 基岩版材質包加上 Tessera 效果
- 材質包的基岩版檔案全部照常作用。
- Java 模型用到的貼圖，材質包只有基岩版版本時（`block/stone` → `textures/blocks/stone.png`），會用那張基岩版貼圖，不必另外複製一份到 Java 資料夾。

### 給玩家和作者
- **F3+R**：不離開世界重新載入材質包（實驗性）。
- **tessera-tool**：不用開遊戲就能檢查、預覽、打包材質包（[見下方](#tessera-tool)）。

## 怎麼運作

1. **讀取。** Tessera 透過遊戲自己的資源讀取器讀 Java 檔案，所以看到的就是「全域資源」裡啟用的材質包和它們的順序。材質包依賴的原版 Java 模型和貼圖，從玩家自己電腦上的 Java 版客戶端 jar 讀（`%APPDATA%\.minecraft\versions`，只讀取、不複製）。
2. **烘焙。** 材質包動到的每個方塊，它的每個 Java 方塊狀態，都會對應到基岩版的方塊和狀態（用 Chunker 的 Java→基岩版對照表）。每個狀態的模型會被「烘焙」：
   - 普通的方塊變成六面的圖；
   - 其他形狀變成一組帶自己圖片的四邊形。
   圖片加進遊戲的地形圖集。結果存在 `mods/Tessera/data/java_bake.cache`，讀過的檔案沒變就直接沿用。
3. **繪製。** 遊戲建區塊時，Tessera 攔下方塊的繪製，找出方塊目前狀態（含計算狀態）對應的規則，然後二選一：
   - 把面的 UV 換到烘焙好的圖；
   - 或把方塊原本的四邊形收起來，畫烘焙好的四邊形，光照沿用方塊原本的面。
   連接材質也在這時依鄰居決定。
4. **計算狀態。** 畫的時候讀不到的值（方塊實體、樹葉距離），在主執行緒讀進快照；值一變，就重建那個區塊。
5. **物品。** 遊戲要物品圖示時換成材質包的圖示；3D 手持物品取代遊戲從圖示建出的網格。

磁碟上不會轉換任何東西：材質包一直是「基岩版材質包裡放著 Java 格式檔案」。

## 製作材質包

建議的做法：**從你的基岩版材質包開始，基岩版做不到的部分再加 Java 格式的檔案。** 這些檔案用你熟悉的 Java 版工具製作，Tessera 原樣讀取。

### 1. 準備資料夾
```
tessera-tool init MyPack
```
這會在基岩版材質包（有 `manifest.json` 的資料夾）裡加上下列資料夾，並寫一份 `assets/TESSERA.txt`：
```
MyPack/
  manifest.json、pack_icon.png、textures/...     ← 原本的基岩版材質包，不變
  assets/minecraft/
    blockstates/       方塊狀態 → 模型（Java 格式）
    models/block/      方塊模型（Blockbench：「Java 方塊／物品」）
    models/item/       物品模型
    items/             1.21.4 起的物品模型定義
    textures/block/    貼圖（＋ labPBR 的 _n／_s、.png.mcmeta）
    textures/item/
    optifine/ctm/      連接材質
    optifine/cit/      自訂物品材質
```

### 2. 用 Java 版工具製作
- **模型：** Blockbench 的「Java 方塊／物品」格式，匯出到 `models/block` 或 `models/item`；貼圖用 Java 名稱（`block/oak_planks`、`block/my_texture`）。
- **方塊狀態：** 照 Java 版寫 `blockstates/<方塊>.json`（方塊用 Java 名稱，例如 `note_block`、`redstone_wire`）。
- **貼圖：** 任何繪圖軟體。基岩版那邊已經有的貼圖，不必再放一份 Java 版的。
- **連接材質、CIT：** OptiFine 的格式（[OptiFine 文件](https://github.com/sp614x/optifine/tree/master/OptiFineDoc/doc)，或會產生這種檔案的工具）。
- **PBR：** labPBR 的 `_n`／`_s` 放在貼圖旁邊。

模型裡的 `block/x` 會用哪張圖（先找到的為準）：
1. 任何啟用中的材質包裡的 Java 貼圖 `assets/minecraft/textures/block/x.png`；
2. 材質包裡同一方塊的基岩版貼圖（`textures/blocks/…`，用基岩版名稱：`block/oak_planks` → `planks_oak`）；
3. 玩家 jar 裡的原版 Java 貼圖；
4. 基岩版內建的貼圖。

### 3. 檢查與預覽
```
tessera-tool check MyPack       → tessera-check/report.html
tessera-tool preview MyPack     → tessera-preview/preview.html
```
報告會列出哪些地方不會生效、為什麼；預覽會畫出每個方塊狀態、連接材質的範例牆、物品圖示，和 Tessera 在遊戲裡顯示的一樣。

### 4. 進遊戲試
把材質包資料夾放進 `resource_packs`，在「全域資源」啟用，進入世界。改了檔案後，在世界裡按 **F3+R** 重新載入。

### 5. 發布
```
tessera-tool pack MyPack -o MyPack.mcpack
```
玩家要裝 Tessera 模組才看得到 Java 格式的部分；沒裝的話，仍然是你原本的基岩版材質包。

只發布自己的作品。不要附上原版 Java 檔案或別人的材質包。

之後可能會做我們自己的繪製工具；在那之前，Java 生態圈的工具就是編輯器。

## tessera-tool

建置：`xmake build tessera_tool` → `build/windows/x64/release/tessera-tool.exe`。每個指令都可以加 `--lang en | zh-Hans | zh-Hant`（預設跟系統語言）。

| 指令 | 作用 |
|---|---|
| `init <材質包資料夾>` | 在基岩版材質包加上 Java 格式的資料夾和 `assets/TESSERA.txt`。 |
| `check <pack> [-o 資料夾] [--vanilla jar\|off]` | 寫出 `report.html`，列出這些項目：<br>・每個方塊的規則數；<br>・基岩版沒有的方塊；<br>・缺少的方塊狀態、模型、parent、貼圖；<br>・Tessera 不處理的方塊；<br>・取自基岩版部分的貼圖；<br>・動態貼圖；<br>・CTM、CIT、物品定義的問題；<br>・Tessera 不讀的檔案。<br>有錯誤時結束碼為 1。 |
| `preview <pack> [-o 資料夾] [--block 名稱] [--max 數量]` | 寫出 `preview.html`：每個方塊狀態從東南方看的樣子（相同外觀只畫一次）、每個連接材質規則一面範例牆、物品圖示。 |
| `pack <材質包資料夾> [-o 檔案.mcpack] [--name 名稱]` | 打包成 zip。沒有 `manifest.json` 時產生一份（新的 UUID，也另存在輸出檔旁邊：放回材質包資料夾保留），沒有 `pack_icon.png` 時用 `pack.png`。 |

`<pack>` 可以是資料夾、`.zip` 或 `.mcpack`。

## 給模組的 API

只有標頭檔，不用連結：`include/Tessera/Api.h` 在執行時找 `Tessera.dll`。沒裝 Tessera 時，每個呼叫都回傳 false（或 0）。完整說明：[docs/api.md](../api.md)。

**狀態值**（API 1）：給任何位置一個值，材質包規則可以拿來判斷。
```cpp
#include "Tessera/Api.h"
tessera::api::Client tessera;
tessera.setInt(0, x, y, z, "mymod:charge", 3);   // 維度：0 主世界、1 地獄、2 終界
tessera.setBool(0, x, y, z, "mymod:active", true);
tessera.setString(0, x, y, z, "mymod:mode", "fast");
tessera.clear(0, x, y, z, "mymod:charge");
```

**計算函式**（API 2）：畫到「規則有用到這個鍵」的方塊時，Tessera 會呼叫你的函式。
```cpp
bool charge(int dim, int x, int y, int z, char const* block, void* user, tessera_value* out) {
    out->kind = TESSERA_VALUE_INT;
    out->i    = myCharge(dim, x, y, z);   // 只讀自己的資料：在建區塊的執行緒上執行
    return true;
}
tessera.registerProvider("mymod:charge", &charge);
tessera.markDirty(dim, x, y, z);           // 這格的答案變了
```

**物品模型**（API 3）：材質包給物品的 3D 模型，給自己畫物品的模組用（掉落物物理、展示架）。
```cpp
std::vector<tessera_quad> quads(4096);
int n = tessera.itemModel(&itemStack, quads.data(), int(quads.size()));
// n == 0：沒有 3D 模型。座標是方塊單位、Java 的軸向（x 東、y 上、z 南），
// 依像素切開，每塊一個 RGBA 顏色；cross(p1 - p0, p3 - p0) 朝外。
```

鍵必須有命名空間；`tessera:` 和 `minecraft:` 保留。所有呼叫都是執行緒安全的。C 匯出函式：`tessera_api_version`（3）、`tessera_set_state_int/bool/string`、`tessera_clear_state`、`tessera_register_provider`、`tessera_unregister_provider`、`tessera_mark_dirty`、`tessera_item_model`。

## 限制

- 柵欄、牆、鐵柵欄、歌萊植株保留基岩版的網格。它們的形狀靠鄰居決定，Tessera 還沒算；連接材質照常作用。
- 動態貼圖只顯示第一格畫面（水和熔岩用基岩版的動畫）。
- 實體、介面、粒子、天空、字型、音效、CEM 的 Java 檔案 Tessera 不讀；這些用材質包的基岩版部分做。
- 連接材質的 `ctm`（47 格）、`horizontal`、`vertical` 及組合、`overlay_ctm`、`biomes` 還沒支援。
- 丟出去的藥水由遊戲畫成平面圖。
- 目前只有 Windows；Android 之後做。
