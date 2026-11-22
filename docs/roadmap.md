# Tessera 路線圖

目標：讓基岩版材質包能做到 Java 版材質包做得到的事。材質包作者照 Java 的寫法寫（blockstates、models、OptiFine CTM／CIT、1.21.4 物品定義、LabPBR），Tessera 在基岩版原生讀取、渲染。另外提供一個幫作者打包、檢查的工具。

- **不做**的事：轉換別人的 Java 材質包並散布。除了授權問題，轉換也會帶來大量差異；基岩版 UI 也跟 Java 版不同。
- `tools/convert_xekr.py` 只當本機測試素材，用來驗證模組功能。

## 參考資料（2026-11-11 盤點）

| 來源 | 用到的功能 | 對 Tessera 的意義 |
|---|---|---|
| XeKr 紅石顯示 | blockstates + 模型疊加數字 | 狀態材質、模型烘焙（已驗證） |
| 梧桐：加减法 Re（26.1.1） | 195 個 blockstates、511 個方塊模型、142 個 1.21.4 物品定義（`condition:component`、`select:display_context`、`range_dispatch:use_duration`、`select:block_state`…）、653 個物品模型 | 方塊狀態＋物品定義兩條路都要支援 |
| 零雾 Fogg05（Java） | OptiFine CTM（無縫玻璃、銅格柵、紅石燈、礦物描邊、草方塊側面）、LabPBR `_n`／`_s`、3D 模型（門、工作方塊、常規）、樹葉枯萎顯示、音符盒資訊、流體方向 | CTM、PBR、3D、計算狀態 |
| 零雾 Fogg05（基岩版 mcpack） | 1892 個 `texture_set.json`（官方光追／灵动视效 PBR） | 基岩版 PBR 的格式就是 texture_set；替換材質時要一起換 PBR |
| Lamium Connected Textures | `TextureAtlas::_createAtlas` 動態加 sprite；`ResourcePackManager::load` 提供記憶體裡的圖；`RenderChunkCoordinator::tick` → `_setAllDirty` 重建區塊 | 不靠 terrain_texture.json 加材質、狀態變了就重建區塊，兩個關鍵技術 |

## 架構分層

### 1. 狀態來源 API（讓「基岩版沒有的狀態」也能用）
- ✅ 方塊自身狀態（`redstone_signal`、`triggered_bit`…）
- ✅ 計算狀態，從鄰近方塊算出來：`tessera:extended`（活塞）
- ⏳ **方塊實體狀態**：讀方塊實體（BlockActor）的資料，例如：
  - `tessera:note`、`tessera:instrument`（音符盒；樂器看下方方塊）
  - `tessera:has_record`（唱片機）
  - `tessera:output`（比較器輸出強度）
  - `tessera:power`（標靶、幽匿感測體）
  - 讀取方式：tick 執行緒定期拍快照存進位置表，渲染執行緒只讀快照（render-thread 安全規則）。值有變化就把該區塊標成需要重建。
- ✅ **遠距計算狀態**：`tessera:distance`（樹葉離原木的距離，用來做 Java 的枯萎顯示）。依 Bedrock 的規則掃 4 格以內（實測：離原木 5 格以上的樹葉會枯萎；Java 是 6 格），超出渲染區域的邊框，所以在用戶端主執行緒算好放快照。樹葉的 `update_bit` 不拿來選模型。
- ✅ **blockstate 跨材質包合併**：同一個 blockstates 檔在多個包都有時，照 Java 的做法逐狀態合併（最上層有涵蓋該狀態的包決定；multipart 檔涵蓋所有狀態）。
- ⏳ **給其他模組的 C++ API**：`include/Tessera/Api.h`，提供 `registerStateProvider("mymod:foo", fn)`、`markDirty(pos)`。其他模組註冊的狀態，材質包也能直接寫進規則。

