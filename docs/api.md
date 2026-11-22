# Tessera 狀態 API

材質包規則（`state_textures.json`）裡，`when` 條件和 `{...}` 模板除了方塊自己的狀態，還能用下面這些「計算狀態」。

## 內建計算狀態

| 鍵 | 方塊 | 值 | 來源 |
|---|---|---|---|
| `tessera:extended` | `piston`、`sticky_piston` | `true`／`false` | 前方有同朝向的活塞臂（等同 Java 的 `extended`） |
| `tessera:powered` | 任何方塊 | `true`／`false` | 從周圍的紅石零件推算（見下方說明） |
| `tessera:power` | 任何方塊 | `0`–`15` | 同上，推算出的最大訊號強度 |
| `tessera:note` | `noteblock` | `0`–`24` | 音符盒方塊實體 |
| `tessera:instrument` | `noteblock` | Java 名稱：`harp`、`basedrum`、`snare`、`hat`、`bass`、`flute`、`bell`、`guitar`、`chime`、`xylophone`、`iron_xylophone`、`cow_bell`、`didgeridoo`、`bit`、`banjo`、`pling`、`zombie`、`skeleton`、`creeper`、`dragon`、`wither_skeleton`、`piglin`；基岩版特有的 `trumpet`、`trumpet_exposed`、`trumpet_weathered`、`trumpet_oxidized`；`none` | 依下方方塊判定，與遊戲相同 |
| `tessera:has_record` | `jukebox` | `true`／`false` | 唱片機方塊實體 |
| `tessera:output` | `powered_comparator`、`unpowered_comparator` | `0`–`15` | 比較器方塊實體的輸出強度 |
| `tessera:distance` | 各種樹葉 | `1`–`4`、`7` | 經由樹葉走到最近原木的步數，寫法跟 Java 的 `distance` 一樣（7＝太遠，會枯萎），但用 Bedrock 的規則：原木在 4 步以內才算（Java 是 6 步），所以只會出現 1–4 和 7。用戶端主執行緒算好放進快照，周圍改變後會在輪詢時更新 |
| `tessera:signal` | 任何方塊（給標靶、幽匿感測體用） | `0`–`15` | 訊號源輸出的強度，從旁邊紅石線的強度推算；旁邊沒有紅石線時是 0 |
| `tessera:redstone_north`／`_east`／`_south`／`_west` | `redstone_wire` | `none`／`side`／`up` | 照 Java 的 RedStoneWireBlock 從鄰居算出的連接（單向會延伸成直線、孤立時是十字）。轉換 Java 材質包時會自動對應到 `north`… |
| `tessera:tripwire_north`／`_east`／`_south`／`_west` | `trip_wire` | `true`／`false` | 照 Java 的 TripWireBlock：旁邊是絆線，或是朝向這條線的絆線鉤 |

範例：
```json
{ "block": "noteblock", "when": { "tessera:instrument": "bell" },
  "replace": { "noteblock": "my_note_bell_{tessera:note}" } }
```

**運作方式**
- 方塊實體只在用戶端主執行緒讀取：每個 tick 輪詢最多 256 個位置。
- 讀到的值存成快照；畫區塊的執行緒只讀快照，不碰活的方塊實體。
- 值一變，就重建該格所在的區塊。
- 第一次畫到某個位置時，資料還沒讀進來，會先用預設值（`note=0`、`harp`、`false`、`0`）。讀到後若不同，再重建一次。

## `tessera:powered`／`tessera:power` 的推算方式

基岩版把「方塊有沒有被充能」放在伺服器的紅石電路系統裡，用戶端看不到，所以這兩個狀態是從周圍方塊的狀態推算的：

