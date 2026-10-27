# 加固批次（v0.74.2）

基线：0.74.1 / HEAD `3f91e19` / 248 项检查。本批次每项独立提交、独立回归；
新增自检行都进 `build.ps1` 的 checks，最终 `ci.ps1 -Full -StrictBom` 连跑两遍。

约定：

- 现象、根因、修复、回归、遗留五段，一项一节。
- 回归行是**内核/用户态打印的稳定字符串**，由 `build.ps1` 断言；手工 `-smp 4`
  验证另记串口证据。
- 设计级问题不硬改，记在每节“遗留”和文末“未修项”。

---

## 1. 4 核 AP LAPIC tick 偏斜（P1）

**现象**：`-smp 4` 启动 2s 时 `smp: tick check after 2s bsp=200`，三个 AP 分别是
368/286/203，核间 tick 最多差 84%。

**根因**（三层，叠在一起）：

1. **校准分频没有固定**。`apic_watchdog_start()` 用 LAPIC 当前计数做 100ms
   校准，但没写 DCR，依赖复位值；真实硬件/QEMU 复位分频是 /2，而
   `apic_tick()` 的 `period = elapsed * 10 / 4` 与 AP 的
   `count = period / 400` 都假设校准在 divide-by-1。分频差一倍，AP 定时器
   频率就错一倍。
2. **校准窗口只取 10 个 PIT tick**。QEMU/TCG 下 PIT 边沿在 IF=0 或投递延迟时
   会被 IOAPIC 合并，10 个 tick 不一定是 100ms；启动早期尤其严重，校准出的
   period 偏大，AP 的 10ms 被装成 12–15ms。
3. **AP tick 基准没有对齐**。AP 在 BSP 校准完成前用启动初始计数
   （`1,000,000` @ /16）跑过一段，且各 AP 启动时刻相差 ~0.5s；即使频率正确，
   2s 时刻的累计 tick 也天然不齐（368/286/203 的阶梯就是启动顺序）。

**修复**：

- `dev/intc/apic.c`：校准前显式 `DCR = 0x0B`（divide-by-1）；NMI 周期也用
  /1 计数，公式才成立。
- 校准窗口从 10 个 tick 加长到 100 个 tick（`APIC_CAL_TICKS`）。per-CPU tick
  要和 BSP 的 PIT tick 直接比较，因此 AP 的“10ms 计数”按 BSP 实际 tick
  间隔（含被合并的边沿）来定；100 tick 把启动早期的高延迟摊平。
- `proc/smp.c`：AP 在第一次拿到校准计数、重装自己的 LAPIC 时，把
  `per_cpu[c].ticks` 置为 `timer_ticks()`（BSP 墙钟），之后按校准后的周期推进。
  这消除了 AP 启动先后和校准前未校准 tick 的影响。
- `proc/trap/irq.c`：tick 200 时新增自检行，比较所有在线 CPU 的绝对 tick
  与 BSP 的偏差，`max_delta <= 10%` 打印 ok。

**回归**：

```text
smp: tick check after 2s bsp=200 ap=189 ap2=0 ap3=0 (online=2)
smp: ap tick skew ok (max_delta=5%, limit=10%, window=2s)

# -smp 4 手工（serial-smp4.log）
smp: tick check after 2s bsp=200 ap=191 ap2=192 ap3=191 (online=4)
smp: ap tick skew ok (max_delta=4%, limit=10%, window=2s)
```

`build.ps1` 新增检查 `AP tick 校准无偏斜`；`-Test` 249/249 PASS。

**遗留**：

- 自检是 2s 绝对 tick 比较，AP 若在 tick 200 之后才上线会天然落后；当前
  `smp_start_aps()` 在 `sti` 前完成，实际不会发生。若将来支持热插拔 CPU，
  应改成“上线后 2s 窗口”的增量比较。
- 校准仍是启发式（100 tick 平均），TCG 负载极端时 190±10% 是预期下限；
  `max_delta` 打印出来便于观察长期漂移。
