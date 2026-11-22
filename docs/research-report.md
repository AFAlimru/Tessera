# 基岩版「依方塊狀態換材質」研究報告

日期：2026-11-10　對象版本：Minecraft 1.26.60.05、LeviLamina 26.60.x（Windows）、Arke 0.1.1

## 結論

1. **可行，而且不需要改遊戲的渲染器**。最穩的做法是在「區塊網格建好、還沒上傳 GPU」的時間點，把屬於某個狀態的方塊的四邊形 UV 重新指到另一張已經放進地形圖集的材質（以下稱 **UV 重映射**）。這一步只動 CPU 端的頂點資料，跟 D3D12 / GLES / Vibrant Visuals 都無關，所以同一套邏輯可以搬到安卓。
2. **材質包只要多放一個 JSON**（規則）加上把新材質登記進 `terrain_texture.json`，不需要寫程式；其他模組可以透過 C API 加入動態規則。
3. **跟 Arke 相容的條件已經查清楚**：Arke 自己就 hook 了同一批函式（區塊建網格、`Tessellator::end`、`tessellateInWorld`），它讀 UV 算 `mc_midTexCoord`／PBR 圖集。只要我們的重映射在 Arke 讀 UV **之前**完成，光影的法線／高光貼圖就會自動跟著新材質走。光影用的方塊 ID（`block.properties` → `mc_Entity`）是看方塊狀態，不看材質，所以不受影響。
4. **安卓 .so 可行，但維護成本高**：PreLoader 有 Hook／Signature API，但沒有遊戲標頭，`libminecraftpe.so` 的類別函式要用特徵碼找，每次遊戲更新都要重找。建議核心邏輯寫成跟平台無關的 C++，Windows 和安卓各寫一層薄的 hook 層。

## 1. 基岩版原本做不到的原因

| 機制 | 能不能依狀態換材質 | 說明 |
| --- | --- | --- |
| `blocks.json` | 不行 | 只能依「面」（up/down/side…）指定材質名稱，例如紅石線是 `up: redstone_dust_cross`、`down/side: redstone_dust_line`。 |
| `terrain_texture.json` 的材質陣列 | 只有寫死的方塊可以 | 陣列的索引由遊戲 C++ 的 `BlockType::getVariant(Block const&)` 決定（例如 `grass_side` 有 17 個：16 種生態域色 + 積雪）。材質包無法改「哪個狀態對應哪個索引」。 |
| 自訂方塊的 permutations + `material_instances` | 只限自訂方塊 | 這是官方唯一能依狀態換材質的方法，但行為包不能重新定義 `minecraft:` 原版方塊。 |
| 紅石線顏色 | 只能染色 | 紅石線是同一張灰階材質，依 `redstone_signal` 由 `BlockGraphics::getColor` 染色，無法換圖。 |

所以「顯示紅石強度數字」、「充能／未充能用不同圖」這類 Java 材質包（blockstates + models，或 OptiFine/Continuity CTM 的 `matchBlocks=redstone_wire:power=15`）在基岩版純資源包做不到，必須靠模組。

## 2. 遊戲端的渲染管線（從 LeviLamina 26.60 標頭整理）

`MCAPI` = LeviLamina 有符號、可直接 hook／呼叫；`MCNAPI` = 沒有符號，只能特徵碼掃描。

| 函式／成員 | 可用性 | 用途 |
| --- | --- | --- |
| `RenderChunkBuilder::build(...)` | MCAPI | 每個區塊子區段建網格的入口。成員同時有 `mBlockTessellator`（舊管線）、`mPipelineTessellator`（新的資料驅動管線）、`mUseNewTessellationPipeline` 旗標、`mMeshData`、`mBuiltMesh`。 |
| `Tessellator::end(UploadMode, ...)` | MCAPI | 網格完成、轉成 `mce::Mesh` 的點。此時 `mMeshData` 還在 CPU 上。 |
| `Tessellator::vertex(x,y,z)` | MCAPI | 舊管線每個頂點都會經過。 |
| `BlockTessellator::tessellateInWorld(Tessellator&, Block const&, BlockPos const&, bool)` | MCAPI | 舊管線單一方塊入口。LHolo 的投影也是呼叫這個。 |
| `BlockTessellator::_getTexture(...)`、`tessellateDustInWorld`（紅石線）等各形狀函式 | MCNAPI | 不能直接 hook。 |
| `BlockGraphics::getTexture(BlockPos const&, slot, variant)` | MCAPI | 舊管線查材質，但參數沒有完整的 `Block`。 |
| `BlockGraphics::getTextureUVCoordinateSet(name, blockVariant, textureVariant)`（static） | MCAPI | 用材質名稱查圖集座標。我們用它把規則裡的材質名轉成 UV。 |
| `BlockGraphics::initBlocks` / `reloadBlockTextures(ResourcePackManager&, ...)` | MCAPI | 資源包（重新）載入的時間點，拿得到 `ResourcePackManager`。 |
| `LevelRenderer::rebuildAllRenderChunkGeometry()` | LLAPI | 規則改變後重建所有區塊。 |
| `ClientBlockPipeline::*`（`MaterialOverrideStep`、`BakedMaterialMap`…）、`VanillaBlockTessellation::*` | 幾乎全是 MCNAPI | 新管線的材質在烘焙時就決定好，**hook `getTexture` 對新管線的方塊無效**。 |