- **緊鄰的訊號源**：紅石磚、亮著的紅石火把（不算它插著的那個方塊）、拉下的拉桿、按下的按鈕、被踩的壓力板、指向這格或鋪在這格上方的紅石粉、同一直線上亮著的中繼器／比較器／偵測器、陽光感測器、絆線鉤、講台、避雷針、偵測鐵軌、觸發中的幽匿感測體。
- **隔一個實心方塊**：旁邊的實心方塊若被充能（紅石粉鋪在上面或指向它、火把在它下方、壓力板在它上面、拉桿／按鈕／中繼器／比較器／偵測器接著它），也算充能。

這是推算，不是遊戲的實際結果。已知的落差：
- 中繼器、比較器、偵測器只判斷「在同一直線上」，沒分輸入端和輸出端。
- 拉桿、按鈕沒判斷是貼在哪個方塊上。
- 只看一層實心方塊，更遠的傳導不算。
- 充能來源在另一個區塊邊界外時，畫面可能晚一點才更新。

## 萬用來源

`replace` 的鍵寫 `"*"` 會套到所有面，寫 `"*:up"`、`"*:down"`、`"*:north"`、`"*:south"`、`"*:east"`、`"*:west"`、`"*:side"` 則只套到指定朝向的面。

這種寫法不看原本是哪張材質，直接依該面在方塊裡的位置貼上新材質，排法跟 Java 模型的預設 UV 相同。

適合的情況：
- 依 Java 模型烘焙出來的整面圖。
- 基岩版的 UV 排法跟 Java 不同的方塊（例如紅石線）。

**面的朝向和深度**
- 面朝哪邊是從頂點的繞序算出來的，所以漏斗、堆肥桶這類中空方塊的內壁，會被當成「朝內」的面，不會和外壁混在一起。
- 鍵後面可以加 `@位置`，只套用到那個深度的面，例如 `"*:east@0.125"` 是「朝東、位在 x=2/16 的面」（位置是模型座標，0–1，容許誤差約 0.035）。沒有對到任何 `@位置` 的面，才用不帶位置的那一個鍵。
- 斜的面（例如拉桿的把手）不會被萬用來源替換，保留原本的材質。

## 有朝向的方塊：`rotation`

做法跟 Java 方塊狀態一樣：材質只依「朝北的模型」準備一套，每個朝向的規則加上 `rotation`（x 先轉、y 後轉，單位是 90°）。`"*:north"` 等鍵代表的是**模型**的面，模組會依旋轉把材質貼到對應的世界面上，面內的方向也會跟著轉。

```json
{ "block": "dropper", "when": { "facing_direction": 5, "triggered_bit": true },
  "rotation": { "y": 90 },
  "replace": { "*:north": "dropper_front_on", "*:south": "dropper_back_on",
               "*:east": "dropper_side_on", "*:west": "dropper_side_on",
               "*:up": "dropper_top_on", "*:down": "dropper_bottom_on" } }
```

`rotation` 只作用在萬用來源（`"*"` 開頭的鍵）上。用材質名稱比對的規則，照基岩版原本的 UV 貼。

## 給其他模組：`include/Tessera/Api.h`

其他模組可以在任何位置放自訂狀態，材質包規則就能直接用：

```cpp
#include "Tessera/Api.h"

tessera::api::Client tessera;               // 不需要連結 Tessera；沒裝時每個呼叫都回傳 false
tessera.setInt(0, x, y, z, "mymod:charge", 3);   // 維度：0 主世界、1 地獄、2 終界
tessera.setBool(0, x, y, z, "mymod:active", true);
tessera.setString(0, x, y, z, "mymod:mode", "fast");
tessera.clear(0, x, y, z, "mymod:charge");
```

```json
{ "block": "mymod:battery", "when": { "mymod:charge": "1..4" },
  "replace": { "battery_side": "battery_side_{mymod:charge}" } }
```

- 所有呼叫都是執行緒安全的。值寫進快照後，下一個用戶端 tick 會重建受影響的區塊。
- 鍵必須帶命名空間；`tessera:` 和 `minecraft:` 保留不能用。
- 底層是 C 匯出函式：`tessera_api_version`、`tessera_set_state_int`、`tessera_set_state_bool`、`tessera_set_state_string`、`tessera_clear_state`。其他語言也能用 `GetProcAddress` 呼叫。

