# MSS-launcher-CI

A Github Action for building and deploying Cubiomes WASM

CI 每天追踪 [xpple/cubiomes](https://github.com/xpple/cubiomes) 上游 master，
经 `bridge/bridge.c` 桥接层用 Emscripten 编译为 WASM，产物以滚动 Release
（tag `wasm-latest`）分发，轻量元数据（下载 URL + sha256）提交在
[`wasm/latest.json`](wasm/latest.json)。以下章节面向**下游集成方**，
说明如何在自己的软件中加载并调用这份 WASM。

## 集成使用指南

### 1. 获取产物

不要硬编码下载地址，先拉取元数据，再按 `files.*.downloadUrl` 下载并校验：

```
https://raw.githubusercontent.com/YGYL2333/MSS-launcher-CI/main/wasm/latest.json
```

`latest.json` 中与集成相关的字段：

| 字段                                                       | 用途                                                  |
| ---------------------------------------------------------- | ----------------------------------------------------- |
| `files["cubiomes.js"].downloadUrl` / `.sha256` / `.size`   | 胶水 JS 的下载与完整性校验                            |
| `files["cubiomes.wasm"].downloadUrl` / `.sha256` / `.size` | WASM 二进制的下载与完整性校验                         |
| `commit` / `builtAt`                                       | 对应的上游 cubiomes 提交，用于复现问题与版本追溯      |
| `exports`                                                  | 当前构建导出的 C 函数列表，集成前建议据此做存在性检查 |

产物每日随上游更新滚动覆盖，**同一 URL 的内容会变化**：建议下载后按
sha256 校验并缓存到本地（Launcher 场景随自身更新通道打包分发更稳妥）。
需要锁定某次构建时，可记录 `latest.json` 的 `runId` 并保留对应副本，
Release 资产本身只保留最新一份。

### 2. 加载模块

构建时启用了 `MODULARIZE=1`、`EXPORT_NAME='CubiomesModule'`、
`ALLOW_MEMORY_GROWTH=1`，并导出 `ccall`/`cwrap` 运行时方法。
`cubiomes.js` 与 `cubiomes.wasm` 建议放在同一目录下。浏览器直接用
`<script src="cubiomes.js">` 即可拿到全局 `CubiomesModule`；产物同时带
CommonJS 导出，Node 下 `require('./cubiomes.js')` 即得到工厂函数；
ESM 工程可用 `createRequire` 或重命名为 `.cjs` 后引入：

```js
// CubiomesModule 是工厂函数，await 后得到实例，导出函数挂在实例上
const mod = await CubiomesModule({
  locateFile: (name) => `./wasm/${name}`, // 自定义 wasm 查找路径
});
```

浏览器中若走 CSP，需要 `wasm-unsafe-eval`（或匹配 wasm 来源的
`script-src`）权限；跨域加载 wasm 时 Release 资产域名
`github.com` 带有 CORS 头，可直接 `fetch`。

### 3. 导出函数一览

所有函数通过 `mod._函数名(...)` 调用（C 符号带下划线前缀）。
返回的大数组通过「C 侧静态缓冲区 + 指针读取」的方式取出：
每次调用会**覆盖**上一批结果，请读取后立即拷贝。

| 函数              | 签名                                          | 说明                                                                                                                                     |
| ----------------- | --------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `set_seed`        | `(mc_version, dim, seed_hi, seed_lo) => void` | 初始化生成器并按维度设置种子。`mc_version` 见版本对照表；`dim` 取 `-1`/`0`/`+1`（下界/主世界/末地）；64 位种子拆成高低两个 32 位整数传入 |
| `gen_biomes_tile` | `(x, z, sx, sz) => count`                     | 生成矩形群系（scale 1:4、y=63 单层），返回群系 ID 数量                                                                                   |
| `get_biome_buf`   | `() => ptr`                                   | 上一次 `gen_biomes_tile` 结果的堆内存指针（int32 数组）                                                                                  |
| `find_structures` | `(struct_id, x1, z1, x2, z2) => count`        | 查找包围盒内的有效结构位置（已经过 `isViableStructurePos` 校验，无幽灵结构），返回数量                                                   |
| `get_struct_buf`  | `() => ptr`                                   | 上一次 `find_structures` 结果的指针（`{x,z}` 即每结构 2 个 int32）                                                                       |

> 另有枚举查询导出（`get_supported_versions`/`_dimensions`/`_structures`/`_biomes`
> 及对应 `*_to_str`、`*_buf`），用于按版本动态取「可用版本 / 维度 / 结构 / 群系」
> 清单并把 ID 转成名字，详见 [`README_DATA.md`](README_DATA.md)。

#### 3.1 设置种子

Minecraft 种子常为有符号 64 位整数。拆分成两半时**必须保留位模式**，
用 BigInt 按无符号 64 位截断，而不是 JS 的浮点 `Math.floor`：

```js
function setSeed(mod, mcVersion, seed /* number | bigint */, dim = 0) {
  const s = BigInt.asUintN(64, BigInt(seed)); // 支持负数种子，如 -12345
  mod._set_seed(
    mcVersion,
    dim,
    Number(s >> 32n) | 0,
    Number(s & 0xffffffffn) | 0,
  );
}
setSeed(mod, 33, 12345); // 33 = MC_1_21_11（即 1.21 别名），dim=0 主世界，见下方版本对照表
```

切换 `mc_version` 时桥接层会自动重建 Generator。维度由 `set_seed` 的 `dim`
入参选择（`-1` 下界 / `0` 主世界 / `1` 末地，非法值回退主世界）；切换维度
只重新 `applySeed`，不重建 Generator。可用 `get_supported_dimensions` 枚举。

#### 3.2 批量获取群系 ID

```js
const count = mod._gen_biomes_tile(x, z, sx, sz); // 例: (-256, -256, 128, 128)
const ptr = mod._get_biome_buf();
// 每 4 字节一个 int32；HEAP32 可能因内存增长而 detached，每次都要重新取
const biomes = mod.HEAP32.slice(ptr >> 2, (ptr >> 2) + count);
// biomes[i] 对应方块坐标 (x + i % sx, z + Math.floor(i / sx))，值为 BiomeID
```

坐标与尺寸均为**方块单位**（scale=4 下每个数组元素代表 4x4 方块）。
行优先排列，失败（未设置种子、尺寸非正、缓存申请失败）时 `count` 返回 0。
群系 ID 与官方 `minecraft:worldgen/biome` 注册表数值一致（如 `plains=1`、
`forest=4`）；但该对应关系仅适用于 1.18+ 的新注册表，旧版本的编号与
空洞不同，映射前请先核对上游 `biomes.h`。着色可参照 cubiomes 上游的
biome 颜色表。

#### 3.3 查找结构

```js
const n = mod._find_structures(structId, x1, z1, x2, z2);
const ptr = mod._get_struct_buf();
const buf = mod.HEAP32;
const out = [];
for (let i = 0; i < n; i++) {
  out.push({ x: buf[(ptr >> 2) + i * 2], z: buf[(ptr >> 2) + i * 2 + 1] });
}
```

`x1..z2` 为**方块坐标**（与 `gen_biomes_tile` 同一坐标系），返回坐标也是
方块坐标（已保证游戏内实际生成）。单次调用候选上限 4096，
超大范围请分块查询。

### 4. 常量对照表

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

> 注：带别名的行（如 `1.16 / 1.16.5`）表示上游 `MC_1_16 = MC_1_16_5`，
> 短别名与具体补丁号共用同一个 ID；`MC_NEWEST` 当前为 26.3（id=36）。
>
> 本表按当前上游枚举整理；由于产物每日随 master 滚动重建，枚举可能
> 增删。稳妥做法：集成端维护自己的「游戏版本 → ID」映射，并在升级
> 元数据后针对目标版本跑一次冒烟测试（`set_seed` + 小范围
> `gen_biomes_tile`，`count > 0` 即通过）。

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

### 5. 完整示例（Node.js）

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
    const h = crypto
      .createHash("sha256")
      .update(fs.readFileSync(dest))
      .digest("hex");
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

  // 1) 种子: MC 1.21.11 (id=33), seed=-12345
  const s = BigInt.asUintN(64, -12345n);
  mod._set_seed(33, 0, Number(s >> 32n) | 0, Number(s & 0xffffffffn) | 0);

  // 2) 群系: 以原点为中心 64x64 (1:4 比例，覆盖 256x256 方块)
  const n = mod._gen_biomes_tile(-128, -128, 64, 64);
  const ptr = mod._get_biome_buf() >> 2;
  const biomes = mod.HEAP32.slice(ptr, ptr + n);

  // 3) 结构: 同范围内找雪屋 (id=4)
  const sn = mod._find_structures(4, -128, -128, 127, 127);
  const sp = mod._get_struct_buf() >> 2;
  for (let i = 0; i < sn; i++) {
    console.log("igloo at", mod.HEAP32[sp + i * 2], mod.HEAP32[sp + i * 2 + 1]);
  }
})();
```

### 6. 集成注意事项

- **调用序约束**：`gen_biomes_tile` / `find_structures` 之前必须先 `set_seed`，
  否则返回 0。静态缓冲区在下次同类调用前有效，读完立刻拷贝。
- **线程**：单次大范围生成是同步阻塞的，地图类应用建议放入 Web Worker
  （Worker 内 `importScripts('cubiomes.js')` 后同样 `await CubiomesModule()`
  得到独立实例），并按可视分块增量生成；一个实例只能串行处理调用，
  多核并行请开多个 Worker 各自持有实例。
- **完整性**：始终以 `latest.json` 的 sha256 为准校验下载；自行分发时
  连同元数据一起打包，便于追溯对应的上游 commit。
- **接口稳定性**：导出函数以 `bridge/bridge.c` 与 `latest.json.exports`
  为准；桥接层刻意保持窄接口（版本切换、维度/种子拆分、缓冲区读取都封装在
  C 侧），上游 API 变动由本仓库 CI 消化，一般只增不减（`set_seed` 曾因加入
  `dim` 参数调整过签名，属破坏性变更），仍建议对 `mod._set_seed` 等做存在性
  检查并做失败降级（回退到内置旧产物）。
