/* =============================================================================
 * include/libsvc.h - 程序层服务客户端 SDK（用户态头，不依赖任何内核头）
 *
 * 三层架构 P7：程序层只通过 svc_* 使用服务（docs/design/three-layer.md §2）。
 * 本头只声明接口/常量/结构，实现位于 src/user/ulib.c；用户程序这样包含：
 *
 *     #include "ulib.h"                      // printf/memcpy/port_* 等
 *     #include "../kernel/include/libsvc.h"  // 本 SDK
 *
 * 提供的 API：
 *   svc_connect(name, timeout_ms)  按名连服务（timeout 内重试 lookup）
 *   svc_rpc(conn, req, len, ...)   一问一答；超时/重连语义见 §3
 *   svc_reconnect(conn)            服务重启后重新 lookup，换新端口
 *   svc_lookup_cached(name)        带进程内缓存的 lookup（失败即失效重查）
 *   svc_close(conn)                释放连接对象（当前 ABI 无 PORT_CLOSE）
 *   svc_last_error()/svc_strerror() 错误码与文本
 *
 * 1. 帧约定（payload 内，小端；外层是 16B IPC 头，见 ulib.h）
 *
 *     请求 payload:  u32 ticket | u32 req_id | req[len]
 *     回复 payload:  u32 req_id | reply[]
 *
 *   ticket = (pid<<16)|port（port_ticket()），服务用它 port_reply；req_id 由
 *   SDK 单调递增，服务必须原样回显，客户端据此丢弃超时后的迟到回复。
 *   约定与 pkgd 的 PKG2/PKG3 帧同族（docs/design/three-layer.md §3.4）。
 *
 * 2. 错误码（统一负值；SVC_OK = 0；svc_rpc 成功时返回回复长度 >= 0）
 *
 *      SVC_ERR_NOENT    服务未注册/重启中/已隔离（lookup 失败）
 *      SVC_ERR_TIMEOUT  发送队列满或等回复超时（内核 -EAGAIN 折算）
 *      SVC_ERR_IO       ABI/参数错误、端口失效且重连失败、坏帧
 *      SVC_ERR_DENIED   权限不足（注册类操作；lookup 目前不区分，按 NOENT）
 *
 * 3. 超时/重连语义
 *
 *   - 没有时钟/非阻塞 lookup，timeout_ms 按 10ms 睡眠切片与内核单次 IPC 超时
 *     （SEND 200ms / RECV 300ms）折算成重试次数，不是硬实时。
 *   - svc_connect 在 timeout_ms 内反复 lookup；超时仍没有服务 → SVC_ERR_NOENT。
 *   - svc_rpc 遇到 SEND/RECV 返回 -1（端口已被服务重启回收）时，自动调用
 *     svc_reconnect 重新 lookup 并重发一次；仍失败 → SVC_ERR_IO。
 *     语义是 at-least-once：重发可能让服务执行两次。
 *   - fork 后子进程沿用父进程的 conn 时，svc_rpc 会检测 pid 变化并先重连
 *     （客户端端口与 ticket 都是按进程的）。
 *   - svc_rpc/svc_lookup_cached 不保证可重入（每进程一份静态收发缓冲）。
 *
 * 4. 权限
 *
 *   SYS_SVC_REGISTER 目前只允许 root（CAP_SYS_ADMIN 位图未落地，见
 *   docs/claims/svc.md §7）；客户端 lookup/RPC 不受限，但服务端可以按
 *   port flags（私有端口仅同 uid）决定谁能发。示例见 src/user/demoapp.c。
 * ========================================================================== */
#ifndef OS_INCLUDE_LIBSVC_H
#define OS_INCLUDE_LIBSVC_H

/* ---- 统一错误码 ---- */
#define SVC_OK              0
#define SVC_ERR_NOENT     (-1)
#define SVC_ERR_TIMEOUT   (-2)
#define SVC_ERR_IO        (-3)
#define SVC_ERR_DENIED    (-4)