`mce::MeshData` 的頂點資料是一般的 `std::vector`：`mPositions`、`mNormals`、`mColors`、`mTextureUVs[3]`、`mPBRTextureIndices`、`mMERS`。`TextureUVCoordinateSet` 帶有 `_u0/_v0/_u1/_v1`、`textureSetTranslationIndex`、`mPBRTextureDataHandle`（Vibrant Visuals 用）。

重點：**原版方塊分兩條管線**，而且新管線可以一次處理整個區塊（`setupFullChunkInputs`），不會逐方塊呼叫 `tessellateInWorld`。所以只 hook 查材質的函式會漏掉一部分方塊；在區塊網格完成後處理才能兩條都涵蓋。

## 3. Arke 的分析（靜態字串，沒有執行）

Arke 是 LiteLDev 的閉源模組（GitHub 只有 README），讓 Iris/OptiFine 光影跑在基岩版 D3D12 上；0.1.1 只有 Windows，README 寫安卓支援「計畫中」，另外在設計一種 HLSL 光影格式。

從 `arke.dll` 解析出的符號：

- **它 hook 的遊戲函式**：`RenderChunkBuilder::build`、`BlockTessellator::tessellateInWorld`、`tessellateDoubleThinFenceInWorld`、`Tessellator::vertex`、`Tessellator::end`、`BlockGraphics::getTextureUVCoordinateSet`、`LevelRenderer::rebuildAllRenderChunkGeometry`、`BlockActorRenderDispatcher::render`、`SkinnedMesh::draw` 等。用的是 `ll::memory::hook` / `hookEx`（有優先度）。
- **方塊 ID**：支援 Iris 的 `block.properties` → `mc_Entity`，也有 `at_midBlock`、`mc_midTexCoord`、`blockEntityId`、`currentRenderedItemId`、`currentSelectedBlockId`。
- **狀態對照表**：內建 Bedrock → Java 的名稱／狀態轉換（`lit_redstone_lamp → redstone_lamp + lit`、`powered_repeater → repeater + powered`、`repeater_delay → delay`、`ground_sign_direction → rotation`…，也有 `redstone_signal`），所以光影包可以照 Java 寫法依狀態給 ID。
- **PBR**：讀 `.texture_set.json`，自己建一張跟地形圖集**位置對齊**的法線圖集與高光圖集（`requestPbrAtlas(AtlasLayout)`、`arke atlas normals/specular`）。

對我們的意義：

| 項目 | 影響 | 對策 |
| --- | --- | --- |
| 光影的方塊 ID | 不受影響（看狀態不看材質） | 無 |
| PBR 法線／高光 | 圖集位置對齊，UV 指到新材質後，自動取到新材質的 `texture_set.json` | 材質包為新材質附上 `texture_set.json` 即可 |
| `mc_midTexCoord`／POM | Arke 從 UV 算出；如果它先算、我們才改 UV，就會錯位 | 我們的重映射要比 Arke 先執行（見第 5 節） |
| 兩個模組 hook 同一個函式 | LeviLamina 的 hook 會串接，不會互相覆蓋 | 用優先度排順序，最好跟 Arke 作者協調一個公開的掛點 |

## 4. 方案比較