- 換世界時，所有狀態（輪詢到的和其他模組推進來的）都會清掉（2026-11-15 起）。

### 遊戲內驗證（2026-11-15 通過）

- `F3+K`：用公開標頭檔的 `tessera::api::Client`（跟外部模組一樣，執行時找 `Tessera.dll` 取函式），對腳下方塊依序設定 `tesseratest:level` 1、2、3，第四下清除。
- 測試材質包 `packs/TesseraApiTest`：草方塊、泥土、石頭等方塊依值換成金磚、綠寶石磚、紅石磚材質。
- 結果：每次按鍵後約 20 毫秒內區塊重畫，規則依新值換材質；清除後恢復原樣。

## 給其他模組：計算函式（API 2，2026-11-15）

除了把值推進來，模組也可以註冊一個函式，讓 Tessera 在畫到「規則有用到這個鍵」的方塊時來問：

```cpp
bool charge(int dim, int x, int y, int z, char const* block, void* user, tessera_value* out) {
    out->kind = TESSERA_VALUE_INT;
    out->i    = lookUpMyCharge(dim, x, y, z); // 自己的資料，不能碰遊戲的活物件
    return true;                              // false = 這格沒有值
}
tessera::api::Client tessera;
tessera.registerProvider("mymod:charge", &charge);
tessera.markDirty(dim, x, y, z);              // 某格的答案變了，要求重畫
```

- 函式在畫區塊的執行緒上被呼叫：必須執行緒安全、要快，而且不能讀遊戲的活物件。
- `tessera_value`：`kind` 是 `TESSERA_VALUE_INT`／`BOOL`／`STRING`；字串放在 `s`（最多 63 字元）。
- 匯出函式：`tessera_register_provider`、`tessera_unregister_provider`、`tessera_mark_dirty`；`tessera_api_version` 回傳 2。舊的標頭檔（版本 1）照常可用。
- `F3+K` 測試會註冊 `tesseratest:parity`（依座標的棋盤格），測試材質包讓白色混凝土依它變成金磚材質。

## 給其他模組：物品 3D 模型（API 3，2026-11-22）

材質包給物品的 3D 模型（有 elements 的 Java 物品模型，例如 Fogg05 的藥水瓶）可以拿去自己畫，例如掉落物物理、展示架類的模組：

```cpp
tessera::api::Client tessera;
std::vector<tessera_quad> quads(4096);
int n = tessera.itemModel(&itemStack, quads.data(), int(quads.size())); // itemStack: 遊戲的 ItemStack
if (n > int(quads.size())) { quads.resize(n); n = tessera.itemModel(&itemStack, quads.data(), n); }
// n == 0：沒有 3D 模型（平面物品），用遊戲自己的樣子
```

- 每個 `tessera_quad`：`pos` 4 個角（xyz）、`normal`、`color`（RGBA，R 在最低位元組）。四邊形依貼圖像素切開，跟 Bedrock 自己的手持物品一樣，每塊一個顏色（已含染色，例如藥水顏色）。
- 座標是模型自己的空間：方塊單位（0–1 ＝ 一格）、Java 的軸向（x 東、y 上、z 南），已套用元素旋轉，沒有套 display（拿法自行決定）。角的順序：`cross(p1 - p0, p3 - p0)` 朝外。
- 只讀快照，任何執行緒都可以呼叫。匯出函式 `tessera_item_model`；`tessera_api_version` 回傳 3。
- 測試：`mods/Tessera/data/api_test.txt` 存在時，手上物品一換就在聊天室回報 API 拿到的四邊形數（`F3+K` 狀態測試也只在這時啟用）。

## 尚未支援

| 項目 | 原因 |
|---|---|
| 標靶、幽匿感測體的「真實」強度 | 基岩版用戶端沒有存；`tessera:signal` 是從旁邊紅石線推算的，旁邊沒有紅石線就不知道 |
