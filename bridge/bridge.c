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
#include "features/end_city.h"  /* END_SHIP / getEndCityPieces / END_CITY_PIECES_MAX */
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

/* ---------------- 结构缓冲区 ----------------
 * 每个结果附带一个 variant 字段：它始终等于“该实例最具体的结构 ID”——若该大类
 * 存在细分变体（如沙漠村庄、桥梁堡垒），variant 为对应的扩展 ID；否则 variant 等于
 * 大类 ID 本身。JS 侧用 struct_to_str(variant) 直接取到与精灵图 key 一致的名字。
 * 缓冲区按每元素 3 个 int 连续存放：(x, z, variant)。 */
typedef struct { int x; int z; int variant; } StructPos;
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

/* ---------------- 变体与探测扩展 ID ----------------
 * cubiomes 的 enum StructureType 只到大类。下面在同一 ID 空间里，自 FEATURE_NUM
 * 起为“结构变体”与“非区域型探测项”分配扩展 ID：get_supported_structures 一次性
 * 列出大类、变体与探测项；find_structures 既能接受大类 ID（返回该大类全部实例并在
 * 每个结果上标注其具体变体），也能接受变体/探测 ID（只返回匹配项）；struct_to_str
 * 把每个 ID（大类或扩展）翻成与前端精灵图 key 一致的名字。 */
enum {
    XT_VILLAGE_DESERT = FEATURE_NUM,
    XT_VILLAGE_SAVANNA,
    XT_VILLAGE_SNOWY,
    XT_VILLAGE_TAIGA,
    XT_VILLAGE_ZOMBIE,
    XT_BASTION_BRIDGE,
    XT_BASTION_STABLES,
    XT_BASTION_TREASURE,
    XT_BASTION_UNITS,
    XT_IGLOO_BASEMENT,
    XT_RUINED_PORTAL_GIANT,
    XT_OCEAN_RUIN_SPECIAL,
    XT_END_CITY_SHIP,
    XT_ABANDONED_CAMP_SECRET_CHEST,
    XT_NETHER_FOSSIL_GHAST,
    XT_SLIME,
    XT_SPAWN,
    XT_RAW_COPPER,
    XT_RAW_IRON,
    XT_NUM
};

/* 有细分精灵图的变体扩展 ID -> 其所属大类（用于区域定位）。 */
typedef struct { int ext; int base; } ExtVariant;
static const ExtVariant ext_variants[] = {
    { XT_VILLAGE_DESERT, Village },
    { XT_VILLAGE_SAVANNA, Village },
    { XT_VILLAGE_SNOWY, Village },
    { XT_VILLAGE_TAIGA, Village },
    { XT_VILLAGE_ZOMBIE, Village },
    { XT_BASTION_BRIDGE, Bastion },
    { XT_BASTION_STABLES, Bastion },
    { XT_BASTION_TREASURE, Bastion },
    { XT_BASTION_UNITS, Bastion },
    { XT_IGLOO_BASEMENT, Igloo },
    { XT_RUINED_PORTAL_GIANT, Ruined_Portal },
    { XT_OCEAN_RUIN_SPECIAL, Ocean_Ruin },
    { XT_END_CITY_SHIP, End_City },
    { XT_ABANDONED_CAMP_SECRET_CHEST, Abandoned_Camp },
    { XT_NETHER_FOSSIL_GHAST, Nether_Fossil },
};

