# ng-WP0-3：域依赖检查（check-domains）

## 出口

- `tools/check-domains.ps1` 落地：7 域规则（L0/L1/L1D/L2/L3 + A/B 横切），**向上依赖判违规**；
- `-SelfTest` 8 条规则用例全部通过（L0→L1 违规、L2→L3 违规、L1D→L2 违规、A/B 允许、同层允许…）；
- 当前树扫描：**0 违规**（205 文件 / 726 个 quoted include）；
- 已接入 `tools/ci.ps1` 为 `static 5/6`，与 check-deps 并列；`ci -Quick` 跑通。

## 规则（E0 骨架）

| 域 | 路径 | 允许依赖 |
|---|---|---|
| L0 | `src/kernel/**`（E0 旧单体） | L0 |
| L1 | `src/services/**` | L0/L1/A/B |
| L1D | `src/drivers/**` | L0/A/B |
| L2 | `src/desktop/**` | L0/L1/L2/A/B |
| L3 | `src/apps/**`、`src/user/**` | 全部 |
| A/B | `sdk/**`、`tools/**` | 任意（横切） |

> E0 把 `src/kernel/**` 整体视为 L0，避免在 64 位主线前搬目录；E2 起按 `src/services`/`src/drivers` 收紧。

## 出口命令

`
powershell -File tools/check-domains.ps1 -SelfTest   # 8 cases PASS
powershell -File tools/check-domains.ps1             # 0 violations
powershell -File tools/ci.ps1 -Quick                 # static 5/6 PASS
`

## 期望行

- `check-domains: selftest PASS (8 rule cases)`
- `domain include violations: 0`
- CI：`[PASS] check-domains 0 violations`

## 遗留

- 当前 `src/kernel/**` 整体 L0，尚未按 L1/L1D 拆分（E2）；
- 只检查 `#include "..."`，外部/系统头不检查；
- 头文件归属按路径推断，未来 `include/` 公共头需要单独规则。