/* ---- 帧/缓存规模 ---- */
#define SVC_NAME_MAX        32      /* 服务名 + NUL；与内核 SVC_NAME_MAX 一致 */
#define SVC_REQ_HDR_SIZE     8      /* ticket(4) + req_id(4) */
#define SVC_REPLY_HDR_SIZE   4      /* req_id(4) */
#define SVC_FRAME_MAX     4096      /* payload 上限，与 IPC_PAYLOAD_MAX 一致 */
#define SVC_REQ_DATA_MAX  (SVC_FRAME_MAX - SVC_REQ_HDR_SIZE)    /* 4088 */
#define SVC_REPLY_DATA_MAX (SVC_FRAME_MAX - SVC_REPLY_HDR_SIZE) /* 4092 */

#define SVC_TYPE_RPC      1u        /* svc_rpc 使用的 IPC type（服务自定义） */

#define SVC_CONNECT_TIMEOUT_MS 500  /* svc_connect 的推荐缺省值 */
#define SVC_RPC_TIMEOUT_MS    1000  /* conn->timeout_ms 缺省值 */

#define SVC_CACHE_MAX          8    /* 进程内 name->port 缓存条目数 */

/* 一个客户端连接：name + 当前端口 + 超时/请求号。
 * 结构公开是为让调用方能看到 port（诊断/日志）；不要绕过 API 改它。 */
struct svc_conn {
    int port;                       /* 当前服务端口句柄（>0） */
    int pid;                        /* 建立连接/重连时的进程 pid（fork 检测） */
    int timeout_ms;                 /* 本连接的 RPC 超时预算 */
    int last_err;                   /* 本连接上一次错误码 */
    unsigned int req_id;            /* 单调递增请求号（服务必须回显） */
    char name[SVC_NAME_MAX];        /* 服务名（svc_reconnect 用） */
};

/* 按名连接服务：timeout_ms 内反复 lookup；成功返回连接对象（malloc），
 * 失败返回 0 并把原因放进 svc_last_error()（通常是 SVC_ERR_NOENT）。
 * timeout_ms <= 0 表示只查一次；连接对象的 RPC 超时取 SVC_RPC_TIMEOUT_MS。 */
struct svc_conn *svc_connect(const char *name, int timeout_ms);

/* 释放连接对象。当前 ABI 没有 PORT_CLOSE，客户端端口由进程退出回收
 * （见 docs/claims/ipc.md §7 #7），所以这里只释放用户态结构。 */
int svc_close(struct svc_conn *conn);

/* 一问一答：请求按 §1 帧发给服务，等回复；成功返回回复字节数 [0..maxlen]，
 * 失败返回 SVC_ERR_*。req/reply 可为 0（len/maxlen 为 0 时）。 */
int svc_rpc(struct svc_conn *conn, const void *req, int len,
            void *reply, int maxlen);

/* 服务重启后重新 lookup 并把 conn 切到新端口；SVC_OK / SVC_ERR_NOENT /
 * SVC_ERR_IO。svc_rpc 内部会在端口失效时自动调用一次。 */
int svc_reconnect(struct svc_conn *conn);

/* 带缓存的 lookup：命中直接返回端口；未命中/上次失败则重新查。
 * 返回端口（>0）或 SVC_ERR_NOENT/SVC_ERR_IO。 */
int svc_lookup_cached(const char *name);

/* 让某个名字的缓存失效（如服务重启后主动刷新）；返回清掉的条目数。 */
int svc_cache_invalidate(const char *name);

/* 本进程最近一次 SDK 调用的错误码（SVC_OK / SVC_ERR_*）。 */
int svc_last_error(void);

/* 错误码的稳定英文文本（日志/演示用）。 */
const char *svc_strerror(int err);

#endif /* OS_INCLUDE_LIBSVC_H */