| 方案 | 涵蓋範圍 | PBR／Arke | 難度 | 評價 |
| --- | --- | --- | --- | --- |
| A. hook `getVariant`，擴充材質陣列 | 舊管線 | 正確 | 低 | **不建議**：`getVariant` 也用在掉落物、物品等遊戲邏輯，改它會有副作用 |
| B. hook 查材質（`BlockGraphics::getTexture` + 執行緒區域「目前方塊」） | 只有舊管線；紅石線這類用名稱查的要另外 hook | 正確（帶 PBR handle） | 中 | 新管線方塊無效，適合當補充 |
| **C. 區塊網格後處理（UV 重映射）** | **兩條管線都涵蓋**，LHolo 投影也會套用 | 正確（同時改 `mPBRTextureIndices`） | 中 | **建議的主方案** |
| D. 額外疊加幾何（例如在紅石線上加數字貼片） | 任意 | 需要另外處理 | 中高 | 第二階段，實現「紅石強度顯示」這類疊圖 |
| E. 完整 Java 模型取代 | 任意 | 要自己算光照／AO | 很高 | 第三階段，或不做 |

### 方案 C 的運作方式

1. 資源載入時（hook `BlockGraphics::initBlocks`／`reloadBlockTextures`）：讀所有資源包的規則檔，用 `getTextureUVCoordinateSet` 把「來源材質」和「目標材質」轉成圖集矩形。再把遊戲裡所有方塊排列（permutation）跑一次規則，建成 `Block* → 規則` 的唯讀表，換成新表時用原子指標替換，區塊建構執行緒可以不加鎖讀取。
2. `RenderChunkBuilder::build` hook：設定執行緒區域的「目前區塊」（`BlockSource`、區段原點）。
3. 在 `Tessellator::end`（或 `build` 回傳前的 `mMeshData`）：逐個四邊形：
   - 先用 UV 中心查它是不是落在某個「有規則的來源材質」矩形內；不是就跳過（絕大多數四邊形在這一步就結束，成本很低）。
   - 是的話，用四邊形中心減去法線 × 0.01 算出它屬於哪個方塊（面總是朝外，往內縮一點就落在擁有它的方塊裡），查方塊狀態 → 規則。
   - 套用：`u' = tu0 + (u - u0) / (u1 - u0) × (tu1 - tu0)`（v 同理），所以部分 UV（半磚、紅石線的直線段）、旋轉的 UV 都對；同時把 `mPBRTextureIndices` 換成目標材質的 PBR handle；規則要求時把頂點色改成白色（取消紅石染色）。
4. 舊管線的方塊另外在 `tessellateInWorld` 記下「這段頂點屬於哪個 `Block`」，就不用靠位置反推，比較精確。

## 5. API 設計草案

### 5.1 給材質包：`state_textures.json`（資源包根目錄）

```json
{
  "format_version": 1,
  "rules": [
    {
      "block": "minecraft:redstone_wire",
      "when": { "redstone_signal": "1..15" },
      "replace": {
        "redstone_dust_cross": "tessera_dust_cross_p{redstone_signal}",
        "redstone_dust_line":  "tessera_dust_line_p{redstone_signal}"
      },
      "tint": "none"
    },
    {
      "block": "minecraft:redstone_wire",
      "when": { "redstone_signal": 0 },
      "replace": { "redstone_dust_cross": "tessera_dust_cross_off" }
    }
  ]
}
```

- `block`：方塊名稱，可用陣列或 `#標籤`。
- `when`：狀態條件，用**基岩版狀態名稱**；支援單值、陣列、範圍 `"1..15"`。可選擇加 `"java": true` 讓我們用 Java 名稱（`power`）再自動轉換（對照表的概念跟 Arke 內建的一樣）。
- `replace`：「來源材質名 → 目標材質名」，以材質而不是以面來對應，所以任何形狀都適用；`{狀態名}` 會代入目前值，一條規則就能涵蓋 16 段紅石強度。
- `faces`（可選）：只換特定面。
- `tint`：`keep`（預設）／`none`／`#RRGGBB`。
- 新材質照一般方式登記在 `terrain_texture.json` 的 `texture_data`，PBR 就放同名的 `.texture_set.json`。
- 多個資源包：v1 只讀最上層的一份；v2 依資源包堆疊順序合併。

另外可以做一個離線轉換工具：把 OptiFine/Continuity CTM 的 `method=fixed` + `matchBlocks=...:power=15`，以及「只換材質、不換形狀」的 Java blockstates，轉成這個格式。

### 5.2 給其他模組：C API（`extern "C"`，跟 ModMenuBE 一樣的作法）

