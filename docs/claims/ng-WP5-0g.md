# ng-WP5-0g claims：ring3 抢占（D1 前置）

- 状态：**待做（pending）**
- 依赖：WP5-0f（PIT tick / TSC 校准 / RTC / clock_gettime）
- 范围：IRQ0 tick 置 need_resched；IRQ 返回 ring3 时按时间片切换；内核保持 IF=0/协作，不在 IRQ 里 switch；不做内核抢占/SMP。
- 允许路径：src/ng/isr.S、trap.c、sched.c、sched_arch.c、thread.h、main.c、tests/unit-ng/test_sched.c、tools/run-ng.ps1、docs/design/sched-ng.md

## 出口证据

- ng: tick preempt ok (hz=1000 slice=10ms)
- ng: wm spin preempt ok (shell alive=1)
- WM 自旋时 shell/其他应用仍可运行；kill WM 后面板恢复
- 1h drift ≤100 ppm（与 WP5-0f 合并验收）
- tick 开销 ≤1% CPU（空闲）
- unit 覆盖 need_resched / 时间片边界

## 复跑

    powershell -File tools/unit-ng.ps1
    powershell -File tools/build-ng.ps1
    powershell -File tools/run-ng.ps1

## 已知限制

- 仅 ring3 抢占；内核态长临界区仍会推迟调度。
- 完整内核抢占 / SMP / RT 优先级属 D2。
