/* =============================================================================
 * svc/registry.c - 服务注册表 + manifest 解析 + 拓扑排序 + 生命周期引擎
 *
 * 设计基线：docs/design/three-layer.md §5（manifest）/ §7（故障与恢复）。
 *
 * 关键决策（见 docs/claims/svc.md）：
 *   1. 本表与 svc/service.c 的旧表并存；SYS_SVC_* 走本表，旧 service_* API
 *      保持原语义（legacy 回退路径不受影响）。
 *   2. register 权限：root（per-process CAP_SYS_ADMIN 位图尚未落地，判定点
 *      集中在 svc_register_allowed()，后续接上 cap 位图只改这一处）。
 *   3. 服务死亡判定：proc_find(pid) 为空或状态是 ZOMBIE/FREE（内核线程退出后
 *      没有父进程回收，会长期停在 ZOMBIE，不能只看 proc_find）。
 *   4. 重启时先释放旧端口：旧客户端立刻拿到 -1（-ENOENT 语义），重连要重新
 *      SYS_SVC_LOOKUP；新实例注册后 lookup 返回新 port（generation 已变）。
 *   5. svcwatch 单线程驱动状态机；持锁区不调用 proc_spawn/proc_find/kprintf
 *      （会睡眠/打印），所有状态迁移走“锁内判定 + 锁外动作 + 锁内落账”。
 * ============================================================================= */
#include "registry.h"
#include "ipc.h"
#include "service.h"
#include "include/kernel.h"
#include "include/proc.h"

#define SVC_KEY_MAX      24U
#define SVC_VALUE_MAX    256U
#define SVC_CAP_ITEM_MAX 64U

/* 内置 manifest 表的归属：谁负责拉起 */
#define SVC_BOOT_NONE       0x0U
#define SVC_BOOT_BOOTSEQ    0x1U   /* bootseq 已经 spawn（fsd/blkd/pkgd/shell） */
#define SVC_BOOT_SUPERVISOR 0x2U   /* supervisor_init 按拓扑序 spawn */

typedef struct {
    const char *text;
    uint32_t    owner;
} svc_builtin_desc_t;

/* 内置 manifest（设计 §5 的格式；本阶段不做 FS 解析）。表序即同层稳定序：
 * Kahn 排序时同层取表序最前，因此计划顺序是 logd -> fsd -> netd -> pkgd ->
 * shell -> echosvc -> svcwatch。 */
static const svc_builtin_desc_t svc_builtin[] = {
    { "name=logd\n"
      "version=1.0.0\ntype=service\nentry=logd\nrequires=\n"
      "capabilities=\nrestart=always\nrestart_limit=3\nuid=0\n",
      SVC_BOOT_NONE },
    { "name=fsd\n"
      "version=1.0.0\ntype=driver\nentry=fsd\nrequires=logd\n"
      "capabilities=\nrestart=on-failure\nrestart_limit=3\nuid=0\n",
      SVC_BOOT_BOOTSEQ },
    { "name=netd\n"
      "version=1.0.0\ntype=driver\nentry=netd\nrequires=logd,fsd\n"
      "capabilities=MMIO:0xFEBC0000:0x20000,IRQ:11,DMA:65536\n"
      "restart=on-failure\nrestart_limit=3\nuid=0\n",
      SVC_BOOT_NONE },
    { "name=pkgd\n"
      "version=1.0.0\ntype=service\nentry=pkgd\nrequires=fsd\n"
      "capabilities=\nrestart=always\nrestart_limit=3\nuid=0\n",
      SVC_BOOT_BOOTSEQ },
    { "name=shell\n"
      "version=1.0.0\ntype=app\nentry=shell\nrequires=pkgd\n"
      "capabilities=\nrestart=never\nrestart_limit=3\nuid=0\n",
      SVC_BOOT_BOOTSEQ },
    { "name=echosvc\n"
      "version=1.0.0\ntype=service\nentry=echosvc\nrequires=supervisor\n"
      "capabilities=\nrestart=on-failure\nrestart_limit=3\nuid=0\n",
      SVC_BOOT_SUPERVISOR },
    { "name=svcwatch\n"
      "version=1.0.0\ntype=service\nentry=svcwatch\nrequires=supervisor\n"
      "capabilities=\nrestart=always\nrestart_limit=3\nuid=0\n",
      SVC_BOOT_SUPERVISOR },
};

#define SVC_BUILTIN_COUNT \
    ((uint32_t)(sizeof(svc_builtin) / sizeof(svc_builtin[0])))

static svc_entry_t     svc_table[SVC_TABLE_MAX];
static spinlock_t      svc_lock = SPINLOCK_INIT;
static volatile uint32_t svc_paused;      /* 自检期间暂停 svcwatch 的 tick */

/* ---------------- 基础校验 ---------------- */

static bool svc_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

bool svc_name_valid(const char *name)
{
    uint32_t n = 0;

    if (name == 0 || name[0] == '\0') return false;
    while (name[n] != '\0') {
        if (n + 1U >= SVC_NAME_MAX) return false;
        if (!svc_name_char(name[n])) return false;
        n++;
    }
    return n != 0U;
}

