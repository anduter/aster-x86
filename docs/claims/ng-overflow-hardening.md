# ng 加固：输入边界 + spawn 截断（2026-10-31）

## 结论

- **输入缓冲区边界复核**：shell 行缓冲、PS/2 事件环、`read_user_string` 均无越界；已有 host 单测覆盖环 128/129 与 `copyin_str` 截断。
- **修复 spawn 静默截断**：`copyin_str()` 在无 NUL 时返回 `cap-1`，与“合法长度 cap-1”不可区分；旧代码用 `>= cap` 判定，导致超长 path/argv 被截断后仍接受。现统一用 `proc_user_str_len_ok(n, cap) => n < cap-1` 判定，超长返回 `PROC_ENAMETOOLONG(-36)`。
- host 单测：`unit-ng` **5034 checks / 0 failures**（新增 9 项边界断言）。

## 证据

| 项 | 值 |
|---|---|
| `proc.h` | 新增 `PROC_ENAMETOOLONG(-36)` |
| `proc_str.h` | 新增 `proc_user_str_len_ok()`（可 host 测试的纯函数） |
| `proc.c` | spawn path + argv 改用该判定；超长返回 ENAMETOOLONG |
| 测试 | `tests/unit-ng/test_uaccess.c` 新增 `t_proc_str_len()`：cap=16（0/14 合法、15/16 拒绝）、cap=2、cap=0/1 |
| 编译 | `clang --target=x86_64-elf -fsyntax-only src/ng/proc.c` → exit 0 |
| 单测 | 5034 checks, 0 failures |

## 边界审计（无溢出）

| 缓冲 | 结论 |
|---|---|
| shell 行缓冲 `SH_LINE_MAX=128` | 插入条件 `len+1 < 128`，最大 127 含 NUL；Backspace/Ctrl-U 有 `len>0` 保护 |
| PS/2 事件环 `INPUT_RING_SIZE=128` | 满则丢弃新事件，push/pop 均检查 `count`，索引取模 |
| `read_user_string` | `n >= cap-1` 即拒绝（偏严，无溢出） |
| 协议 body | 编译期 static assert 固定长度 |

## 遗留

- shell 行编辑逻辑本身未抽成 host 可测函数（插入条件已人工核对 + 端到端覆盖）；
- 键盘“多轮询者抢键”（displayd 与 shell 同时 poll PS/2）是另一个真实问题，需输入服务化，未在本轮修。