### 2. 方塊外觀
- ✅ 依來源材質換材質、限定面（`on`）
- ✅ 萬用來源 `"*"`／`"*:up"`：按面在方塊裡的位置貼材質，跟 Java 模型的預設 UV 一致
- ⏳ **原生讀 Java 格式**：材質包直接放 `assets/minecraft/blockstates|models|textures`。Tessera 載入時用 C++ 版的「模型投影烘焙」（現在 Python 轉換腳本裡那套）把每個狀態每個面烘成 sprite，透過 `_createAtlas` 注入圖集。作者不需要寫 terrain_texture.json 或 state_textures.json。
- ⏳ **連接材質**：支援 OptiFine CTM 格式（`method=ctm|horizontal|vertical|top|random|repeat|fixed|overlay`、`matchBlocks`／`matchTiles`、`connect`、`faces`），sprite 同樣在建圖集時烘好。
- ⏳ **3D 模型 API**：Java 模型的 elements，包含旋轉、cullface、tintindex、shade，直接產生成 Tessellator 的四邊形。純平面能烘焙的走烘焙，有立體結構的走這條。
  - 要處理：光照／AO（沿用原版頂點光照）、Vibrant Visuals 的 PBR 索引、跟 Arke 的順序（我們在 Lowest 先寫入）。
- ⏳ **PBR**：替換材質時一起帶目標的 texture_set（MERS＋法線）；LabPBR `_n`／`_s` 轉 texture_set 交給工具處理。

### 3. 物品
- ⏳ Java 1.21.4 物品定義（`select`／`condition`／`range_dispatch`，依元件、附魔、耐久、顯示情境等條件）＋ OptiFine CIT。
- ⏳ 要先研究基岩版物品圖示和手持模型的渲染入口，再決定掛哪個函式。

### 3.5 遊戲內熱切換材質包（像 Java 版）
- 目標：在世界裡直接開關、調整材質包順序，不用退回主選單。
- 2026-11-12 搜尋結果：GitHub 上沒找到已公開、做到這件事的 Levi 社群模組。找到的相關程式只有 LiteLDev/LeviSchematic 的 `TickHook.cpp`，它是掛資源重載事件來重置自己的狀態，不是主動觸發重載。
- SDK 裡有需要的函式（LeviLamina 26.60 標頭）：
  - `ResourcePackManager::setStack(stack, type)`：換掉某一層的材質包堆疊。
  - `MinecraftGame::_onActiveResourcePacksChanged(mgr, fullVanillaPackOnStack)`：遊戲在設定頁套用全域資源時走的路徑。
  - `MinecraftGame::_beginReloadingResources()`／`_finishReloadingResources(...)`、`cleanReloadMaterials()`。
  - `ResourcePackRepository::requestReloadUserPacks()`：重新掃描磁碟上的材質包（給「改完檔案馬上看結果」用）。
- 2026-11-12 補充：使用者指出的模組是 **BetterResourcePackManager**（作者 DreamGuXiang，發布於 QYCottage/BetterResourcePackManager-Release，只有成品沒有原始碼）。另有 gamelist1990/BedrockDynamicSDK 的同名範例（`exsample/BetterResourcePackManager`、`docs/resource-packs.md`，該倉庫沒有授權條款，只能參考思路，不能複製程式碼）。它的思路：
  - 不自己做介面，而是**解鎖遊戲原本的「全域資源」畫面，讓它在世界裡也能套用**。
  - 套用走遊戲自己的流程：選取變更 → 原生套用回呼 → 堆疊合成 → 圖集完成 → 載入器完成並過一個畫面 → 更新方塊繪製定義、UV、模型參照 → 更新攝影機。
  - 不做全域重新初始化；等圖集和載入器真的完成才更新畫面，中途又開始載入就重試。
  - 它用 16 個掛鉤，依遊戲版本寫死位址；我們要改用 LeviLamina 的符號重做。
- 2026-11-14 盤點 LeviLamina 26.60 的符號：設定畫面相關的類別（`GeneralSettingsScreenController`、`ResourcePacksScreenController`、`ContentManager`）全部標成不可用，所以「解鎖原生全域資源畫面」這條路目前要靠特徵碼掃描，先不走。可用的有：
  - `MinecraftGame::endFrame`（主執行緒每幀）、`_onActiveResourcePacksChanged`、`_beginReloadingResources`、`_finishReloadingResources`、`cleanReloadMaterials`。
  - `ResourcePackManager::setStack`、`_composeFullStack`；`ResourcePackStack` 可用「識別碼清單＋倉庫」建構，也可用 `deserialize(檔案內容, 倉庫, levelId)` 從 `global_resource_packs.json` 的內容建出來。
  - `RenderChunkCoordinator::_setAllDirty`。
  - 虛擬函式不需要符號：`MinecraftGame::getResourcePackManager()`、`getResourcePackRepository()`、`onActiveResourcePacksChanged(mgr)`。