```cpp
uint32_t tessera_api_version();
// 執行時新增規則（跟 JSON 同結構）
bool tessera_add_rules_json(const char* json, const char* ownerId);
// 動態規則：每個 Block 排列只會問一次，結果會被快取
void tessera_add_resolver(TesseraResolveFn fn, void* user);   // fn(blockName, statesNbtJson, out)
// 網格處理完、上傳前通知，給 Arke 這類需要讀最終 UV 的模組
void tessera_add_mesh_listener(TesseraMeshFn fn, void* user);
// 規則改變後要求重建區塊
void tessera_invalidate();
```

### 5.3 跟 Arke 的整合順序

LeviLamina 的 hook 是串接的（`HookPriority`：Highest=0 … Lowest=400）。我們要在 Arke 讀 `mMeshData` 之前改完 UV。做法有兩個：

- **短期**：用優先度排在 Arke 前面，並在遊戲內實測 `mc_midTexCoord` 是否正確（可用一張有 POM 的光影包看紅石線）。
- **長期（建議）**：向 Arke 提議，由它在讀網格前呼叫一個公開的「網格前處理」掛點，或由我們提供上面的 `tessera_add_mesh_listener`；這樣就不用依賴 hook 順序。Arke 閉源，只能開 issue 跟作者談。

## 6. 安卓 .so 可行性

| 項目 | 狀況 |
| --- | --- |
| 載入／hook | PreLoader 有 `pl::memory::HookHandle`、`resolveSignature(符號名或特徵碼, "libminecraftpe.so")`、Patch API；目標 arm64-v8a。 |
| 遊戲標頭 | 官方文件只提供 `include/pl` 標頭，**沒有遊戲類別標頭**：`MeshData`、`Tessellator`、`TextureUVCoordinateSet`、`RenderChunkBuilder` 的結構要自己對照 Windows 版宣告。 |
| 找函式 | `libminecraftpe.so` 的類別函式大多沒有匯出符號，要用特徵碼；每次遊戲版本更新都要重找（需要 6–8 個：區塊建構、`Tessellator::end`、材質名查詢、資源載入、取方塊、讀方塊狀態、重建區塊）。 |
| 結構相容性 | `std::vector`、`std::optional`、浮點欄位在 libc++ 和 MSVC STL 的佈局相同；`std::string` 不同，所以要用 NDK（libc++ `__ndk1`，跟遊戲一致）編譯，傳字串給遊戲函式才不會錯。 |
| 渲染後端 | 方案 C 只動 CPU 端網格，GLES 或 Vulkan 都不影響，這是它在安卓上最大的優點。 |
| Arke | 安卓版還沒出，沒辦法驗證相容。 |
| 結論 | 技術上可行；建議等 Windows 版的規則格式和 API 穩定後再做。核心（規則解析、狀態比對、UV 重映射）寫成不依賴遊戲標頭的 C++，Windows（LeviLamina 符號）和安卓（特徵碼）各寫一層薄的 hook 層。 |

## 7. 待驗證的風險（做 PoC 時要先確認）

1. **沒被方塊使用的材質會不會打進地形圖集？** 預期 `terrain_texture.json` 裡的所有 `texture_data` 都會打包（自訂方塊就是這樣引用的），但要實測。如果不會，需要另一種登記方式。
2. **新管線輸出的頂點是否都在 `RenderChunkBuilder::mMeshData`／`Tessellator` 裡**，而且在我們的 hook 點還沒上傳。
3. **`mPBRTextureIndices` 該填 `mPBRTextureDataHandle` 還是 `textureSetTranslationIndex`**（Vibrant Visuals 下要測）。
4. **紅石線的染色**：取消染色是改頂點色就夠，還是材質（shader）也會再乘一次顏色。
5. **跟 Arke 的 hook 順序**（第 5.3 節）。
6. **效能**：只處理落在「有規則的來源材質」裡的四邊形，預期影響很小，但要在區塊大量載入時量一次。
7. **透明層排序**：半透明方塊（玻璃、水）之後還會被遊戲重新排序，UV 已經改好，理論上沒影響，要確認。

## 8. 建議路線

1. **PoC（Windows）**：只做紅石線 + 一條規則（`redstone_signal ≥ 1` 換一張圖），驗證第 7 節的 1、2、4。
2. **v0.1**：完整 `state_textures.json`、範圍與 `{狀態}` 代入、資源包重新載入、`rebuildAllRenderChunkGeometry`、C API。
3. **v0.2**：PBR 正確性（Vibrant Visuals + Arke）、跟 Arke 作者協調掛點、OptiFine CTM 轉換工具。
4. **v0.3**：疊加幾何（紅石強度數字等）。
5. **安卓**：核心抽出、特徵碼層。