static bool svc_store(char *dst, uint32_t cap, const char *src, uint32_t len)
{
    if (dst == 0 || cap == 0U || len >= cap) return false;
    if (len != 0U && src != 0) memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

/* 十进制 / 0x 十六进制；遍历到底才成功（不接受 "12x"）。 */
static bool svc_parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    uint32_t base = 10U;

    if (s == 0 || out == 0 || s[0] == '\0') return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16U; s += 2; }
    if (s[0] == '\0') return false;

    for (; *s != '\0'; s++) {
        uint32_t d;

        if (*s >= '0' && *s <= '9') d = (uint32_t)(*s - '0');
        else if (base == 16U && *s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a') + 10U;
        else if (base == 16U && *s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A') + 10U;
        else return false;
        v = v * base + d;
    }
    *out = v;
    return true;
}

static uint32_t svc_backoff_of(uint32_t fail_count)
{
    uint32_t ticks;

    if (fail_count == 0U) fail_count = 1U;
    if (fail_count > 3U) fail_count = 3U;         /* 1s/2s/4s 封顶 */
    ticks = SVC_BACKOFF_BASE_TICKS << (fail_count - 1U);
    if (ticks > SVC_BACKOFF_MAX_TICKS) ticks = SVC_BACKOFF_MAX_TICKS;
    return ticks;
}

uint32_t svc_backoff_ticks(uint32_t fail_count)
{
    return svc_backoff_of(fail_count);
}

const char *svc_state_name(uint32_t state)
{
    switch (state) {
    case SVC_FREE:        return "free";
    case SVC_STARTING:    return "starting";
    case SVC_RUNNING:     return "running";
    case SVC_UNHEALTHY:   return "unhealthy";
    case SVC_FAILED:      return "failed";
    case SVC_RESTARTING:  return "restarting";
    case SVC_QUARANTINED: return "quarantined";
    case SVC_STOPPED:     return "stopped";
    default:              return "?";
    }
}

const char *svc_restart_name(uint32_t policy)
{
    switch (policy) {
    case SVC_RESTART_ON_FAILURE: return "on-failure";
    case SVC_RESTART_ALWAYS:     return "always";
    default:                     return "never";
    }
}

const char *svc_type_name(uint32_t type)
{
    switch (type) {
    case SVC_TYPE_DRIVER: return "driver";
    case SVC_TYPE_APP:    return "app";
    default:              return "service";
    }
}

uint32_t svc_restart_policy_of(const char *s)
{
    if (s == 0) return 0xFFFFFFFFU;
    if (strcmp(s, "never") == 0) return SVC_RESTART_NEVER;
    if (strcmp(s, "on-failure") == 0 || strcmp(s, "on_failure") == 0) {
        return SVC_RESTART_ON_FAILURE;
    }
    if (strcmp(s, "always") == 0) return SVC_RESTART_ALWAYS;
    return 0xFFFFFFFFU;
}

/* ---------------- manifest 解析 ---------------- */

static bool svc_entry_path_valid(const char *s)
{
    uint32_t n = 0;

    if (s == 0 || s[0] == '\0') return false;
    while (s[n] != '\0') {
        char c = s[n];

        if (n + 1U >= SVC_ENTRY_MAX) return false;
        if (!(svc_name_char(c) || c == '/')) return false;
        n++;
    }
    return n != 0U;
}

static bool svc_cap_add(svc_manifest_t *out, uint32_t type, uint32_t id,
                        uint32_t len)
{
    if (out->cap_count >= SVC_CAPS_SPEC_MAX) return false;
    out->caps[out->cap_count].type = type;
    out->caps[out->cap_count].id = id;
    out->caps[out->cap_count].len = len;
    out->cap_count++;
    return true;
}

/* capabilities= 的单项：MMIO:base:len / IRQ:n / DMA:len / CAP_*（包权限，忽略） */
static bool svc_parse_cap_item(svc_manifest_t *out, const char *item)
{
    if (strncmp(item, "MMIO:", 5) == 0) {
        const char *base = item + 5;
        const char *colon = strchr(base, ':');
        char tmp[48];
        uint32_t b, l;

        if (colon == 0) return false;
        if (!svc_store(tmp, sizeof(tmp), base, (uint32_t)(colon - base))) return false;
        if (!svc_parse_u32(tmp, &b) || b == 0U) return false;
        if (!svc_parse_u32(colon + 1, &l) || l == 0U) return false;
        return svc_cap_add(out, IPC_CAP_MMIO, b, l);
    }
    if (strncmp(item, "IRQ:", 4) == 0) {
        uint32_t irq;

        if (!svc_parse_u32(item + 4, &irq) || irq >= 256U) return false;
        return svc_cap_add(out, IPC_CAP_IRQ, irq, 0U);
    }
    if (strncmp(item, "DMA:", 4) == 0) {
        uint32_t len;

        if (!svc_parse_u32(item + 4, &len) || len == 0U) return false;
        return svc_cap_add(out, IPC_CAP_DMA, len, 0U);
    }
    if (strncmp(item, "CAP_", 4) == 0) {
        /* 包权限位（CAP_SVC_REGISTER/CAP_NET/...）不是硬件 cap，登记表不解释 */
        return true;
    }
    return false;
}

static bool svc_parse_caps(svc_manifest_t *out, const char *value)
{
    const char *p = value;

    out->cap_count = 0;
    while (*p != '\0') {
        const char *start = p;
        uint32_t len;
        char item[SVC_CAP_ITEM_MAX];

        while (*p != '\0' && *p != ',') p++;
        len = (uint32_t)(p - start);
        while (len > 0U && (*start == ' ' || *start == '\t')) { start++; len--; }
        while (len > 0U && (start[len - 1U] == ' ' || start[len - 1U] == '\t')) len--;
        if (len != 0U) {
            if (!svc_store(item, sizeof(item), start, len)) return false;
            if (!svc_parse_cap_item(out, item)) return false;
        }
        if (*p == ',') p++;
    }
    return true;
}

static bool svc_parse_requires(svc_manifest_t *out, const char *value)
{
    const char *p = value;

    out->req_count = 0;
    while (*p != '\0') {
        const char *start = p;
        uint32_t len;

        while (*p != '\0' && *p != ',') p++;
        len = (uint32_t)(p - start);
        while (len > 0U && (*start == ' ' || *start == '\t')) { start++; len--; }
        while (len > 0U && (start[len - 1U] == ' ' || start[len - 1U] == '\t')) len--;
        if (len != 0U) {
            if (out->req_count >= SVC_REQ_MAX) return false;
            if (!svc_store(out->requires[out->req_count], SVC_NAME_MAX, start, len)) {
                return false;
            }
            if (!svc_name_valid(out->requires[out->req_count])) return false;
            out->req_count++;
        }
        if (*p == ',') p++;
    }
    return true;
}

int svc_manifest_parse(const char *text, svc_manifest_t *out)
{
    const char *p = text;

    if (text == 0 || out == 0) return -1;
    memset(out, 0, sizeof(*out));
    out->type = SVC_TYPE_SERVICE;
    out->restart_policy = SVC_RESTART_ON_FAILURE;
    out->restart_limit = SVC_RESTART_LIMIT_DEFAULT;

    while (*p != '\0') {
        const char *line = p;
        const char *nl = strchr(p, '\n');
        uint32_t len;

        if (nl != 0) {
            len = (uint32_t)(nl - p);
            p = nl + 1;
        } else {
            len = (uint32_t)strlen(p);
            p += len;
        }

        while (len > 0U && (*line == ' ' || *line == '\t')) { line++; len--; }
        while (len > 0U && (line[len - 1U] == ' ' || line[len - 1U] == '\t' ||
                            line[len - 1U] == '\r')) {
            len--;
        }
        if (len == 0U || line[0] == '#' || line[0] == ';') continue;

        {
            const char *eq = 0;
            const char *val;
            uint32_t klen, vlen, i;
            char key[SVC_KEY_MAX];
            char value[SVC_VALUE_MAX];

            for (i = 0; i < len; i++) {
                if (line[i] == '=') { eq = line + i; break; }
            }
            if (eq == 0 || eq == line) return -1;

            klen = (uint32_t)(eq - line);
            while (klen > 0U && (line[klen - 1U] == ' ' || line[klen - 1U] == '\t')) klen--;
            if (klen == 0U || klen >= sizeof(key)) return -1;
            memcpy(key, line, klen);
            key[klen] = '\0';
            for (i = 0; i < klen; i++) {
                if (!svc_name_char(key[i])) return -1;
            }

            val = eq + 1;
            vlen = len - (uint32_t)(val - line);
            while (vlen > 0U && (*val == ' ' || *val == '\t')) { val++; vlen--; }
            while (vlen > 0U && (val[vlen - 1U] == ' ' || val[vlen - 1U] == '\t')) vlen--;
            if (!svc_store(value, sizeof(value), val, vlen)) return -1;

            if (strcmp(key, "name") == 0) {
                if (!svc_store(out->name, sizeof(out->name), value, vlen)) return -1;
            } else if (strcmp(key, "version") == 0) {
                if (!svc_store(out->version, sizeof(out->version), value, vlen)) return -1;
            } else if (strcmp(key, "type") == 0) {
                if (strcmp(value, "service") == 0) out->type = SVC_TYPE_SERVICE;
                else if (strcmp(value, "driver") == 0) out->type = SVC_TYPE_DRIVER;
                else if (strcmp(value, "app") == 0) out->type = SVC_TYPE_APP;
                else return -1;
            } else if (strcmp(key, "entry") == 0) {
                if (!svc_store(out->entry, sizeof(out->entry), value, vlen)) return -1;
            } else if (strcmp(key, "requires") == 0) {
                if (!svc_parse_requires(out, value)) return -1;
            } else if (strcmp(key, "capabilities") == 0) {
                if (!svc_parse_caps(out, value)) return -1;
            } else if (strcmp(key, "restart") == 0) {
                uint32_t policy = svc_restart_policy_of(value);

                if (policy == 0xFFFFFFFFU) return -1;
                out->restart_policy = policy;
            } else if (strcmp(key, "restart_limit") == 0) {
                uint32_t lim;

                if (!svc_parse_u32(value, &lim) || lim > 16U) return -1;
                out->restart_limit = lim;
            } else if (strcmp(key, "uid") == 0) {
                if (!svc_parse_u32(value, &out->uid)) return -1;
            } else {
                return -1;          /* 未知键：宁可拒绝，避免 manifest 被静默忽略 */
            }
        }
    }

    if (!svc_name_valid(out->name)) return -1;
    if (out->version[0] != '\0') {
        uint32_t vlen = (uint32_t)strlen(out->version);
        uint32_t i;

        if (vlen >= SVC_VERSION_MAX) return -1;
        for (i = 0; i < vlen; i++) {
            if (!svc_name_char(out->version[i])) return -1;
        }
    }
    if (out->entry[0] == '\0') {
        memcpy(out->entry, out->name, SVC_NAME_MAX);
    } else if (!svc_entry_path_valid(out->entry)) {
        return -1;
    }
    return 0;
}

/* ---------------- 拓扑排序 ---------------- */

int svc_topo_sort(const svc_manifest_t *nodes, uint32_t n,
                  svc_provided_fn provided,
                  uint32_t *order, uint32_t *ordered, uint32_t *skipped)
{
    int dep_idx[SVC_TABLE_MAX][SVC_REQ_MAX];
    bool done[SVC_TABLE_MAX];
    bool dropped[SVC_TABLE_MAX];
    bool missing[SVC_TABLE_MAX];
    uint32_t done_n = 0;
    uint32_t skipped_n = 0;
    uint32_t i, j;

    if (nodes == 0 || order == 0 || ordered == 0 || skipped == 0) return -1;
    if (n == 0U || n > SVC_TABLE_MAX) return -1;

    for (i = 0; i < n; i++) {
        done[i] = false;
        dropped[i] = false;
        missing[i] = false;
        for (j = 0; j < SVC_REQ_MAX; j++) dep_idx[i][j] = -1;

        for (j = 0; j < nodes[i].req_count && j < SVC_REQ_MAX; j++) {
            const char *dep = nodes[i].requires[j];
            int k = -1;
            uint32_t x;

            for (x = 0; x < n; x++) {
                if (strcmp(nodes[x].name, dep) == 0) { k = (int)x; break; }
            }
            dep_idx[i][j] = k;
            if (k < 0) {
                if (provided == 0 || !provided(dep)) missing[i] = true;
            }
        }
    }

    for (;;) {
        bool progress = false;

        /* 1) 依赖缺失/已丢弃的节点跟着跳过（requires 缺失 -> 报错并跳过）。 */
        for (i = 0; i < n; i++) {
            if (done[i] || dropped[i]) continue;
            if (missing[i]) {
                dropped[i] = true;
                skipped_n++;
                progress = true;
                continue;
            }
            for (j = 0; j < nodes[i].req_count && j < SVC_REQ_MAX; j++) {
                int k = dep_idx[i][j];

                if (k >= 0 && dropped[k]) {
                    dropped[i] = true;
                    skipped_n++;
                    progress = true;
                    break;
                }
            }
        }

        /* 2) 取表序最前的就绪节点入序（同一层稳定顺序）。 */
        for (i = 0; i < n; i++) {
            bool ready = true;

            if (done[i] || dropped[i]) continue;
            for (j = 0; j < nodes[i].req_count && j < SVC_REQ_MAX; j++) {
                int k = dep_idx[i][j];

                if (k >= 0 && !done[k]) { ready = false; break; }
            }
            if (!ready) continue;
            done[i] = true;
            order[done_n++] = i;
            progress = true;
            break;
        }

        if (!progress) break;
    }

    *ordered = done_n;
    *skipped = skipped_n;
    if (done_n + skipped_n < n) return -1;      /* 剩下的是环 */
    return 0;
}

static bool svc_builtin_provided(const char *name)
{
    return service_exists(name) || svc_registry_contains(name);
}

static int svc_builtin_parse(svc_manifest_t *out, uint32_t max)
{
    uint32_t i;

    if (out == 0 || max < SVC_BUILTIN_COUNT) return -1;
    for (i = 0; i < SVC_BUILTIN_COUNT; i++) {
        if (svc_manifest_parse(svc_builtin[i].text, &out[i]) != 0) return -1;
    }
    return (int)SVC_BUILTIN_COUNT;
}

int svc_builtin_order(char (*names)[SVC_NAME_MAX], uint32_t max)
{
    svc_manifest_t nodes[SVC_TABLE_MAX];
    uint32_t order[SVC_TABLE_MAX];
    uint32_t n, ordered = 0, skipped = 0, i;
    int rc = svc_builtin_parse(nodes, SVC_TABLE_MAX);

    if (names == 0 || max == 0U || rc < 0) return -1;
    n = (uint32_t)rc;
    if (svc_topo_sort(nodes, n, svc_builtin_provided, order, &ordered,
                      &skipped) != 0) {
        return -1;
    }
    if (ordered > max) return -1;
    for (i = 0; i < ordered; i++) {
        memcpy(names[i], nodes[order[i]].name, SVC_NAME_MAX);
    }
    return (int)ordered;
}

int svc_startup_mode(void)
{
    if (boot_cmdline_has("services=legacy")) return SVC_START_LEGACY;
    /* 缺省走拓扑启动；services=manifest 显式打开（与设计 §9 的参数一致）。 */
    return SVC_START_MANIFEST;
}

int svc_topology_start(int mode)
{
    svc_manifest_t nodes[SVC_TABLE_MAX];
    uint32_t order[SVC_TABLE_MAX];
    uint32_t n, ordered = 0, skipped = 0, i;
    uint32_t started = 0;
    char plan[192];
    char started_names[96];
    uint32_t plen = 0;
    uint32_t slen = 0;
    int rc;

    if (mode == SVC_START_LEGACY) {
        kprintln("svc: topology legacy (services=legacy)");
        return 0;
    }

    rc = svc_builtin_parse(nodes, SVC_TABLE_MAX);
    if (rc < 0) {
        kprintln("svc: topology FAILED (builtin manifest parse)");
        return -1;
    }
    n = (uint32_t)rc;
    if (svc_topo_sort(nodes, n, svc_builtin_provided, order, &ordered,
                      &skipped) != 0) {
        kprintln("svc: topology FAILED (requires cycle or unresolved dependency)");
        return -1;
    }

    plan[0] = '\0';
    for (i = 0; i < ordered; i++) {
        const char *nm = nodes[order[i]].name;
        uint32_t l = (uint32_t)strlen(nm);

        if (plen + l + 4U >= sizeof(plan)) break;
        if (plen != 0U) {
            plan[plen++] = '-';
            plan[plen++] = '>';
        }
        memcpy(plan + plen, nm, l);
        plen += l;
        plan[plen] = '\0';
    }
    kprintf("svc: topology plan ok (%s)\n", plan);
    if (skipped != 0U) {
        kprintf("svc: topology skipped %u node(s) (missing requires)\n",
                (unsigned)skipped);
    }

    started_names[0] = '\0';
    for (i = 0; i < ordered; i++) {
        uint32_t idx = order[i];
        const char *nm = nodes[idx].name;
        const char *entry = (nodes[idx].entry[0] != '\0') ? nodes[idx].entry : nm;
        proc_t *p;

        if (svc_builtin[idx].owner == SVC_BOOT_BOOTSEQ) continue;  /* bootseq 负责 */
        if (svc_builtin[idx].owner != SVC_BOOT_SUPERVISOR) {
            /* 本阶段还没有 ring3 程序的服务（logd/netd）：保留拓扑位置，跳过启动 */
            kprintf("svc: topology skip %s (no program)\n", nm);
            continue;
        }
        if (service_exists(nm) || svc_registry_contains(nm)) {
            kprintf("svc: topology %s already running\n", nm);
            continue;
        }
        p = proc_spawn(entry);
        if (p == 0) {
            kprintf("svc: topology start %s FAILED\n", nm);
            continue;
        }
        started++;
        if (slen + strlen(nm) + 3U < sizeof(started_names)) {
            if (slen != 0U) {
                started_names[slen++] = ',';
                started_names[slen++] = ' ';
            }
            memcpy(started_names + slen, nm, strlen(nm));
            slen += (uint32_t)strlen(nm);
            started_names[slen] = '\0';
        }
    }

    if (started != 0U) kprintf("svc: topology start ok (%s)\n", started_names);
    else kprintln("svc: topology start ok (nothing to spawn)");
    return (int)started;
}

/* ---------------- 注册表 ---------------- */

static int svc_find_locked(const char *name)
{
    uint32_t i;

    if (name == 0) return -1;
    for (i = 0; i < SVC_TABLE_MAX; i++) {
        if (svc_table[i].used && strcmp(svc_table[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int svc_find_port_locked(uint32_t port)
{
    uint32_t i;

    if (port == 0U) return -1;
    for (i = 0; i < SVC_TABLE_MAX; i++) {
        if (svc_table[i].used && svc_table[i].port == port) return (int)i;
    }
    return -1;
}

static uint32_t svc_caps_mask(const svc_manifest_t *m)
{
    uint32_t mask = 0;
    uint32_t i;

    for (i = 0; i < m->cap_count; i++) {
        uint32_t type = m->caps[i].type;

        if (type >= IPC_CAP_PORT && type <= IPC_CAP_SOCKET) mask |= (1U << type);
    }
    return mask;
}

bool svc_register_allowed(const struct process *p)
{
    if (p == 0) return false;
    /* CAP_SYS_ADMIN 的 per-process 位图尚未落地（P1 已记录同一限制）；
     * 判定点集中在这里，接上位图后只改这一行。 */
    return proc_is_root(p);
}

int svc_registry_register(const svc_manifest_t *m, uint32_t port,
                          struct process *caller)
{
    uint32_t f, i, now;
    int idx;

    if (m == 0 || caller == 0) return -1;
    if (!svc_register_allowed(caller)) return -1;
    if (!svc_name_valid(m->name) || port == 0U) return -1;

    now = timer_ticks();
    f = spin_lock_irqsave(&svc_lock);

    idx = svc_find_locked(m->name);
    if (idx >= 0) {
        svc_entry_t *e = &svc_table[idx];

        /* 只有“重启中”的表项允许新实例接管；其余按重复注册拒绝。 */
        if (e->state != SVC_STARTING) {
            spin_unlock_irqrestore(&svc_lock, f);
            return -1;
        }
        if (svc_find_port_locked(port) >= 0) {
            spin_unlock_irqrestore(&svc_lock, f);
            return -1;
        }
        e->port = port;
        e->pid = caller->pid;
        e->uid = proc_euid_of(caller);
        e->state = SVC_RUNNING;
        e->last_heartbeat = now;
        e->start_at = now;
        e->type = m->type;
        e->restart_policy = m->restart_policy;
        e->restart_limit = m->restart_limit;
        e->caps_mask = svc_caps_mask(m);
        memcpy(e->version, m->version, SVC_VERSION_MAX);
        memcpy(e->entry, m->entry, SVC_ENTRY_MAX);
        e->req_count = m->req_count;
        for (i = 0; i < SVC_REQ_MAX; i++) {
            memcpy(e->requires[i], m->requires[i], SVC_NAME_MAX);
        }
        spin_unlock_irqrestore(&svc_lock, f);
        return 0;
    }

    for (i = 0; i < SVC_TABLE_MAX; i++) {
        if (!svc_table[i].used) break;
    }
    if (i == SVC_TABLE_MAX) {
        spin_unlock_irqrestore(&svc_lock, f);
        return -1;
    }
    if (svc_find_port_locked(port) >= 0) {
        spin_unlock_irqrestore(&svc_lock, f);
        return -1;
    }

    {
        svc_entry_t *e = &svc_table[i];

        memset(e, 0, sizeof(*e));
        e->used = true;
        memcpy(e->name, m->name, SVC_NAME_MAX);
        memcpy(e->version, m->version, SVC_VERSION_MAX);
        memcpy(e->entry, m->entry, SVC_ENTRY_MAX);
        e->type = m->type;
        e->port = port;
        e->pid = caller->pid;
        e->uid = proc_euid_of(caller);
        e->caps_mask = svc_caps_mask(m);
        e->state = SVC_RUNNING;
        e->restart_policy = m->restart_policy;
        e->restart_limit = m->restart_limit;
        e->fail_count = 0;
        e->restarts = 0;
        e->last_heartbeat = now;
        e->start_at = now;
        e->req_count = m->req_count;
        for (i = 0; i < SVC_REQ_MAX; i++) {
            memcpy(e->requires[i], m->requires[i], SVC_NAME_MAX);
        }
    }
    spin_unlock_irqrestore(&svc_lock, f);
    return 0;
}

int svc_registry_lookup(const char *name, uint32_t *port)
{
    uint32_t f, p = 0;
    bool found = false;
    int idx;

    if (name == 0 || port == 0) return -2;

    f = spin_lock_irqsave(&svc_lock);
    idx = svc_find_locked(name);
    if (idx >= 0) {
        const svc_entry_t *e = &svc_table[idx];

        if (e->state == SVC_RUNNING && e->port != 0U) {
            p = e->port;
            found = true;
        }
    }
    spin_unlock_irqrestore(&svc_lock, f);

    if (!found) return -2;                  /* 不存在/重启中/隔离：客户端重连 */
    if (!ipc_port_exists(p)) return -2;     /* 端口已被回收：旧句柄不能再用 */
    *port = p;
    return 0;
}

int svc_registry_heartbeat(uint32_t port, struct process *caller)
{
    uint32_t f;
    int idx;

    if (port == 0U || caller == 0) return -1;

    f = spin_lock_irqsave(&svc_lock);
    idx = svc_find_port_locked(port);
    if (idx < 0) {
        spin_unlock_irqrestore(&svc_lock, f);
        return -1;
    }
    /* 心跳只能由服务自身的进程发（伪造别的服务的 port 一律拒绝） */
    if (svc_table[idx].pid != caller->pid) {
        spin_unlock_irqrestore(&svc_lock, f);
        return -1;
    }
    svc_table[idx].last_heartbeat = timer_ticks();
    if (svc_table[idx].state == SVC_UNHEALTHY) {
        svc_table[idx].state = SVC_RUNNING;
    }
    spin_unlock_irqrestore(&svc_lock, f);
    return 0;
}

void svc_registry_remove(const char *name)
{
    uint32_t f;
    int idx;

    if (name == 0) return;
    f = spin_lock_irqsave(&svc_lock);
    idx = svc_find_locked(name);
    if (idx >= 0) svc_table[idx].used = false;
    spin_unlock_irqrestore(&svc_lock, f);
}

int svc_registry_get(const char *name, svc_entry_t *out)
{
    uint32_t f;
    int idx;

    if (name == 0 || out == 0) return -2;
    f = spin_lock_irqsave(&svc_lock);
    idx = svc_find_locked(name);
    if (idx >= 0) memcpy(out, &svc_table[idx], sizeof(*out));
    spin_unlock_irqrestore(&svc_lock, f);
    return (idx >= 0) ? 0 : -2;
}

bool svc_registry_contains(const char *name)
{
    uint32_t f;
    int idx;

    f = spin_lock_irqsave(&svc_lock);
    idx = svc_find_locked(name);
    spin_unlock_irqrestore(&svc_lock, f);
    return idx >= 0;
}

uint32_t svc_registry_count(void)
{
    uint32_t f, n = 0, i;

    f = spin_lock_irqsave(&svc_lock);
    for (i = 0; i < SVC_TABLE_MAX; i++) {
        if (svc_table[i].used) n++;
    }
    spin_unlock_irqrestore(&svc_lock, f);
    return n;
}

/* ---------------- 生命周期引擎 ---------------- */

/* 进程是否已经消失：空槽或 ZOMBIE 都算死（内核线程退出后没人回收，会停在
 * ZOMBIE，所以不能只判断 proc_find == 0）。 */
static bool svc_process_gone(uint32_t pid)
{
    proc_t *p;

    if (pid == 0U) return true;
    p = proc_find(pid);
    if (p == 0) return true;
    return p->state == PROC_ZOMBIE || p->state == PROC_FREE;
}

/* 记一次失败并落账（锁内判定 + 锁外释放端口/打印）。 */
static void svc_note_failure_at(const char *name, const char *reason, uint32_t now)
{
    uint32_t f, old_port = 0, fail = 0, backoff = 0, state = 0;
    char cname[SVC_NAME_MAX];
    int idx;

    if (name == 0) return;
    cname[0] = '\0';

    f = spin_lock_irqsave(&svc_lock);
    idx = svc_find_locked(name);
    if (idx < 0) {
        spin_unlock_irqrestore(&svc_lock, f);
        return;
    }
    {
        svc_entry_t *e = &svc_table[idx];

        if (e->state == SVC_QUARANTINED || e->state == SVC_STOPPED) {
            spin_unlock_irqrestore(&svc_lock, f);
            return;
        }
        e->fail_count++;
        old_port = e->port;
        e->port = 0U;
        e->pid = 0U;
        fail = e->fail_count;
        memcpy(cname, e->name, SVC_NAME_MAX);

        if (e->restart_policy == SVC_RESTART_NEVER) {
            e->state = SVC_STOPPED;
        } else if (fail > e->restart_limit) {
            e->state = SVC_QUARANTINED;
        } else {
            backoff = svc_backoff_of(fail);
            e->state = SVC_RESTARTING;
            e->restart_at = now + backoff;
        }
        state = e->state;
    }
    spin_unlock_irqrestore(&svc_lock, f);

    /* 旧端口立刻回收：旧客户端下一次 SEND/LOOKUP 就失败，必须重新 lookup。 */
    if (old_port != 0U) ipc_port_release(old_port, proc_current());

    if (state == SVC_QUARANTINED) {
        kprintf("svc: quarantine %s after %u failure(s)\n", cname,
                (unsigned)fail);
    } else if (state == SVC_STOPPED) {
        kprintf("svc: %s failed (%s), restart=never -> stopped\n", cname,
                (reason != 0) ? reason : "?");
    } else {
        kprintf("svc: %s failed (%s), fail_count=%u -> restart in %u tick(s)\n",
                cname, (reason != 0) ? reason : "?", (unsigned)fail,
                (unsigned)backoff);
    }
}

int svc_registry_note_failure(const char *name, const char *reason)
{
    if (name == 0) return -1;
    svc_note_failure_at(name, reason, timer_ticks());
    return 0;
}

void svc_supervise_pause(bool paused)
{
    svc_paused = paused ? 1U : 0U;
}

void svc_supervise_tick(uint32_t now)
{
    if (svc_paused) return;
    svc_supervise_step(now);
}

void svc_supervise_step(uint32_t now)
{
    uint32_t i;

    for (i = 0; i < SVC_TABLE_MAX; i++) {
        svc_entry_t snap;
        uint32_t f;
        bool used;

        f = spin_lock_irqsave(&svc_lock);
        used = svc_table[i].used;
        if (used) snap = svc_table[i];
        spin_unlock_irqrestore(&svc_lock, f);
        if (!used) continue;

        switch (snap.state) {
        case SVC_RUNNING:
            if (svc_process_gone(snap.pid)) {
                svc_note_failure_at(snap.name, "process gone", now);
            } else if ((int32_t)(now - snap.last_heartbeat) >=
                       (int32_t)SVC_HEARTBEAT_TIMEOUT_TICKS) {
                bool marked = false;
                uint32_t age = now - snap.last_heartbeat;

                f = spin_lock_irqsave(&svc_lock);
                if (svc_table[i].used && svc_table[i].state == SVC_RUNNING &&
                    svc_table[i].last_heartbeat == snap.last_heartbeat) {
                    svc_table[i].state = SVC_UNHEALTHY;
                    svc_table[i].unhealthy_at = now;
                    marked = true;
                }
                spin_unlock_irqrestore(&svc_lock, f);
                if (marked) {
                    kprintf("svc: heartbeat timeout %s (age=%u ticks) -> unhealthy\n",
                            snap.name, (unsigned)age);
                }
            }
            break;

        case SVC_UNHEALTHY: {
            uint32_t f2;

            if (snap.restart_policy == SVC_RESTART_NEVER) break;   /* 只报不重启 */

            if (!svc_process_gone(snap.pid)) {
                proc_t *p = proc_find(snap.pid);

                /* ring3 服务可以 SIGKILL 掉再重启；内核线程服务杀不掉，等宽限期
                 * 后直接隔离（见 claims 的设计问题：缺 proc_kill）。 */
                if (p != 0 && p->is_user) {
                    proc_send_signal(p, SIGKILL);
                }
                if ((int32_t)(now - snap.unhealthy_at) <
                    (int32_t)SVC_UNHEALTHY_GRACE_TICKS) {
                    break;
                }
                {
                    bool quarantined = false;
                    uint32_t old_port = 0;

                    f2 = spin_lock_irqsave(&svc_lock);
                    if (svc_table[i].used &&
                        svc_table[i].state == SVC_UNHEALTHY &&
                        svc_table[i].pid == snap.pid) {
                        old_port = svc_table[i].port;
                        svc_table[i].state = SVC_QUARANTINED;
                        svc_table[i].port = 0U;
                        quarantined = true;
                    }
                    spin_unlock_irqrestore(&svc_lock, f2);
                    if (old_port != 0U) ipc_port_release(old_port, proc_current());
                    if (quarantined) {
                        kprintf("svc: quarantine %s (unhealthy, cannot terminate pid %u)\n",
                                snap.name, (unsigned)snap.pid);
                    }
                }
                break;
            }
            svc_note_failure_at(snap.name, "heartbeat timeout", now);
            break;
        }

        case SVC_STARTING:
            if (svc_process_gone(snap.pid)) {
                svc_note_failure_at(snap.name, "exited before register", now);
            } else if ((int32_t)(now - snap.start_at) >=
                       (int32_t)SVC_START_TIMEOUT_TICKS) {
                svc_note_failure_at(snap.name, "register timeout", now);
            }
            break;

        case SVC_RESTARTING: {
            proc_t *np;
            if ((int32_t)(now - snap.restart_at) < 0) break;

            {
                uint32_t f3 = spin_lock_irqsave(&svc_lock);
                bool go = svc_table[i].used &&
                          svc_table[i].state == SVC_RESTARTING &&
                          svc_table[i].restart_at == snap.restart_at;

                if (go) svc_table[i].state = SVC_FAILED;  /* 占位：防重复 spawn */
                spin_unlock_irqrestore(&svc_lock, f3);
                if (!go) break;
            }

            np = proc_spawn(snap.entry[0] != '\0' ? snap.entry : snap.name);

            {
                uint32_t f3 = spin_lock_irqsave(&svc_lock);
                svc_entry_t *e = &svc_table[i];
                bool log_ok = false, log_fail = false, log_quar = false;
                uint32_t log_pid = 0, log_fail_count = 0, log_backoff = 0;

                if (e->used && e->state == SVC_FAILED) {
                    if (np != 0) {
                        e->pid = np->pid;
                        e->port = 0U;
                        e->state = SVC_STARTING;
                        e->start_at = now;
                        e->restarts++;
                        log_pid = np->pid;
                        log_fail_count = e->fail_count + 1U;
                        log_ok = true;
                    } else {
                        e->fail_count++;
                        log_fail_count = e->fail_count;
                        if (e->fail_count > e->restart_limit) {
                            e->state = SVC_QUARANTINED;
                            log_quar = true;
                        } else {
                            log_backoff = svc_backoff_of(e->fail_count);
                            e->state = SVC_RESTARTING;
                            e->restart_at = now + log_backoff;
                            log_fail = true;
                        }
                    }
                }
                spin_unlock_irqrestore(&svc_lock, f3);

                if (log_ok) {
                    kprintf("svc: restart %s -> pid %u (attempt %u)\n",
                            snap.name, (unsigned)log_pid,
                            (unsigned)log_fail_count);
                } else if (log_quar) {
                    kprintf("svc: quarantine %s after %u failure(s)\n",
                            snap.name, (unsigned)log_fail_count);
                } else if (log_fail) {
                    kprintf("svc: restart %s spawn failed, retry in %u tick(s)\n",
                            snap.name, (unsigned)log_backoff);
                }
            }
            break;
        }

        default:
            break;
        }
    }
}