- **第一步（2026-11-14 完成，遊戲內測過）**：世界內重新載入，等同 Java 的 F3+T，按鍵是 `F3+R`（T 會開聊天）。流程：
  1. `MinecraftGame::endFrame` 掛鉤偵測按鍵，直接呼叫 `_onActiveResourcePacksChanged(mgr, true)`。走通知（`onActiveResourcePacksChanged`）不會重載。
  2. 遊戲重建地形圖集（約 2 秒）。`TextureAtlas::_createAtlas` 掛鉤包住完成回呼，得知圖集建完。
  3. 世界開著時遊戲不會重查方塊材質，所以模組自己呼叫 `BlockGraphics::reloadBlockTextures`，接著重讀規則、`_setAllDirty` 重建所有區塊。
  4. 補回從記憶體上傳的貼圖（`TextureGroup::uploadTexture` 掛鉤留副本，重載後 `updateTextureInPlace`）。
  5. 玩家外觀：貼圖還在，但實體定義重讀後外觀的模型與繪製設定失效，玩家會消失。把每個 `ClientSkin` 標成 `mInitialized=false`、外觀資料標成 `mIsDirty=true`，遊戲就會重做一次。重載後 2 秒和 6 秒各做一遍。
- **第二步（2026-11-14 擱置）**：`F3+P` 材質包選單（`src/game/PackMenu.cpp`）。選單本身可用，但世界內換順序不生效。
  - 選單做法（可用）：伺服器表單格式，由客戶端自己顯示、自己收答案。把 `ModalFormRequestPacket` 交給客戶端的 `ClientNetworkHandler::handle`，答案在 `LoopbackPacketSender::send`／`sendToServer` 攔下。一個視窗：每個材質包一個開關加一條順序滑桿。回答裡標籤也佔一格（`null`）。
  - 卡住的地方：`setStack(…, Global)` 回傳成功，`iteratePacks` 看到新清單，但 `load()` 仍提供舊的組合結果；`mPendingRestack` 一直是 true。試過 `_composeFullStack()`，也試過暫時清掉 `mGameplayResourcesLoaded`／`mInitializing`／`mPendingRestack` 再合成，都沒變。實際提供內容的是 `ResourcePackLoader` 裡沒有名字的欄位，大概是進世界時準備好的。
  - 寫入 `global_resource_packs.json` 有效，下次開遊戲會用新順序。
  - 等 BetterResourcePackManager 作者回覆，或之後拆解遊戲本體再繼續。
- 做法草案：模組內提供清單介面（或指令／快捷鍵）→ 改全域堆疊 → 呼叫上面那條套用路徑 → Tessera 既有的 `reloadBlockTextures` 掛鉤會重讀規則。
- 風險：在世界內重載會不會卡住或崩潰，要實測；伺服器強制的材質包不能動。

### 4. 工具（材質包製作）
- 檢查材質包：狀態名稱對不對、材質有沒有缺，以及哪些 Java 功能在基岩版無法對應。
- 預覽：在遊戲外依狀態看結果。
- 打包：產生 mcpack；LabPBR 轉 texture_set；mcmeta 動畫轉 flipbook。
- Android 版還沒有模組時，把 Java 寫法預先烘成 state_textures.json 當備案。

### 5. 平台
- Windows 先做；Android `.so` 之後做。`src/core` 保持不依賴遊戲標頭。

## 建議順序

