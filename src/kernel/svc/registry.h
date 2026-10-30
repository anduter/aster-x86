#ifndef SVC_REGISTRY_H
#define SVC_REGISTRY_H

/* =============================================================================
 * svc/registry.h - 服务注册表 + manifest + 生命周期（P3 服务注册/重启）
 *
 * 契约来源：docs/design/three-layer.md §5（manifest）与 §7（故障与恢复）。
 * 与 svc/service.c 的“name -> port 连接端点”旧表并存：
 *   - service.c 只登记 native ABI 的 PORT_CONNECT/ACCEPT 端点（旧路径回退）；
 *   - 本模块是 ABI 1.5 的 SYS_SVC_REGISTER/LOOKUP/HEARTBEAT（86..88）后端，
 *     表项带版本/类型/pid/caps/状态/重启策略/失败计数/心跳时间。
 *
 * 状态机（svcwatch 线程驱动，见 svc_supervise_tick）：
 *
 *   FREE --register--> RUNNING --心跳超时--> UNHEALTHY
 *     ^                  |                     |
 *     |            进程消失/心跳超时       策略 != never
 *     |                  v                     v
 *     |              RESTARTING --spawn--> STARTING --re-register--> RUNNING
 *     |                  |                     |
 *     |              失败次数 > limit      注册超时/进程退出
 *     |                  v                     v
 *     +-------------- QUARANTINED <-----------+
 *
 * 退避：1s/2s/4s（100/200/400 tick，100Hz），第 limit(3)+1 次失败后隔离。
 * ============================================================================= */

#include "../include/types.h"

#define SVC_NAME_MAX       32U
#define SVC_VERSION_MAX    16U
#define SVC_ENTRY_MAX      32U
#define SVC_REQ_MAX        4U
#define SVC_CAPS_SPEC_MAX  8U
#define SVC_MANIFEST_MAX   1024U
#define SVC_TABLE_MAX      16U

/* manifest 的 type= */
#define SVC_TYPE_SERVICE 0U
#define SVC_TYPE_DRIVER  1U
#define SVC_TYPE_APP     2U

/* 表项状态 */
#define SVC_FREE        0U
#define SVC_STARTING    1U   /* 已 spawn，等新实例注册 */
#define SVC_RUNNING     2U
#define SVC_UNHEALTHY   3U   /* 心跳超时，等 svcwatch 处理 */
#define SVC_FAILED      4U   /* 保留（失败已转 RESTARTING/STOPPED） */
#define SVC_RESTARTING  5U   /* 退避等待中 */
#define SVC_QUARANTINED 6U   /* 超过重启上限，不再自动拉起 */
#define SVC_STOPPED     7U   /* restart=never 的失败终态 */

/* manifest 的 restart= */
#define SVC_RESTART_NEVER      0U
#define SVC_RESTART_ON_FAILURE 1U
#define SVC_RESTART_ALWAYS     2U

/* 时间参数（1 tick = 10ms @ 100Hz） */
#define SVC_HEARTBEAT_TIMEOUT_TICKS 500U  /* 5s 无心跳 -> unhealthy */
#define SVC_START_TIMEOUT_TICKS     300U  /* 3s 内必须重新注册 */
#define SVC_UNHEALTHY_GRACE_TICKS   100U  /* 判 unhealthy 后等进程自己退出 */
#define SVC_BACKOFF_BASE_TICKS      100U  /* 1s */
#define SVC_BACKOFF_MAX_TICKS       400U  /* 4s */
#define SVC_RESTART_LIMIT_DEFAULT   3U

struct process;

/* capabilities= 里的硬件 cap 规格（MMIO:base:len / IRQ:n / DMA:len） */
typedef struct {
    uint32_t type;   /* IPC_CAP_* */
    uint32_t id;     /* MMIO 基址 / IRQ 号 / DMA 长度 */
    uint32_t len;    /* MMIO 长度；其它为 0 */
} svc_cap_spec_t;

/* 解析后的 manifest（内核侧；FS 解析留到后续阶段，先只认文本） */
typedef struct {
    char     name[SVC_NAME_MAX];
    char     version[SVC_VERSION_MAX];
    char     entry[SVC_ENTRY_MAX];
    uint32_t type;
    uint32_t restart_policy;
    uint32_t restart_limit;
    uint32_t uid;
    uint32_t cap_count;
    svc_cap_spec_t caps[SVC_CAPS_SPEC_MAX];
    uint32_t req_count;
    char     requires[SVC_REQ_MAX][SVC_NAME_MAX];
} svc_manifest_t;

