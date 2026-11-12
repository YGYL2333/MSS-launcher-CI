# cubiomes WASM 引擎 —— 完整用法参考

本文件是 [`README.md`](README.md) 构建链路的**运行时接口手册**：编译产物
`cubiomes.js` + `cubiomes.wasm`（由 `bridge/bridge.c` 封装 [xpple/cubiomes](https://github.com/xpple/cubiomes)
而来）在已能正常调通的前提下，逐一列出**所有对外用法**、每个参数的含义、
返回值语义、数据读取方式与约束。数值均以当前 CI 实际配置与
[`bridge/bridge.c`](bridge/bridge.c)、[`wasm/latest.json`](wasm/latest.json) 为准。

- 编译器：Emscripten `3.1.51`，优化级别 `-O3`
- 模块形态：`MODULARIZE=1` + `EXPORT_NAME='CubiomesModule'`（工厂函数）
- 内存：`ALLOW_MEMORY_GROWTH=1`（堆可增长，TypedArray 视图会失效）
- 运行时方法：额外导出 `ccall`、`cwrap`
- 导出 C 函数：`set_seed`、`gen_biomes_tile`、`get_biome_buf`、`find_structures`、`get_struct_buf`

---

## 0. 调用模型总览

引擎是一台**有状态的单例生成器**，JS 侧不持有 cubiomes 对象，所有状态都
留在 WASM 内部的静态变量里。典型使用顺序：

```
set_seed(...)            → 建立/复用 Generator 并写入种子（每次查询前必做）
gen_biomes_tile(...)     → 把一批群系 ID 写进 C 侧静态缓冲区，返回元素个数
get_biome_buf()          → 取上一批结果的堆指针，立即从 HEAP32 拷出
find_structures(...)     → 把一批结构 {x,z} 写进另一静态缓冲区，返回个数
get_struct_buf()         → 取上一批结果的堆指针，立即从 HEAP32 拷出
```

三条核心不变量（违反会得到 0 或错乱数据）：

1. **先 `set_seed` 再查询**：未初始化时 `gen_biomes_tile` / `find_structures`
   直接返回 `0`。
2. **缓冲区会被下一批同类调用覆盖**：`get_biome_buf` 的指针只在下一次
   `gen_biomes_tile` 前有效；`get_struct_buf` 同理。取到指针后**立刻拷贝**。
3. **每次读堆前重新获取视图**：`ALLOW_MEMORY_GROWTH` 会让旧的 `HEAP32`
   detached，务必从模块实例上重新取 `mod.HEAP32`。

---

## 1. 加载模块

产物是 Emscripten 工厂函数，`await` 后得到实例，导出函数以 `_函数名`
挂在实例上（C 符号带下划线前缀）。

```js
// 浏览器：<script src="cubiomes.js"></script> 后即有全局 CubiomesModule
// Node：  const CubiomesModule = require('./cubiomes.js')
const mod = await CubiomesModule({
  locateFile: (name) => `./wasm/${name}`, // 指定 wasm 的查找路径
});
// mod._set_seed / mod._gen_biomes_tile / ... 均可用
// mod.ccall / mod.cwrap 也已导出，可用于更底层的调用
```

| 环境       | 获取工厂函数                                                | 备注                                            |
| ---------- | ----------------------------------------------------------- | ----------------------------------------------- |
| 浏览器     | `<script src="cubiomes.js">` 后取全局 `CubiomesModule`      | 走 CSP 需 `wasm-unsafe-eval`                    |
| Node.js    | `require('./cubiomes.js')`                                  | 产物挂了 CommonJS 导出                          |
| ESM        | `createRequire` 引入，或将文件重命名为 `.cjs`               | 直接 `import` 裸 `.js` 可能因 CJS 格式报错      |
| Web Worker | Worker 内 `importScripts('cubiomes.js')` 后同样 `await` 工厂 | 每个 Worker 持有独立实例，可多核并行            |

`cubiomes.js` 与 `cubiomes.wasm` 建议同目录放置；跨域加载时 Release 资产域名
带 CORS 头，可直接 `fetch`。

---

## 2. `set_seed(mc_version, seed_hi, seed_lo) => void`

初始化（或复用）生成器并应用种子。这是**唯一携带版本/种子状态**的入口，
后续所有查询都基于它。

### 参数

| 参数       | 类型   | 含义                                                                                                                       |
| ---------- | ------ | -------------------------------------------------------------------------------------------------------------------------- |
| mc_version | number | Minecraft 版本 ID，取自上游 `enum MCVersion` 的枚举序号，见 [§7 版本对照表](#7-常量对照表mc_version)。改动它会重建生成器。 |
| seed_hi    | number | 64 位种子的**高 32 位**（按有符号 int32 传入，保留位模式即可）。                                                             |
| seed_lo    | number | 64 位种子的**低 32 位**。                                                                                                    |

C 侧重建：`seed = ((uint64)seed_hi << 32) | (uint32)seed_lo`。之所以拆成两半，
是因为 Emscripten 导出函数在 JS 侧传 int64 不便且易丢精度。

### 返回

无返回值（`void`）。调用即完成 `setupGenerator`（首次或版本变化时）与
`applySeed(DIM_OVERWORLD, seed)`。

### 种子拆分（务必用 BigInt 保留位模式）

JS 的 `number` 是浮点，直接用 `Math.floor` 拆大整数会丢精度；负数种子也需
正确处理。统一用 `BigInt.asUintN(64, ...)` 归一到无符号 64 位再高低拆分：

```js
function setSeed(mod, mcVersion, seed /* number | bigint */) {
  const s = BigInt.asUintN(64, BigInt(seed)); // 支持负数，如 -12345
  mod._set_seed(mcVersion, Number(s >> 32n) | 0, Number(s & 0xffffffffn) | 0);
}
setSeed(mod, 33, -12345); // 33 = MC_1_21_11（1.21 别名）
```

### 约束

- **维度固定为主世界**：桥接层把 `DIM_OVERWORLD` 硬编码在 C 侧，接口没有
  维度参数。要下界/末地须改 `bridge/bridge.c` 新增入口并重新构建。
- 切换 `mc_version` 会自动重建 Generator，无需手动释放；相同版本重复调用
  只重设种子，开销小。

---

## 3. `gen_biomes_tile(x, z, sx, sz) => count`

批量生成矩形区域的**群系 ID**。固定 `scale=4`（1:4 缩略地图常用比例）、
固定单层 `y=63`（海平面）。结果写入 C 侧静态整型缓冲区，返回元素数量。

### 参数（全部为**瓦片坐标**，非方块坐标）

| 参数 | 类型   | 含义                                                                   |
| ---- | ------ | ---------------------------------------------------------------------- |
| x    | number | 矩形起点 X（1:4 缩放后的瓦片坐标）                                     |
| z    | number | 矩形起点 Z（瓦片坐标）                                                 |
| sx   | number | 水平方向瓦片数，须 `> 0`                                               |
| sz   | number | 垂直方向瓦片数，须 `> 0`                                               |

> `scale=4` 下每个数组元素代表 **4×4 方块**。方块坐标 = 瓦片坐标 × 4。
> 例：`sx=64` 覆盖 `64 × 4 = 256` 方块宽。

### 返回

- 成功：元素数量 `count = sx * sz`（行优先排列）。
- 失败：`0`（未 `set_seed`、`sx<=0`/`sz<=0`、缓存申请失败等）。**以返回值
  是否为 0 判断成败，不要假设总等于 `sx*sz`。**

### 读取结果

```js
const count = mod._gen_biomes_tile(-128, -128, 64, 64);
if (count > 0) {
  const ptr = mod._get_biome_buf();          // 堆指针（字节地址）
  const i32 = ptr >> 2;                        // 转 Int32 下标
  // 每次重新取视图：内存增长后旧的 mod.HEAP32 已 detached
  const biomes = mod.HEAP32.slice(i32, i32 + count);
  // biomes[i] 对应方块 (x*4 + i % sx, z*4 + floor(i / sx))，值为 BiomeID
}
```

数据布局：`int32` 一维数组，行优先（先 X 后 Z），第 `i` 个元素对应
方块坐标 `(x*4 + i % sx, z*4 + ⌊i / sx⌋)`。

---

## 4. `get_biome_buf() => ptr`

返回上一次 `gen_biomes_tile` 写入的内部静态缓冲区**堆指针**（字节地址）。

- 无参数。
- 本身不触发计算，只是暴露指针；须配合 `gen_biomes_tile` 的返回值确定长度。
- **指针指向 `int32` 数组**，转下标用 `>> 2`。
- 该指针在下一次 `gen_biomes_tile` 调用后被覆盖，务必先拷出。
- 若从未成功调用过 `gen_biomes_tile`，可能返回 `0`/NULL，读取前判空。

---

## 5. `find_structures(struct_id, x1, z1, x2, z2) => count`

查找包围盒内某类结构的**有效坐标**。桥接层逐 region 取生成尝试位置
（`getStructurePos`），再用 `isViableStructurePos` 校验群系/地形，**剔除
地图上可见但游戏内不生成的「幽灵结构」**，因此返回坐标保证游戏内实际生成。

### 参数（包围盒为**方块坐标**）

| 参数      | 类型   | 含义                                                                  |
| --------- | ------ | --------------------------------------------------------------------- |
| struct_id | number | 结构类型 ID，取自 `enum StructureType`，见 [§7 结构对照表](#7-常量对照表mc_version) |
| x1, z1    | number | 包围盒一个角（方块坐标）                                              |
| x2, z2    | number | 对角（方块坐标）。`x1>x2` / `z1>z2` 会自动交换                        |

> ⚠️ 坐标系与 `gen_biomes_tile` 不同：这里 `x1..z2` 是**方块坐标**，
> 返回的也是方块坐标。两者不可混用。

### 返回

- 成功：找到的结构数量 `count`。
- 失败：`0`（未 `set_seed`、`struct_id`/版本无对应配置等）。
- **单次调用候选上限 4096**（每个 region 至多一个候选，超大包围盒截断到
  4096）。范围更大请分块多次查询。

### 读取结果

```js
const n = mod._find_structures(4, -512, -512, 511, 511); // 4 = Igloo
const ptr = mod._get_struct_buf();
const buf = mod.HEAP32;              // 重新取视图
const out = [];
for (let i = 0; i < n; i++) {
  out.push({
    x: buf[(ptr >> 2) + i * 2],
    z: buf[(ptr >> 2) + i * 2 + 1],
  });
}
```

数据布局：结构缓冲区是 `{int32 x; int32 z;}` 数组，每个结构占 **2 个
int32**（8 字节），按 `x, z, x, z, ...` 平铺。

---

## 6. `get_struct_buf() => ptr`

返回上一次 `find_structures` 写入的结构缓冲区**堆指针**。规则与
`get_biome_buf` 一致：无参数、指向下批同类调用前有效、按 `>>2` 转 Int32
下标、每结构 2 个 int32。读取前判 `find_structures` 返回值是否 `> 0`。

---

## 7. 常量对照表（`mc_version` / `struct_id`）

`mc_version` 取自上游 `biomes.h` 的 `enum MCVersion`（枚举序号）：

| 版本     | ID  | 版本          | ID  | 版本            | ID      |
| -------- | --- | ------------- | --- | --------------- | ------- |
| Beta 1.8 | 2   | 1.12          | 15  | 1.19.2          | 23      |
| 1.0      | 3   | 1.13          | 16  | 1.19 / 1.19.4   | 25      |
| 1.1      | 4   | 1.14          | 17  | 1.20 / 1.20.6   | 26      |
| 1.2      | 5   | 1.15          | 18  | 1.21.1          | 27      |
| 1.3      | 6   | 1.16.1        | 19  | 1.21.3          | 28      |
| 1.4      | 7   | 1.16 / 1.16.5 | 20  | 1.21.4 (24w21d) | 29      |
| 1.5      | 8   | 1.17          | 21  | 1.21.5          | 30      |
| 1.6      | 9   | 1.18          | 22  | 1.21.6          | 31      |
| 1.7      | 10  | —             | —   | 1.21.9          | 32      |
| 1.8      | 11  | —             | —   | 1.21.11 / 1.21  | 33      |
| 1.9      | 12  | —             | —   | 26.1            | 34      |
| 1.10     | 13  | —             | —   | 26.2 / 26.3     | 35 / 36 |

> 带别名的行（如 `1.16 / 1.16.5`）表示上游 `MC_1_16 = MC_1_16_5`，短别名与
> 具体补丁号共用同一 ID；`MC_NEWEST` 当前为 26.3（id=36）。

`struct_id` 取自 `enum StructureType`：

| 结构           | ID  | 结构            | ID  |
| -------------- | --- | --------------- | --- |
| Desert_Pyramid | 1   | Shipwreck       | 7   |
| Jungle_Temple  | 2   | Monument        | 8   |
| Swamp_Hut      | 3   | Mansion         | 9   |
| Igloo          | 4   | Outpost         | 10  |
| Village        | 5   | Ruined_Portal   | 11  |
| Ocean_Ruin     | 6   | Ruined_Portal_N | 12  |
| Ancient_City   | 13  | Treasure        | 14  |
| Mineshaft      | 15  | Desert_Well     | 16  |
| Geode          | 17  | Fortress        | 18  |
| Bastion        | 19  | Nether_Fossil   | 20  |
| End_City       | 21  | End_Gateway     | 22  |
| End_Island     | 23  | Trail_Ruins     | 24  |
| Trial_Chambers | 25  | Abandoned_Camp  | 26  |
| Stronghold     | 27  |                 |     |

> **枚举随 master 滚动变动**：产物每天按上游最新提交重建，枚举可能增删。
> 稳妥做法是集成端自维护「游戏版本 / 结构名 → ID」映射，升级元数据后对
> 目标版本跑冒烟测试（`set_seed` + 小范围 `gen_biomes_tile`，`count > 0` 即通过）。
> 仓库内 `cubiomes-master/` 副本内容陈旧，**不可**作为取值依据，须以 workflow
> 按 master SHA 下载的上游头文件为准。

群系 ID（`gen_biomes_tile` 输出值）与官方 `minecraft:worldgen/biome` 注册表
数值一致（如 `plains=1`、`forest=4`），但该对应**仅适用于 1.18+** 的新注册表；
旧版本编号与空洞不同，映射前先核对上游 `biomes.h`。

---

## 8. 端到端示例

### 8.1 浏览器最小用法

```html
<script src="cubiomes.js"></script>
<script>
  (async () => {
    const mod = await CubiomesModule({ locateFile: (n) => `./wasm/${n}` });
    const s = BigInt.asUintN(64, 12345n);
    mod._set_seed(33, Number(s >> 32n) | 0, Number(s & 0xffffffffn) | 0);

    const count = mod._gen_biomes_tile(-64, -64, 128, 128);
    const ptr = mod._get_biome_buf() >> 2;
    const biomes = mod.HEAP32.slice(ptr, ptr + count);
    // 用 biomes 着色地图 ...
  })();
</script>
```

### 8.2 Node.js（含下载校验与缓存）

```js
const fs = require("fs");
const crypto = require("crypto");
const path = require("path");

const META_URL =
  "https://raw.githubusercontent.com/YGYL2333/MSS-launcher-CI/main/wasm/latest.json";
const DIR = path.join(__dirname, "wasm");

async function fetchVerified(json, name) {
  const meta = json.files[name];
  const dest = path.join(DIR, name);
  if (fs.existsSync(dest)) {
    const h = crypto.createHash("sha256").update(fs.readFileSync(dest)).digest("hex");
    if (h === meta.sha256) return dest; // 本地缓存有效
  }
  const buf = Buffer.from(await (await fetch(meta.downloadUrl)).arrayBuffer());
  const h = crypto.createHash("sha256").update(buf).digest("hex");
  if (h !== meta.sha256 || buf.length !== meta.size)
    throw new Error(`${name} 校验失败`);
  fs.mkdirSync(DIR, { recursive: true });
  fs.writeFileSync(dest, buf);
  return dest;
}

(async () => {
  const meta = await (await fetch(META_URL)).json();
  await fetchVerified(meta, "cubiomes.js");
  const wasmPath = await fetchVerified(meta, "cubiomes.wasm");

  const CubiomesModule = require(path.join(DIR, "cubiomes.js"));
  const mod = await CubiomesModule({
    locateFile: (name) => (name.endsWith(".wasm") ? wasmPath : name),
  });

  // 1) 种子：MC 1.21.11 (id=33)，seed=-12345
  const s = BigInt.asUintN(64, -12345n);
  mod._set_seed(33, Number(s >> 32n) | 0, Number(s & 0xffffffffn) | 0);

  // 2) 群系：原点为中心 128x128 瓦片（覆盖 512x512 方块）
  const n = mod._gen_biomes_tile(-128, -128, 128, 128);
  const bp = mod._get_biome_buf() >> 2;
  const biomes = mod.HEAP32.slice(bp, bp + n);

  // 3) 结构：同范围方块坐标内找雪屋 (id=4)
  const sn = mod._find_structures(4, -512, -512, 511, 511);
  const sp = mod._get_struct_buf() >> 2;
  for (let i = 0; i < sn; i++) {
    console.log("igloo at", mod.HEAP32[sp + i * 2], mod.HEAP32[sp + i * 2 + 1]);
  }
})();
```

### 8.3 用 `cwrap` 让调用更清爽

```js
const set_seed = mod.cwrap("set_seed", null, ["number", "number", "number"]);
const gen = mod.cwrap("gen_biomes_tile", "number", ["number", "number", "number", "number"]);
const buf = mod.cwrap("get_biome_buf", "number", []);
set_seed(33, hi, lo);
const count = gen(-64, -64, 128, 128);
const ptr = buf();
```

---

## 9. 性能与工程注意事项

- **同步阻塞**：单次大范围生成在调用线程上同步执行。地图类应用建议放入
  Web Worker，按可视分块增量生成；一个实例只能串行处理调用，多核并行请开
  多个 Worker 各自持有实例。
- **缓冲区生命周期**：静态缓冲区在下一次同类调用前有效，读完立刻拷贝；
  不要把 `get_biome_buf`/`get_struct_buf` 的指针长期保存。
- **视图失效**：`ALLOW_MEMORY_GROWTH` 下每次读堆都从 `mod` 重新取 `HEAP32`。
- **存在性检查与降级**：导出函数以 `bridge/bridge.c` 与 `latest.json.exports`
  为准，桥接层当前只增不删，但仍建议对 `mod._set_seed` 等做存在性检查，
  失败时回退到内置旧产物。
- **完整性校验**：始终以 `latest.json` 的 sha256/size 为准校验下载；同一 URL
  内容每天随上游滚动变化，需锁版本时记录 `latest.json` 的 `runId` 并保留副本。

---

## 10. 接口一览（速查）

| 函数              | 签名                                     | 坐标系     | 返回         | 输出布局                     |
| ----------------- | ---------------------------------------- | ---------- | ------------ | ---------------------------- |
| `set_seed`        | `(mc_version, seed_hi, seed_lo) => void` | —          | 无           | 建立 Generator + 应用种子    |
| `gen_biomes_tile` | `(x, z, sx, sz) => count`                | 瓦片 1:4   | 群系 ID 个数 | int32 数组，行优先，`>>2`    |
| `get_biome_buf`   | `() => ptr`                              | —          | 堆指针       | 上批群系结果                 |
| `find_structures` | `(struct_id, x1, z1, x2, z2) => count`   | 方块       | 结构个数(≤4096) | `{x,z}` 数组，每结构 2 int32 |
| `get_struct_buf`  | `() => ptr`                              | —          | 堆指针       | 上批结构结果                 |