| 階段 | 內容 | 狀態 |
|---|---|---|
| P0 | 紅石線方向（改用俯視烘焙＋位置貼圖）、投擲器／發射器六面獨立 | 已完成，待遊戲內測試 |
| P1 | 方塊實體狀態框架＋區塊重建（音符盒、唱片機、比較器輸出）＋對外 C++ API（見 `docs/api.md`）；尚缺標靶、幽匿感測體、樹葉 distance | 前半完成，待遊戲內測試 |
| P1.5 | 遊戲內熱切換材質包（見 3.5） | `F3+R` 世界內重載完成；世界內換順序擱置（見 3.5） |
| P2 原型 | `tools/convert_java_pack.py`：通用 Java 材質包轉換（Chunker 的 Java→基岩版狀態表＋模型烘焙＋旋轉規則），等於原生載入器的離線版 | 已完成，待遊戲內測試 |
| P2 | 原生讀 Java 方塊格式（圖集注入＋C++ 烘焙），之後就不需要轉換 | 2026-11-14 第一版完成，待遊戲內測試（見下方 P2 說明） |
| P3 | OptiFine CTM 連接材質 | 2026-11-14 第一版：ctm_compact／fixed／random／repeat／top，待遊戲內測試（見下方 P3 說明） |
| P4 | 物品（1.21.4 物品定義、CIT） | OptiFine CIT（type=item）：手持、物品欄、容器、游標、方塊物品、未公開類別都已完成並測試；1.21.4 物品定義第一版完成並測試（見下方 P4 說明） |
| P5 | 3D 模型幾何 | 2026-11-15 第一版：Java 方塊模型的元素畫成真的立體（見下方 P5 說明） |
| P6 | PBR／Vibrant Visuals、Arke 光影相容 | |
| P7 | 製作工具 | 2026-11-22：`tessera-tool` init（在基岩版材質包加 Java 資料夾）、check（HTML 檢查報告）、preview（方塊依狀態、連接材質範例牆、物品圖示）、pack（.mcpack）；英文／簡體／繁體。說明見 `docs/guide/`。之後考慮自己的繪製工具 |

為什麼 P1 先做：範圍小，而且直接解決「基岩版做不到」的那幾項。P2 是後面 CTM、物品、3D 的共同基礎，排在第二。

## P2 原生讀 Java 格式（2026-11-14）

- 材質包作者把 Java 版的 `assets/minecraft/blockstates|models|textures` 原樣放進一個基岩版材質包（有 manifest.json 即可），不用寫 terrain_texture.json 或 state_textures.json。
- `src/core/java/`：`tools/convert_java_pack.py` 的 C++ 版（不依賴遊戲標頭）。`tools/bake_test`（`xmake build bake_test`）在遊戲外跑，`tools/compare_bake.py` 跟 Python 版逐張比對：RedstoneTuning 38226 張相同、44 張（轉彎鐵軌邊緣）差幾個像素。烘焙本身約 2 秒。
- `src/game/JavaPacks.cpp`：建地形圖集時（`TextureAtlas::_createAtlas`）從整個材質包堆疊讀 Java 檔案、烘焙，把圖加進 `texture_data`；圖由 `ResourcePackLoader::$getPackStackIndexOfResource` 和 `$load`（ResourceLocationPair 版本）兩個掛鉤直接提供。產生的規則接在材質包自己的 `state_textures.json` 規則後面。
- 遊戲裡沒有 Java 原版檔案，所以：
  - 材質包常繼承卻不附的原版模型，由 Tessera 內建替代品（`src/core/java/Builtin.cpp`）：立方體類模板（block、cube、cube_all、cube_column、orientable…）、各種按鈕（依材質名產生）、蜂箱與蜂巢。依原版的形狀和用法自己寫，不是複製；遊戲外比對過，烘出來的圖跟用原版檔時相同。
  - 材質包沒附的原版材質，改用同名（或對照表裡）的基岩版材質；直排多格的動畫材質取第一格。找不到的記在 log：`textures found nowhere`。
  - 已知限制：材質包只換貼圖或模型、沒附方塊狀態檔的方塊不會生效（例：混凝土、黑曜石、銅燈、竹書架），因為要原版狀態檔才知道狀態怎麼對應模型。