/* 注册表项（快照可整体拷出，调用方不需要持锁） */
typedef struct {
    bool     used;
    char     name[SVC_NAME_MAX];
    char     version[SVC_VERSION_MAX];
    char     entry[SVC_ENTRY_MAX];
    uint32_t type;
    uint32_t port;
    uint32_t pid;
    uint32_t uid;
    uint32_t caps_mask;      /* 已声明 cap 的类型位图（1<<type） */
    uint32_t state;
    uint32_t restart_policy;
    uint32_t restart_limit;
    uint32_t fail_count;
    uint32_t restarts;
    uint32_t last_heartbeat;
    uint32_t restart_at;
    uint32_t start_at;
    uint32_t unhealthy_at;
    uint32_t req_count;
    char     requires[SVC_REQ_MAX][SVC_NAME_MAX];
} svc_entry_t;

/* ---- manifest ---- */
/* 解析文本 manifest（格式见设计 §5）：返回 0 / -1（未知键/坏数字/坏名字）。 */
int svc_manifest_parse(const char *text, svc_manifest_t *out);
bool svc_name_valid(const char *name);
const char *svc_state_name(uint32_t state);
const char *svc_restart_name(uint32_t policy);
const char *svc_type_name(uint32_t type);
uint32_t svc_restart_policy_of(const char *s);      /* -1 语义用 0xFFFFFFFF */
uint32_t svc_backoff_ticks(uint32_t fail_count);

/* ---- 注册表 ---- */
/* 权限：root（CAP_SYS_ADMIN 位图未落地，与 P1 的 MMIO/DMA/IRQ 判定一致）。 */
bool svc_register_allowed(const struct process *p);
int  svc_registry_register(const svc_manifest_t *m, uint32_t port,
                           struct process *caller);   /* 0 / -1 */
int  svc_registry_lookup(const char *name, uint32_t *port); /* 0 / -2 */
int  svc_registry_heartbeat(uint32_t port, struct process *caller); /* 0 / -1 */
void svc_registry_remove(const char *name);          /* 自检/卸载清理 */
int  svc_registry_get(const char *name, svc_entry_t *out);  /* 0 / -2 */
bool svc_registry_contains(const char *name);
uint32_t svc_registry_count(void);

/* ---- 生命周期引擎 ---- */
/* svcwatch 周期调用（自检互斥期间直接返回）。 */
void svc_supervise_tick(uint32_t now);
/* 不受自检互斥影响：自检用确定的 now 驱动状态机。 */
void svc_supervise_step(uint32_t now);
/* 显式上报一次失败（进程退出/心跳超时/测试注入）。延迟到下一个 tick 重启。 */
int  svc_registry_note_failure(const char *name, const char *reason);
/* 自检期间暂停 svcwatch 的 tick，避免和自检抢状态机。 */
void svc_supervise_pause(bool paused);

/* ---- 拓扑启动（内置 manifest 表，先不做 FS 解析） ---- */
#define SVC_START_LEGACY   0U    /* 旧硬编码顺序（services=legacy） */
#define SVC_START_MANIFEST 1U    /* requires 拓扑序（缺省） */

typedef bool (*svc_provided_fn)(const char *name);

/* requires 拓扑排序（Kahn；同层取表序最前，输出稳定）。
 * 返回 0 成功：order 收下标、*ordered 收个数、*skipped 收“缺依赖被跳过”的个数；
 * 返回 -1：n 非法或有环（order 里是已排出的无环前缀）。 */
int svc_topo_sort(const svc_manifest_t *nodes, uint32_t n,
                  svc_provided_fn provided,
                  uint32_t *order, uint32_t *ordered, uint32_t *skipped);

/* 启动模式：boot cmdline 的 services=legacy / services=manifest（缺省 manifest）。 */
int svc_startup_mode(void);
/* 按内置 manifest 表拓扑启动。返回启动的服务数；-1 = 计划失败。 */
int svc_topology_start(int mode);
/* 内置表的拓扑序（只算不启动），names 收名字；返回节点数 / -1。 */
int svc_builtin_order(char (*names)[SVC_NAME_MAX], uint32_t max);

/* ---- 自检（-Test 构建，supervisor 线程调度器启动后跑一次） ---- */
void svc_selftest(void);

#endif /* SVC_REGISTRY_H */
