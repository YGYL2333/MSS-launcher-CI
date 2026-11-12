/*
 * bridge.c —— cubiomes -> JS 的批处理桥接层
 *
 * 关于种子：Emscripten 导出的 C 函数以 int64 作为参数在 JS 侧调用不便，
 * 因此 set_seed 拆成 (mc_version, dim, seed_hi, seed_lo)，种子用两个 32 位整数在 C 侧重建 uint64，
 * dim 取 enum Dimension（DIM_NETHER=-1 / DIM_OVERWORLD=0 / DIM_END=+1）。
 */

#include "generator.h"
#include "biomes.h"
#include "finders.h"
#include "util.h"   /* mc2str / str2mc：版本枚举值与显示字符串互转 */
#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static Generator g;
static int g_ready = 0;
static int g_version = 0;

/* ---------------- 群系缓冲区 ---------------- */
static int *biome_buf = NULL;
static int biome_buf_len = 0;

/* ---------------- 结构缓冲区 ---------------- */
typedef struct { int x; int z; } StructPos;
static StructPos *struct_buf = NULL;
static int struct_buf_len = 0;

/* region 索引计算需要向负无穷取整：直接复用 cubiomes rng.h 提供的
 * static inline int32_t floordiv(int32_t, int32_t)（经 finders.h 传递包含），
 * 不要在本文件重复定义，否则与 rng.h 冲突。 */

/* 初始化 Generator 并应用种子。scale 采用 1:4（地图缩略常用比例）。
 * dim 取 enum Dimension（DIM_NETHER=-1 / DIM_OVERWORLD=0 / DIM_END=+1），非法值回退主世界。
 * Generator 只随版本重建，维度切换仅重新 applySeed（其内部按 dim 配置维度噪声）。 */
EMSCRIPTEN_KEEPALIVE
void set_seed(int mc_version, int dim, int seed_hi, int seed_lo)
{
    uint64_t seed = ((uint64_t)(uint32_t)seed_hi << 32) | (uint32_t)seed_lo;

    if (dim != DIM_NETHER && dim != DIM_END)
        dim = DIM_OVERWORLD;   /* 非法/未定义维度回退主世界 */

    if (!g_ready || g_version != mc_version) {
        /* flags=0：默认地表生成器（新版 setupGenerator 返回 void，无错误码） */
        setupGenerator(&g, mc_version, 0);
        g_ready = 1;
        g_version = mc_version;
    }
    applySeed(&g, dim, seed);
}

/*
 * 批量生成矩形区域群系 ID（scale=4，单层 y=63）。
 * 结果写入内部静态缓冲区，JS 通过 get_biome_buf() 读取，返回元素数量。
 */
EMSCRIPTEN_KEEPALIVE
int gen_biomes_tile(int x, int z, int sx, int sz)
{
    Range r;
    int needed;
    size_t cache_len;

    if (!g_ready || sx <= 0 || sz <= 0) return 0;

    memset(&r, 0, sizeof(r));
    r.scale = 4;
    r.x = x;
    r.z = z;
    r.sx = sx;
    r.sz = sz;
    r.y = 63;
    r.sy = 1;

    needed = sx * sz;
    /*
     * 新版 genBiomes 直接写入调用方缓冲区，缓冲区尺寸须按 getMinCacheSize
     * 计算（部分版本会额外要求临时空间，但群系 ID 仍位于前 sx*sz 个元素，
     * JS 侧只需读取前 needed 个）。旧版的 allocCache/freeCache 已移除。
     */
    cache_len = getMinCacheSize(&g, r.scale, r.sx, r.sy, r.sz);
    if (cache_len == 0) return 0;
    if ((size_t)biome_buf_len < cache_len) {
        free(biome_buf);
        biome_buf = (int *)malloc(cache_len * sizeof(int));
        if (!biome_buf) { biome_buf_len = 0; return 0; }
        biome_buf_len = (int)cache_len;
    }

    if (genBiomes(&g, biome_buf, r) != 0) return 0;

    return needed;
}

EMSCRIPTEN_KEEPALIVE
int *get_biome_buf(void) { return biome_buf; }

/*
 * 查找矩形范围内某类结构的有效坐标，结果写入结构缓冲区，返回数量。
 * 新版 cubiomes 取消了批量的 getStructurePositions/Bounding/Position，
 * 改为先取结构的 region 尺寸，再逐个 region 调用 getStructurePos 得到
 * “生成尝试位置”，并用 isViableStructurePos 校验群系/地形，否则会出现
 * 地图上可见但游戏内不生成的“幽灵结构”。
 */