## 9. 涵蓋範圍（2026-11-10 補充）

### 9.1 哪些東西會被涵蓋

方案 C 處理的是**放在世界裡、畫進區塊網格的方塊**。判斷依據：方塊在 `blocks.json`（原版分散在 `vanilla`、`vanilla_1.16`、`vanilla_1.21.0`…各版本資源包）裡有材質設定，而且沒有專屬的方塊實體渲染器。

| 類別 | 涵蓋 | 說明 |
| --- | --- | --- |
| 世界中的一般方塊與紅石元件 | ✅ | 狀態一改變，遊戲本來就會重建該區段網格，新材質會立即生效 |
| LHolo 投影 | ✅ | 投影也是呼叫 `tessellateInWorld` 來畫 |
| 活塞推動中的方塊、活塞臂動畫 | ⚠️ 要另外 hook | 走 `PistonBlockActorRenderer`／`MovingBlockActorRenderer`（兩者的 `$render` 都是 MCAPI） |
| 掉落中的方塊、礦車裡的方塊、終界使者拿的方塊 | ⚠️ 要另外 hook | 實體渲染，不在區塊網格裡 |
| 用模型畫的方塊實體：箱子、告示牌、床、旗幟、頭顱、鐘的本體、講台上的書、界伏盒、飾紋陶罐等 | ❌ | 用實體模型和實體材質（`textures/entity`），是另一套系統 |
| 物品欄圖示、手持物品、掉落物 | ❌（也不需要） | 物品沒有方塊狀態，一律用預設狀態；物品圖示用另一張物品圖集 |

### 9.2 紅石元件與容器

「狀態」欄是基岩版的方塊狀態名稱；有一部分已經從 Arke 內建的對照表交叉確認過。

| 方塊 | 可以依什麼換材質 | 備註 |
| --- | --- | --- |
| 紅石線 | `redstone_signal` 0–15 | 每一段強度都能用一張圖；可取消原本的染色 |
| 紅石火把、紅石燈 | 亮／不亮本來就是不同方塊 ID | 原版已經能分開換圖，不需要這個模組 |
| 中繼器 | `repeater_delay`、亮／不亮（不同方塊 ID） | 「被鎖住」不是方塊狀態，是渲染時檢查鄰居畫出來的 → 要用 9.3 的幾何判斷 |
| 比較器 | `output_subtract_bit`、亮／不亮 | **輸出強度不在方塊狀態裡**，存在方塊實體 `ComparatorBlockActor::mOutput`。客戶端有沒有同步這個值要實測；強度改變時網格不會自動重建，要另外觸發 |
| 壓力板（石、木、輕／重質測重） | `redstone_signal` | 測重壓力板 0–15 都可以分。原版壓力板用的是 `stone`／`gold_block` 等共用材質，規則同時指定方塊和來源材質，所以不會影響到一般金磚 |
| 陽光感測器 | `redstone_signal`；反相是不同方塊 ID | |
| 拉桿、按鈕 | `open_bit`／`button_pressed_bit`、方向 | |
| 觀察者、漏斗、發射器、投擲器 | `powered_bit`／`toggle_bit`／`triggered_bit` | |
| 動力／偵測／觸發鐵軌 | `rail_data_bit` | |
| 絆線鉤、絆線 | `attached_bit`、`powered_bit`、`disarmed_bit` | |
| 銅燈泡、合成器、避雷針、伏聆振測器 | `lit`／`powered_bit`／`crafting`／`sculk_sensor_phase` | |
| 鍊藥鍋 | `fill_level` 0–6、`cauldron_liquid`（水／岩漿／細雪） | 液面在原版是用 `cauldron_water`／`still_water_grey` 這兩個材質名畫的，可以依水位換。藥水顏色存在方塊實體裡（`mPotionId`／`mCustomColor`），想依藥水種類換圖要另外讀方塊實體 |
| 堆肥桶、重生錨、蜂巢、釀造台 | `composter_fill_level`、`respawn_anchor_charge`、`honey_level`、`brewing_stand_slot_*_bit` | |
| 音符盒 | ❌ 音高不在方塊狀態裡（存在 `NoteBlockActor::mNote`） | 跟比較器一樣要讀方塊實體，還要自己觸發重建 |