/* 每个扩展/探测 ID 的精灵图名（与前端 atlas key 对齐）。 */
static const struct { int id; const char *name; } ext_names[] = {
    { XT_VILLAGE_DESERT, "village_desert" },
    { XT_VILLAGE_SAVANNA, "village_savanna" },
    { XT_VILLAGE_SNOWY, "village_snowy" },
    { XT_VILLAGE_TAIGA, "village_taiga" },
    { XT_VILLAGE_ZOMBIE, "village_zombie" },
    { XT_BASTION_BRIDGE, "bastion_remnant_bridge" },
    { XT_BASTION_STABLES, "bastion_remnant_stables" },
    { XT_BASTION_TREASURE, "bastion_remnant_treasure" },
    { XT_BASTION_UNITS, "bastion_remnant_units" },
    { XT_IGLOO_BASEMENT, "igloo_basement" },
    { XT_RUINED_PORTAL_GIANT, "ruined_portal_giant" },
    { XT_OCEAN_RUIN_SPECIAL, "ocean_ruin_special" },
    { XT_END_CITY_SHIP, "end_city_ship" },
    { XT_ABANDONED_CAMP_SECRET_CHEST, "abandoned_camp_secret_chest" },
    { XT_NETHER_FOSSIL_GHAST, "nether_fossil_ghast" },
    { XT_SLIME, "slime" },
    { XT_SPAWN, "spawn" },
    { XT_RAW_COPPER, "raw_copper" },
    { XT_RAW_IRON, "raw_iron" },
};

static const char *ext_name_of(int id)
{
    size_t i;
    for (i = 0; i < sizeof(ext_names) / sizeof(ext_names[0]); i++)
        if (ext_names[i].id == id) return ext_names[i].name;
    return NULL;
}

/* 变体扩展 ID -> 大类 ID；非变体（大类 ID 与探测项）返回 -1。 */
static int ext_base_of(int id)
{
    size_t i;
    for (i = 0; i < sizeof(ext_variants) / sizeof(ext_variants[0]); i++)
        if (ext_variants[i].ext == id) return ext_variants[i].base;
    return -1;
}

/* 确保结构缓冲区至少容纳 need 个元素；成功返回 0，失败返回 -1。 */
static int ensure_struct_capacity(int need)
{
    if (struct_buf_len >= need && struct_buf) return 0;
    free(struct_buf);
    struct_buf = (StructPos *)malloc((size_t)need * sizeof(StructPos));
    if (!struct_buf) { struct_buf_len = 0; return -1; }
    struct_buf_len = need;
    return 0;
}

/* 判定某个已定位结构实例的具体变体 ID；无细分时返回大类 ID 本身。
 * 依赖已 applySeed 的全局 g（版本/种子/维度）。 */
static int classify_variant(int base_id, int bx, int bz)
{
    StructureVariant sv;
    switch (base_id) {
    case Village: {
        /* isViableStructurePos 的 flags 传入具体群系即可判定村庄类型 */
        const int vv[] = { plains, desert, savanna, taiga, snowy_tundra };
        int type = -1, i;
        for (i = 0; i < (int)(sizeof(vv) / sizeof(vv[0])); i++)
            if (isViableStructurePos(Village, &g, bx, bz, (uint32_t)vv[i])) { type = vv[i]; break; }
        if (type < 0) return Village;
        if (getVariant(&sv, Village, g_version, g.seed, bx, bz, type) && sv.abandoned)
            return XT_VILLAGE_ZOMBIE;
        switch (type) {
        case desert:       return XT_VILLAGE_DESERT;
        case savanna:      return XT_VILLAGE_SAVANNA;
        case taiga:        return XT_VILLAGE_TAIGA;
        case snowy_tundra: return XT_VILLAGE_SNOWY;
        default:           return Village;   /* plains 等：用大类精灵图 */
        }
    }
    case Bastion:
        if (getVariant(&sv, Bastion, g_version, g.seed, bx, bz, -1))
            switch (sv.start) {
            case 0: return XT_BASTION_UNITS;
            case 1: return XT_BASTION_STABLES;
            case 2: return XT_BASTION_TREASURE;
            case 3: return XT_BASTION_BRIDGE;
            }
        return Bastion;
    case Igloo:
        if (getVariant(&sv, Igloo, g_version, g.seed, bx, bz, -1) && sv.basement)
            return XT_IGLOO_BASEMENT;
        return Igloo;
    case Ruined_Portal: {
        int id = getBiomeAt(&g, 4, bx >> 2, 0, bz >> 2);
        if (getVariant(&sv, Ruined_Portal, g_version, g.seed, bx, bz, id) && sv.giant)
            return XT_RUINED_PORTAL_GIANT;
        return Ruined_Portal;
    }
    case Ocean_Ruin:
        if (getBiomeAt(&g, 4, bx >> 2, 0, bz >> 2) == warm_ocean)
            return XT_OCEAN_RUIN_SPECIAL;
        return Ocean_Ruin;
    case End_City: {
        static Piece ec_pieces[END_CITY_PIECES_MAX];
        int n = getEndCityPieces(ec_pieces, g.seed, bx >> 4, bz >> 4), i;
        for (i = 0; i < n; i++)
            if (ec_pieces[i].type == END_SHIP) return XT_END_CITY_SHIP;
        return End_City;
    }
    case Abandoned_Camp: {
        StructureSaltConfig sc;
        Piece pieces[2];
        int id = getBiomeAt(&g, 4, bx >> 2, 320 >> 2, bz >> 2);
        if (getStructureSaltConfig(Abandoned_Camp, g_version, id, &sc) &&
            getVariant(&sv, Abandoned_Camp, g_version, g.seed, bx, bz, id)) {
            int np = getStructurePieces(pieces, 2, Abandoned_Camp, sc, &sv, g_version, g.seed, bx, bz);
            int i, j;
            for (i = 0; i < np; i++)
                for (j = 0; j < pieces[i].chestCount && j < 4; j++)
                    if (pieces[i].lootTables[j] &&
                        strcmp(pieces[i].lootTables[j], "chests/abandoned_camp_secret_chest") == 0)
                        return XT_ABANDONED_CAMP_SECRET_CHEST;
        }
        return Abandoned_Camp;
    }
    case Nether_Fossil:
        if (getVariant(&sv, Nether_Fossil, g_version, g.seed, bx, bz, -1) &&
            netherFossilHasGhast(bx >> 4, bz >> 4, g.seed, &sv))
            return XT_NETHER_FOSSIL_GHAST;
        return Nether_Fossil;
    default:
        return base_id;
    }
}

