/*
 * bridge.c —— cubiomes -> JS 的批处理桥接层
 *
 * 关于种子：Emscripten 导出的 C 函数以 int64 作为参数在 JS 侧调用不便，
 * 因此 set_seed 拆成 (mc_version, seed_hi, seed_lo) 两个 32 位整数，在 C 侧重建 uint64。
 */

#include "generator.h"
#include "biomes.h"
#include "finders.h"
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

/* 初始化 Generator 并应用种子。scale 采用 1:4（地图缩略常用比例）。 */
EMSCRIPTEN_KEEPALIVE
void set_seed(int mc_version, int seed_hi, int seed_lo)
{
    uint64_t seed = ((uint64_t)(uint32_t)seed_hi << 32) | (uint32_t)seed_lo;

    if (!g_ready || g_version != mc_version) {
        /* flags=0：默认地表生成器（新版 setupGenerator 返回 void，无错误码） */
        setupGenerator(&g, mc_version, 0);
        g_ready = 1;
        g_version = mc_version;
    }
    applySeed(&g, DIM_OVERWORLD, seed);
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
