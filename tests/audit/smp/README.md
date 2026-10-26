# tests/audit/smp - SMP 并发/竞态回归（audit-smp）

本目录是 `audit-smp` 会话的最小复现/回归入口；真正的测试逻辑在内核自检里
（`src/kernel/proc/audit_smp_selftest.c`、`src/kernel/proc/audit_priv_selftest.c`），
由 `sched_start()` 在选第一个进程之前注册/放置，所以 2 核/4 核启动都会自动跑，
不需要登录或额外命令。

## 怎么跑

```powershell
# 构建（独立产物；不占 12345，QMP 4544）
powershell -NoProfile -File build.ps1 -ObjDir .obj-audit-smp -DistDir .dist-audit-smp `
    -QmpPort 4544 -SerialLog obj/audit/smp.log

# 2 核
powershell -NoProfile -File tools\audit-smp-run.ps1 -Smp 2

# 4 核（自动复制到单独的镜像目录 .dist-audit-smp4，串口 obj/audit/smp4.log）
powershell -NoProfile -File tools\audit-smp-run.ps1 -Smp 4

# 断言串口里的自检行
powershell -NoProfile -File tests\audit\smp\check.ps1 -Log obj/audit/smp.log -Cpus 2
powershell -NoProfile -File tests\audit\smp\check.ps1 -Log obj/audit/smp4.log -Cpus 4

# 4 核 12 分钟压测（QMP 注入 sectest/fork/linktest，循环 fork/exit/mmap/signal）
powershell -NoProfile -File tools\audit-smp-soak.ps1 -Minutes 12
```

## 期望串口行

```text
audit: sigreturn frame validation ok (user cs/ss, user eip, no iopl/nt/vm)
audit: kill uid policy ok (root/self/same-uid/other)
audit: uaccess user window ok (kernel-low/3gb/wrap rejected)
audit: smp selftest start (cpus=N)
audit: smp spawn race PASS (rounds=24)
audit: smp wake-vs-block PASS (latency=0 ticks)
audit: mm vmalloc concurrent PASS (iters=2400)
audit: smp selftest PASS
audit: smp selftest done
```

任一项失败会打印对应的 `... FAIL (...)` 行并带现场（槽位/pid、等待 tick、
worker 状态、`vmm_faults`/`pmm_double_free` 增量），最后仍会打印
`audit: smp selftest done` 以便宿主脚本收尾。

## 三个并发自检在测什么

1. **spawn race**：两颗 CPU 用两方屏障同时进入 `proc_spawn()`，比较返回的
   `proc_t` 槽位与 pid。旧的无锁“扫描 FREE 槽 + `next_pid++`”会让两个子进程
   共用同一个 `proc_t`（同一内核栈）或拿到同一 pid。
2. **wake-vs-block**：目标在 CPU0 自旋（RUNNING），CPU1 在它进入 `proc_block()`
   之前 `proc_wakeup()`；旧实现看到 RUNNING 直接丢弃唤醒，目标永久睡眠。
3. **vmalloc concurrent**：两线程并发 `vmalloc()/vfree()`。旧实现没有锁，
   会拿到同一虚拟区间（共享内核页表）、重复释放物理页、踩坏块头。

负向验证（临时还原旧代码）记录在 `docs/audit/smp.md` §4：spawn race 出现
`FAIL (reason=1 slot0==slot1 pid0==pid1)`；wake-vs-block 出现
`FAIL (lost wakeup, waited=600 ticks)`；vmalloc 自检在旧代码上表现为
块头/双重释放告警或数据互踩。