EMSCRIPTEN_KEEPALIVE
int find_structures(int struct_id, int x1, int z1, int x2, int z2)
{
    StructureConfig sconf;
    Pos pos;
    int count = 0;
    int regionBlocks;
    int regX1, regX2, regZ1, regZ2, rx, rz;
    long long totalRegions;
    int capacity;

    if (!g_ready) return 0;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (z1 > z2) { int t = z1; z1 = z2; z2 = t; }

    /* 取得该结构在此版本下的 region 尺寸（以 chunk 为单位） */
    if (!getStructureConfig(struct_id, g_version, &sconf)) return 0;
    regionBlocks = (int)sconf.regionSize << 4;   /* regionSize * 16 方块 */
    if (regionBlocks <= 0) regionBlocks = 1;

    /* 覆盖包围盒的 region 索引范围，向外扩 1 以纳入相邻 region 的边缘候选 */
    regX1 = floordiv(x1, regionBlocks) - 1;
    regX2 = floordiv(x2, regionBlocks) + 1;
    regZ1 = floordiv(z1, regionBlocks) - 1;
    regZ2 = floordiv(z2, regionBlocks) + 1;

    /* 容量：每个 region 至多一个候选位置；上限 4096 防止超大包围盒爆内存 */
    totalRegions = (long long)(regX2 - regX1 + 1) * (regZ2 - regZ1 + 1);
    if (totalRegions < 1) totalRegions = 1;
    capacity = (totalRegions > 4096) ? 4096 : (int)totalRegions;

    if (struct_buf_len < capacity) {
        free(struct_buf);
        struct_buf = (StructPos *)malloc((size_t)capacity * sizeof(StructPos));
        if (!struct_buf) { struct_buf_len = 0; return 0; }
        struct_buf_len = capacity;
    }
    if (!struct_buf) return 0;

    for (rz = regZ1; rz <= regZ2 && count < capacity; rz++) {
        for (rx = regX1; rx <= regX2 && count < capacity; rx++) {
            /* 每个 region 至多给出一个生成尝试位置 */
            if (!getStructurePos(struct_id, g_version, g.seed, rx, rz, &pos))
                continue;
            /* 过滤落在包围盒之外的候选 */
            if (pos.x < x1 || pos.x > x2 || pos.z < z1 || pos.z > z2)
                continue;
            /* 校验群系/地形，剔除“幽灵结构” */
            if (!isViableStructurePos(struct_id, &g, pos.x, pos.z, 0))
                continue;
            struct_buf[count].x = pos.x;
            struct_buf[count].z = pos.z;
            count++;
        }
    }

    return count;
}

EMSCRIPTEN_KEEPALIVE
StructPos *get_struct_buf(void) { return (StructPos *)struct_buf; }

/* ---------------- 版本参数接口 ---------------- */
/*
 * 版本真值来源为 biomes.h 的 enum MCVersion；mc2str/str2mc（util.h）负责枚举值
 * 与显示字符串之间的转换。此处包装成 snake_case 导出，命名风格与
 * set_seed/gen_biomes_tile 一致，并沿用「静态缓冲区 + 返回数量」的读取范式：
 * JS 先调 get_supported_versions() 取数量，再从 get_supported_versions_buf()
 * 逐个读取版本枚举值，最后对每个值调 mc_version_to_str() 取名字。
 */
static int *version_buf = NULL;
static int version_buf_len = 0;

/* 枚举值 -> 显示字符串（未知版本返回 "?"）。返回内部静态字符串，调用方勿释放。 */
EMSCRIPTEN_KEEPALIVE
const char *mc_version_to_str(int mc) { return mc2str(mc); }

/* 显示字符串 -> 枚举值；无法识别（含 NULL）返回 MC_UNDEF。 */
EMSCRIPTEN_KEEPALIVE
int mc_str_to_version(const char *s) { return s ? str2mc(s) : MC_UNDEF; }

/*
 * 遍历 MC_B1_7..MC_NEWEST，把有显示名的版本枚举值写入静态缓冲区，返回数量。
 * 跳过 mc2str 返回 "?" 的项，使枚举存在别名/空洞时结果仍然干净。
 */
EMSCRIPTEN_KEEPALIVE
int get_supported_versions(void)
{
    int mc, count = 0;
    int total = MC_NEWEST - MC_B1_7 + 1;

    if (total <= 0) return 0;
    if (version_buf_len < total) {
        free(version_buf);
        version_buf = (int *)malloc((size_t)total * sizeof(int));
        if (!version_buf) { version_buf_len = 0; return 0; }
        version_buf_len = total;
    }

    for (mc = MC_B1_7; mc <= MC_NEWEST; mc++) {
        const char *name = mc2str(mc);
        if (!name || (name[0] == '?' && name[1] == '\0'))
            continue;   /* 无显示名，跳过 */
        version_buf[count++] = mc;
    }
    return count;
}

EMSCRIPTEN_KEEPALIVE
int *get_supported_versions_buf(void) { return version_buf; }

/* ---------------- 维度枚举接口 ---------------- */
/*
 * 维度真值来源为 biomes.h 的 enum Dimension（DIM_NETHER=-1 / DIM_OVERWORLD=0 /
 * DIM_END=+1，DIM_UNDEF=1000 为哨兵）。cubiomes 未提供维度<->字符串函数，故此处
 * 自建映射；set_seed 的 dim 入参即取这些值。读取范式同版本/结构接口。
 */
static int *dim_buf = NULL;
static int dim_buf_len = 0;