/* 要塞：非 region 型，用螺旋迭代器定位（主世界，B1.8+）。 */
static int find_stronghold(int x1, int z1, int x2, int z2)
{
    StrongholdIter sh;
    Pos p;
    int i, count = 0, total;
    if (g.dim != DIM_OVERWORLD || g_version < MC_B1_8) return 0;
    if (ensure_struct_capacity(128) != 0) return 0;
    initFirstStronghold(&sh, g_version, g.seed & MASK48);
    total = (g_version >= MC_1_9) ? 128 : 3;
    for (i = 0; i < total; i++) {
        int rem = nextStronghold(&sh, &g);   /* 定位当前要塞并推进 */
        p = sh.pos;
        if (p.x >= x1 && p.x <= x2 && p.z >= z1 && p.z <= z2) {
            struct_buf[count].x = p.x;
            struct_buf[count].z = p.z;
            struct_buf[count].variant = Stronghold;
            count++;
        }
        if (rem <= 0) break;
    }
    return count;
}

/* 出生点：单点，仅当落在包围盒内时返回。 */
static int find_spawn(int x1, int z1, int x2, int z2)
{
    Pos sp;
    if (g.dim != DIM_OVERWORLD) return 0;
    if (ensure_struct_capacity(1) != 0) return 0;
    sp = getSpawn(&g);
    if (sp.x >= x1 && sp.x <= x2 && sp.z >= z1 && sp.z <= z2) {
        struct_buf[0].x = sp.x;
        struct_buf[0].z = sp.z;
        struct_buf[0].variant = XT_SPAWN;
        return 1;
    }
    return 0;
}