- 載入速度：方塊狀態檔直接從各材質包列出（`ResourcePack::forEachIn`）。烘焙結果存到模組資料夾的 `java_bake.cache`，連同它讀過的每個檔案的指紋；下次建圖集時只重讀這些檔案比對，沒變就直接用。
- Java→基岩版方塊狀態表：Chunker（MIT），見 `third_party/chunker/NOTICE.md`。

## P3 連接材質（OptiFine CTM 格式，2026-11-14 第一版）

- 材質包作者照 Java 寫法放 `assets/minecraft/optifine/ctm/**.properties`（OptiFine／Continuity 的格式，規格見 OptiFine 文件 `ctm.properties`），Tessera 直接讀。
- `src/core/ctm/`：解析設定檔（method、tiles、matchTiles、matchBlocks、connect、faces、weight、innerSeams、width/height、weights、symmetry、randomLoops、heights、ctm.N、connectBlocks/connectTiles…），以及 ctm_compact 四分之一面的選圖、random、repeat。
- `src/game/Ctm.cpp`：
  - 建地形圖集時列出所有材質包的設定檔，圖塊原樣放進圖集（不重新烘焙）。
  - 方塊材質載入後，找出圖塊和 matchTiles 在圖集的位置（Java 材質名換成基岩版名，例：`black_stained_glass` → `glass_black`），matchBlocks 透過 Chunker 表換成基岩版方塊名。
  - 繪製每個方塊時（所有方塊都會經過），對每個平放的面：依材質（matchTiles）或方塊名（matchBlocks）找規則，看面上 8 個方向的鄰居是否「相同」（connect=block 比方塊名、tile 比該面材質、state 比完整狀態；innerSeams=false 時鄰居前面被擋住就不算），選圖塊、依位置換 UV。
  - ctm_compact 的四個角不同時，把一個面切成四塊：原本的四邊形改成第一塊，其餘用 `Tessellator::vertex` 補上（位置、光照 uv、顏色依四角內插）。
- 方向：面上的「右」「上」跟 Java 預設 UV 一致（上面：右=東、上=北；下面：右=東、上=南；北面：右=西；南面：右=東；西面：右=南；東面：右=北；側面上=上）。
- 2026-11-14 第二版：
  - `overlay`／`overlay_fixed`／`overlay_random`／`overlay_repeat`：在面上多畫一層（略微往外，避免重疊閃爍）。17 格的意思從零雾的自然連接圖讀出：15 上緣、1 下緣、9 左緣、7 右緣；11 上左、10 上右、4 下左、3 下右；13 缺下、5 缺上、6 缺右、12 缺左；8 四邊；16 左上角、14 右上角、2 左下角、0 右下角（兩邊都沒連時才畫）。上下或左右兩邊同時連、但另外兩邊沒連時，畫兩張。
  - 有 overlay 的方塊改到 alpha 測試的繪製層（`BlockType::mRenderLayer`，原本是不透明層才改），透明像素才不會蓋掉底圖；改過會要求重建區塊，規則重載時改回。
  - `matchTiles` 也能指到材質包自己模型用的材質：P2 烘焙時記下每張圖用了哪些 Java 材質（快取格式改為 2）。
  - `connect=tile` 時，同名方塊也算相連（P2 烘過的面跟遊戲記的面材質不同）。
- 玻璃片、鐵柵欄上下疊同一種時，Java 會隱藏中間的上下邊條，基岩版不會；有連接材質時，Tessera 把這些邊條縮成一點（不畫）。
- 只用方塊名字對應的 Java 材質（例：`oak_log`、`furnace_side`），找不到基岩版同名材質時，改從基岩版方塊那一面實際用的材質去找。
- 還沒做：`ctm`（47 格）、`horizontal`／`vertical`（及兩者組合）、`overlay_ctm` 需要官方範本確認每格的意義；biomes；matchBlocks 的狀態條件目前只用來挑方塊名。

## P4 物品材質（2026-11-15 第一版）