/* 维度值 -> 名称（未知返回 "undefined"）。返回内部静态字符串，调用方勿释放。 */
EMSCRIPTEN_KEEPALIVE
const char *dim_to_str(int dim)
{
    switch (dim) {
    case DIM_NETHER:    return "nether";
    case DIM_OVERWORLD: return "overworld";
    case DIM_END:       return "end";
    default:            return "undefined";
    }
}

/* 把受支持的维度值（下界/主世界/末地）写入缓冲区，返回数量（恒为 3）。 */
EMSCRIPTEN_KEEPALIVE
int get_supported_dimensions(void)
{
    static const int dims[3] = { DIM_NETHER, DIM_OVERWORLD, DIM_END };
    if (dim_buf_len < 3) {
        free(dim_buf);
        dim_buf = (int *)malloc(3 * sizeof(int));
        if (!dim_buf) { dim_buf_len = 0; return 0; }
        dim_buf_len = 3;
    }
    memcpy(dim_buf, dims, sizeof(dims));
    return 3;
}

EMSCRIPTEN_KEEPALIVE
int *get_supported_dimensions_buf(void) { return dim_buf; }

/* ---------------- 结构类型枚举接口 ---------------- */
/*
 * 结构 ID 真值来源为 finders.h 的 enum StructureType（哨兵 FEATURE_NUM 给出上界），
 * struct2str（util.h）给出资源名。此处按版本过滤出 find_structures 实际可用的结构：
 * 同时要求 getStructureConfig 成功（该版本支持、且为 region 型结构）与 struct2str
 * 有名字，从而排除 Feature/End_Island/Stronghold 等非 region 型或无名字面量。
 * 读取范式同版本接口：get_supported_structures() 返回数量，缓冲区逐个读结构 ID。
 */
static int *struct_id_buf = NULL;
static int struct_id_buf_len = 0;

/* 结构 ID -> 资源名（无对应名字返回 NULL）。返回内部静态字符串，调用方勿释放。 */
EMSCRIPTEN_KEEPALIVE
const char *struct_to_str(int stype) { return struct2str(stype); }

/*
 * 遍历 enum StructureType，把 mc 版本下 find_structures 可用的结构 ID 写入缓冲区，
 * 返回数量。
 */
EMSCRIPTEN_KEEPALIVE
int get_supported_structures(int mc)
{
    StructureConfig sconf;
    int stype, count = 0;

    if (FEATURE_NUM <= 0) return 0;
    if (struct_id_buf_len < FEATURE_NUM) {
        free(struct_id_buf);
        struct_id_buf = (int *)malloc((size_t)FEATURE_NUM * sizeof(int));
        if (!struct_id_buf) { struct_id_buf_len = 0; return 0; }
        struct_id_buf_len = FEATURE_NUM;
    }

    for (stype = 0; stype < FEATURE_NUM; stype++) {
        if (!struct2str(stype)) continue;                     /* 无资源名，跳过 */
        if (!getStructureConfig(stype, mc, &sconf)) continue; /* 该版本不支持 */
        struct_id_buf[count++] = stype;
    }
    return count;
}

EMSCRIPTEN_KEEPALIVE
int *get_supported_structures_buf(void) { return struct_id_buf; }

/* ---------------- 群系 ID 枚举接口 ---------------- */
/*
 * 群系 ID 真值来源为 biomes.h 的 enum BiomeID（取值 0..255，无计数哨兵，故按
 * 固定上界 256 遍历，与 util.c 内部一致）；biomeExists(mc,id) 判定该版本是否存在，
 * biome2str(mc,id) 给出资源名。gen_biomes_tile 返回的即这些 ID，软件可据此转名字。
 */
#define BIOME_ID_COUNT 256
static int *biome_id_buf = NULL;
static int biome_id_buf_len = 0;

/* 群系 ID -> 资源名（该版本无此群系时可能返回 NULL）。返回内部静态字符串。 */
EMSCRIPTEN_KEEPALIVE
const char *biome_to_str(int mc, int id) { return biome2str(mc, id); }

/* 遍历 0..255，把 mc 版本下存在的群系 ID 写入缓冲区，返回数量。 */
EMSCRIPTEN_KEEPALIVE
int get_supported_biomes(int mc)
{
    int id, count = 0;

    if (biome_id_buf_len < BIOME_ID_COUNT) {
        free(biome_id_buf);
        biome_id_buf = (int *)malloc((size_t)BIOME_ID_COUNT * sizeof(int));
        if (!biome_id_buf) { biome_id_buf_len = 0; return 0; }
        biome_id_buf_len = BIOME_ID_COUNT;
    }

    for (id = 0; id < BIOME_ID_COUNT; id++) {
        if (!biomeExists(mc, id)) continue;   /* 该版本不存在此群系 */
        if (!biome2str(mc, id)) continue;     /* 无资源名，跳过 */
        biome_id_buf[count++] = id;
    }
    return count;
}

EMSCRIPTEN_KEEPALIVE
int *get_supported_biomes_buf(void) { return biome_id_buf; }