/* 史莱姆区块：逐 chunk 判定，命中即标注该区块中心（主世界）。 */
static int find_slime(int x1, int z1, int x2, int z2)
{
    int cx1, cx2, cz1, cz2, cx, cz, count = 0, capacity;
    long long total;
    if (g.dim != DIM_OVERWORLD) return 0;
    cx1 = floordiv(x1, 16); cx2 = floordiv(x2, 16);
    cz1 = floordiv(z1, 16); cz2 = floordiv(z2, 16);
    total = (long long)(cx2 - cx1 + 1) * (cz2 - cz1 + 1);
    if (total < 1) total = 1;
    capacity = (total > 4096) ? 4096 : (int)total;
    if (ensure_struct_capacity(capacity) != 0) return 0;
    for (cz = cz1; cz <= cz2 && count < capacity; cz++)
        for (cx = cx1; cx <= cx2 && count < capacity; cx++) {
            int bx, bz;
            if (!isSlimeChunk(g.seed, cx, cz)) continue;
            bx = cx * 16 + 8; bz = cz * 16 + 8;
            if (bx < x1 || bx > x2 || bz < z1 || bz > z2) continue;
            struct_buf[count].x = bx;
            struct_buf[count].z = bz;
            struct_buf[count].variant = XT_SLIME;
            count++;
        }
    return count;
}

/* 粗铜块/粗铁块：大矿脉噪声仅 1.18+ 有。按区块中心粗采样（区块内 4x4 网格），
 * 命中目标方块即标注该区块——属近似，缩略图级别用于指示矿脉分布已足够。 */
static int find_ore_vein(int ext_id, int x1, int z1, int x2, int z2)
{
    OreVeinParameters params;
    int target, sampleY, cx1, cx2, cz1, cz2, cx, cz, ix, iz, count = 0, capacity;
    long long total;
    if (g.dim != DIM_OVERWORLD || g_version < MC_1_18) return 0;
    if (!initOreVeinNoise(&params, g.seed, g_version)) return 0;
    if (ext_id == XT_RAW_COPPER) { target = RAW_COPPER_BLOCK; sampleY = 16; }
    else                         { target = RAW_IRON_BLOCK;   sampleY = -24; }
    cx1 = floordiv(x1, 16); cx2 = floordiv(x2, 16);
    cz1 = floordiv(z1, 16); cz2 = floordiv(z2, 16);
    total = (long long)(cx2 - cx1 + 1) * (cz2 - cz1 + 1);
    if (total < 1) total = 1;
    capacity = (total > 4096) ? 4096 : (int)total;
    if (ensure_struct_capacity(capacity) != 0) return 0;
    for (cz = cz1; cz <= cz2 && count < capacity; cz++) {
        for (cx = cx1; cx <= cx2 && count < capacity; cx++) {
            int found = 0, bx, bz;
            for (iz = 0; iz < 4 && !found; iz++)
                for (ix = 0; ix < 4 && !found; ix++)
                    if (getOreVeinBlockAt(cx * 16 + ix * 4 + 2, sampleY, cz * 16 + iz * 4 + 2, &params) == target)
                        found = 1;
            if (!found) continue;
            bx = cx * 16 + 8; bz = cz * 16 + 8;
            struct_buf[count].x = bx;
            struct_buf[count].z = bz;
            struct_buf[count].variant = ext_id;
            count++;
        }
    }
    return count;
}

/*
 * 查找矩形范围内某类结构（含变体/探测项）的有效坐标，结果写入结构缓冲区，返回数量。
 * 缓冲区每元素为 (x, z, variant)，variant 由 classify_variant 得出，是对该实例最具体
 * 的结构 ID（大类或其某个变体）。传入大类 ID 返回该大类全部实例（各自带变体标注）；
 * 传入某个变体 ID 只返回匹配该变体的实例；要塞与史莱姆/出生点/粗铜/粗铁走专用通道。
 * region 型结构沿用 getStructurePos + isViableStructurePos 校验，剔除“幽灵结构”。
 */