通則：**存在方塊狀態裡的值都能直接用**，而且會自動更新；**只存在方塊實體裡的值**（比較器強度、音符盒音高、鍊藥鍋藥水顏色）要多做兩件事：讀方塊實體，並在它改變時要求重建那個區段。

同一個材質名可能有多張圖（例如 `repeater_up` 是 `[repeater_off, repeater_on]`），所以規則的來源材質要支援 `repeater_up#1` 這種寫法來指定是哪一張；不寫索引就是全部都換。

### 9.3 紅石線的連接與側邊（爬牆）連接

**基岩版的紅石線只有 `redstone_signal` 一個狀態，沒有連接方向**（Java 版有 `north/east/south/west = none/side/up`）。連接形狀是遊戲在畫的時候檢查鄰居算出來的（`tessellateDustInWorld`，沒有符號）。所以光看方塊狀態分不出連接方式，但從遊戲畫出來的幾何可以分出來：

| 部位 | 怎麼分辨 | 能不能單獨換材質 |
| --- | --- | --- |
| 地面部分 | 四邊形法線朝上 | ✅ |
| 爬牆部分（側邊往上連） | 四邊形是垂直的；法線朝向「離開牆」的方向，所以東邊有牆時法線朝西 | ✅ **可以跟地面分開**，也可以依東南西北各自指定 |
| 十字、T 字、L 字 | 預期遊戲用 `redstone_dust_cross` 並裁切 UV，直線則用 `redstone_dust_line`（這是推測，PoC 時用實際頂點資料確認） | ✅ 整個形狀可以換；因為重映射是按比例對應，裁切會照樣套到新材質上 |
| 十字的每一條手臂各用不同的圖 | 如果地面只有一個四邊形，一個四邊形只能對到一張圖 | ⚠️ 要用方案 D：拿掉原本的地面四邊形，改畫「中心點 + 每個方向一段」的多個四邊形（就是 Java 1.16 之後的畫法）。地面是平的，光照可以直接沿用原四邊形的頂點資料，難度中等 |

**推出 Java 式的連接狀態**：從遊戲畫出的幾何直接讀出來，不重寫紅石連接的判斷邏輯——地面四邊形往哪幾個方向延伸，就表示那幾個方向有連接（`side`）；那個方向有爬牆四邊形就是 `up`。這樣保證跟遊戲畫出來的一模一樣，也能提供 `tessera:north = none/side/up` 這類虛擬狀態給規則用，讓 Java 材質包的寫法可以直接轉過來。

同樣的方法也適用於其他「連接不在狀態裡」的方塊：柵欄、玻璃片、鐵柵欄、絆線（石牆的連接則本來就存在狀態裡）。

### 9.4 規則格式要補的欄位

```json
{
  "block": "minecraft:redstone_wire",
  "when": { "redstone_signal": "1..15", "tessera:east": "up" },
  "part": "climb",
  "facing": ["east"],
  "replace": { "redstone_dust_line": "tessera_dust_climb_p{redstone_signal}" },
  "tint": "none"
}
```

- `part`：`floor`（地面）／`climb`（爬牆）／`any`（預設）。
- `facing`：限定方向（爬牆部分指的是「牆在哪一邊」）。
- `tessera:north/east/south/west`：從幾何推出的虛擬連接狀態，值為 `none`／`side`／`up`。
- 來源材質可寫 `名稱#索引`。
- 讀方塊實體的值（比較器強度等）放在 `be:` 前綴，例如 `"be:output": "8..15"`，等確認客戶端有同步後再加入。

## 附錄：證據來源

- LeviLamina 26.60.0 client SDK 標頭：`mc/client/renderer/block/BlockGraphics.h`、`BlockTessellator.h`、`Tessellator.h`、`chunks/RenderChunkBuilder.h`、`deps/minecraft_renderer/renderer/MeshData.h`、`block/tessellation_pipeline/**`、`ll/api/memory/Hook.h`。
- 原版資源包 `blocks.json`（`redstone_wire`）與 `terrain_texture.json`（`grass_side` 的材質陣列）。
- Arke：https://github.com/LiteLDev/Arke （README、tooth.json）；`Arke-client-windows-x64.zip` v0.1.1 的 `arke.dll` 字串（只做靜態分析，沒有執行）。
- LeviLamina 安卓文件：`docs/main/contents/developer_guides/android/api/signature.md`、`memory_hook_macros.md`。
- LHolo 原始碼：`ProjectionSectionBuilder.cpp` 直接呼叫 `BlockTessellator::tessellateInWorld`，所以方案 C 的投影也會套用新材質。