- 格式：OptiFine CIT，`assets/minecraft/optifine/cit/**.properties`（也讀 `mcpatcher/cit`），`type=item`。
  - 支援：`items`、`texture`（同資料夾、`./`、或 `item/x` 路徑）、`model`（Java 物品模型：`item/generated` 的 layer0、layer1… 疊起來；有 elements 的模型取正面）、`damage`（含百分比）、`stackSize`、`enchantmentIDs`／`enchantmentLevels`、`nbt.display.Name`／`components.custom_name`（exact、pattern、ipattern、regex、iregex）、`weight`。
  - 已解析但還不能用：藥水種類（`nbt.Potion`、`components.potion_contents.potion`），因為基岩版的藥水類別自己決定圖示，那個函式沒有公開。
  - 其他 `nbt.*`／`components.*` 條件會記在 log：`not supported yet (ignored)`。
- 做法：建物品圖集（`atlas.items`）時讀 CIT、畫好圖示放進圖集；掛 `Item::$getIconInfo`、`BlockItem::$getIconInfo`、`HumanoidArmorItem::$getIconInfo`，依物品的名稱、耐久、數量、附魔挑規則，換掉圖示名稱。
- 自己覆寫圖示函式、而函式沒公開的物品類別（藥水、弓、羅盤、時鐘、箭、雪球、資料驅動的 ComponentItem 等）：建圖集時，對 CIT 規則提到的物品讀虛擬函式表的圖示欄位（偏移 0x3a8，取自遊戲自己的虛擬呼叫），直接掛那個實作（最多 48 個）。
- 物品欄格子（2026-11-15）：格子只留物品 ID 和資料值的空白副本，看不到名稱和附魔。現在依格子的 `#collection_name`／`#collection_index` 換回真正的物品：玩家快捷欄、背包、副手、游標上的物品，以及其他畫面（箱子等）經 `ContainerScreenController::_getVisualItemStack` 記下的物品。
- 方塊物品和有自己圖示圖集的物品（羅盤、時鐘）：格子建立前，把它的 `#item_id_aux` 暫時換成替身平面物品（鑽石劍），格子就會照平面物品設定，再由圖示函式回傳 CIT 圖示。
- Java 1.21.4 物品定義（`assets/minecraft/items/*.json`，2026-11-15）：`model`、`composite`、`condition`、`select`、`range_dispatch`、`empty`。
  - 條件：`damaged`、`broken`、`has_component`（custom_name、enchantments、damage）；選擇：`display_context`（物品欄 gui、手上 firstperson_righthand）、`main_hand`、`component` custom_name；範圍：`count`、`damage`（含 normalize、scale）。
  - 其他屬性（custom_model_data、compass、time、trim_material、using_item…）先當成「不成立／0／走 fallback」。`special`（箱子、頭顱、盾牌…）和 `tints` 還沒做：前者保留遊戲圖示，後者不上色。
  - 建圖集時列出每棵樹所有可能的模型組合，各畫一張圖示（composite 疊圖）；模型不在包裡時，`item/x` 會改用包裡的 `textures/item/x.png`，都沒有就保留遊戲圖示。
  - 同一物品同時有 CIT 規則和物品定義時，CIT 優先。
  - 物品定義只換平面物品模型（layer 疊圖）的圖示；3D 模型（方塊物品、有 elements 的物品）保留遊戲自己的立體圖示，因為壓平的正面比原版差（梧桐加减法測試時發現）。

### 2026-11-17 補充（物品）

- **原版 Java 物品定義**：材質包只換了原版物品的貼圖或模型（沒有自己的 `items/*.json`）時，從玩家的 Java 用戶端 jar 讀原版定義來套用。只畫材質包真的有動到的結果。
- **依物品資料挑模型**：`select` 的 `potion_contents`、`suspicious_stew_effects`，`condition` 的附魔 predicate（`enchantments` / `stored_enchantments`，含等級），藥水 tint。巢狀深度不設 32 層上限（梧桐的附魔書是一百多層的鏈）。
- **方塊物品走遊戲原生的 3D**：遊戲用方塊網格畫的物品（render type 0）不再換成圖片。Java 物品模型的形狀透過 `"item": true` 的規則餵給遊戲：手上走 `BlockTessellator::appendTessellatedBlock`，物品欄格子走 `renderGuiBlock`（格子裡的頂點已經轉到螢幕空間、沒有深度緩衝，所以只畫朝向觀看者的面、由遠到近）。
- **方塊但顯示成平面圖示的物品**（燈籠、紫水晶簇，render type 1）：手上改成真的 3D（`getRenderTypeFromItem` 回 0）；格子裡仍是照 Java 物品欄視角畫的圖。
- **依位置隨機挑模型**：blockstate 的加權 variants 用 `"pick": [lo, hi, total]` 規則，位置種子跟 Java 一樣。超出方塊邊界的面再依位置奇偶沿法線微移 0.002 格，避免跟鄰格同一個面重疊。
- **流體**：Java 的 `water_still`、`water_flow`、`lava_still`、`lava_flow` 貼圖在遊戲讀 Bedrock 對應貼圖時直接換上。流動貼圖要轉成 Bedrock 的排法（一格圖案 2x2 重複，圖案是 Java 影格中間那塊、左右上下對調）。