EMSCRIPTEN_KEEPALIVE
int find_structures(int struct_id, int x1, int z1, int x2, int z2)
{
    StructureConfig sconf;
    Pos pos;
    int base_id, variant;
    int count = 0;
    int regionBlocks;
    int regX1, regX2, regZ1, regZ2, rx, rz;
    long long totalRegions;
    int capacity;

    if (!g_ready) return 0;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (z1 > z2) { int t = z1; z1 = z2; z2 = t; }

    /* 非区域型探测项 */
    if (struct_id == XT_SPAWN) return find_spawn(x1, z1, x2, z2);
    if (struct_id == XT_SLIME) return find_slime(x1, z1, x2, z2);
    if (struct_id == XT_RAW_COPPER || struct_id == XT_RAW_IRON)
        return find_ore_vein(struct_id, x1, z1, x2, z2);

    /* 大类或某个变体：解析出用于区域定位的大类 ID */
    base_id = (struct_id >= FEATURE_NUM) ? ext_base_of(struct_id) : struct_id;
    if (base_id < 0) return 0;
    if (base_id == Stronghold) return find_stronghold(x1, z1, x2, z2);

    /* 取得该结构在此版本下的 region 尺寸（以 chunk 为单位） */
    if (!getStructureConfig(base_id, g_version, &sconf)) return 0;
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
    if (ensure_struct_capacity(capacity) != 0) return 0;

    for (rz = regZ1; rz <= regZ2 && count < capacity; rz++) {
        for (rx = regX1; rx <= regX2 && count < capacity; rx++) {
            /* 每个 region 至多给出一个生成尝试位置 */
            if (!getStructurePos(base_id, g_version, g.seed, rx, rz, &pos))
                continue;
            /* 过滤落在包围盒之外的候选 */
            if (pos.x < x1 || pos.x > x2 || pos.z < z1 || pos.z > z2)
                continue;
            /* 校验群系/地形，剔除“幽灵结构” */
            if (!isViableStructurePos(base_id, &g, pos.x, pos.z, 0))
                continue;
            /* 判定该实例的具体变体；查询指定变体时过滤不匹配项 */
            variant = classify_variant(base_id, pos.x, pos.z);
            if (struct_id != base_id && variant != struct_id)
                continue;
            struct_buf[count].x = pos.x;
            struct_buf[count].z = pos.z;
            struct_buf[count].variant = variant;
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
const char *struct_to_str(int stype)
{
    const char *ext = ext_name_of(stype);   /* 变体/探测扩展 ID 优先 */
    return ext ? ext : struct2str(stype);
}

/*
 * 遍历 enum StructureType，把 mc 版本下 find_structures 可用的结构 ID 写入缓冲区，
 * 返回数量。
 */
EMSCRIPTEN_KEEPALIVE
int get_supported_structures(int mc)
{
    StructureConfig sconf;
    int stype, count = 0;
    size_t i;
    int total = FEATURE_NUM + XT_NUM;   /* 足以容纳大类 + 全部变体/探测项 */

    if (struct_id_buf_len < total) {
        free(struct_id_buf);
        struct_id_buf = (int *)malloc((size_t)total * sizeof(int));
        if (!struct_id_buf) { struct_id_buf_len = 0; return 0; }
        struct_id_buf_len = total;
    }

    /* 1) 可区域定位的大类结构 */
    for (stype = 0; stype < FEATURE_NUM; stype++) {
        if (!struct2str(stype)) continue;                     /* 无资源名，跳过 */
        if (!getStructureConfig(stype, mc, &sconf)) continue; /* 该版本不支持 */
        struct_id_buf[count++] = stype;
    }

    /* 2) 要塞（非 region 型，find_structures 内走螺旋定位特判） */
    if (mc >= MC_B1_8)
        struct_id_buf[count++] = Stronghold;

    /* 3) 结构变体：其大类在本版本可用时一并列出 */
    for (i = 0; i < sizeof(ext_variants) / sizeof(ext_variants[0]); i++) {
        if (!struct2str(ext_variants[i].base)) continue;
        if (!getStructureConfig(ext_variants[i].base, mc, &sconf)) continue;
        struct_id_buf[count++] = ext_variants[i].ext;
    }

    /* 4) 非区域探测项（史莱姆区块 / 出生点，粗铜块 / 粗铁块仅 1.18+） */
    struct_id_buf[count++] = XT_SLIME;
    struct_id_buf[count++] = XT_SPAWN;
    if (mc >= MC_1_18) {
        struct_id_buf[count++] = XT_RAW_COPPER;
        struct_id_buf[count++] = XT_RAW_IRON;
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