## P5 3D 模型幾何（2026-11-15 第一版）

- 烘焙（P2）時，模型不是「整塊方塊」的狀態（元素不是 0–16 立方體、或有旋轉），改輸出 `geometry`：每個元素的每一面變成一個四邊形（方塊座標 0–1、已套 blockstate x/y 旋轉、元素旋轉含 rescale），附自己的貼圖（另放進地形圖集）和 UV、面向、`cullface`、`shade`。
- 遊戲裡：該方塊原本的面收成一個點不畫，改畫這些四邊形；光照和顏色取原本同方向那幾面的平均（沒有同方向的就取全部平均）。`cullface` 那側是實心方塊時不畫。混合圖層（鐵軌等，會依距離排序）的方塊，以及不透明但貼圖有透明像素的方塊，改到 alpha 測試圖層，否則重疊的部件會互相透出來；有方塊實體的方塊（煉藥鍋等）不改，改了會整個不畫。
- 排除：靠鄰居連接決定形狀、但基岩版沒有對應狀態的方塊（樓梯、柵欄、牆、玻璃片、鐵柵欄、紅石線、藤蔓等），仍走 P2 的貼圖方式。
- 工具：`tools/preview_geometry.py` 從 `bake_test` 的輸出畫出等角預覽，不必進遊戲就能看形狀和 UV。
- 已在遊戲內（Be.D.D.）驗證：RedstoneTuning 的拉桿、中繼器、比較器、漏斗、鐵軌（含動力、偵測鐵軌）、按鈕、音符盒、偵測器、煉藥鍋。
- 還沒做：`uvlock`、`tintindex` 的分別上色（目前整塊用原本面的顏色）、環境光遮蔽逐頂點計算、方塊實體（箱子等）。

## 原版 Java 資源（2026-11-15）

- Tessera 會讀玩家自己電腦上的 Java 版客戶端 jar（`%APPDATA%\.minecraftersions` 裡最新的版本，`data/java_vanilla.txt` 可指定路徑或寫 `off` 關閉），當作原版模型、方塊狀態、貼圖的來源；不會散布任何原版檔案。
- 效果：只改模型或貼圖、沒附方塊狀態的包（門、繁茂樹葉、玻璃模型等）也能套用；材質包用到的原版父模型（`block/leaves`、作物、`chiseled_bookshelf`…）不再缺。
- 烘焙結果的快取會把 jar 的大小和時間算進去，換版本會重新烘焙。
- 鐵柵欄類、柵欄、牆：形狀靠鄰居、基岩版沒有對應狀態，P2 不處理，保留基岩版的網格和貼圖（連接材質照常作用）。
- 玻璃片（2026-11-22 起）：Tessera 自己算四邊的相連（`tessera:pane_north` 等，相鄰的是玻璃片、鐵柵欄、牆、玻璃或實心方塊就算相連），照 Java 方塊狀態挑模型；連接材質也套在模型面上（Fogg05 完美玻璃）。
- 測試：零雾〇五 Fogg05 全套（19 個包，`local/Fogg05_*_Java`，僅本機測試）在 Be.D.D. 裡：1154 種方塊、11089 條規則、首次烘焙約 32 秒，之後用快取；畫面約 110–120 FPS。
