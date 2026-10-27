#ifndef CORE_ERROR_H
#define CORE_ERROR_H

/* 内核内部统一错误码：0 表示成功，负数表示失败（模仿 errno，但不需要全局变量） */
#define E_OK      0
#define E_INVAL  (-1)   /* 参数非法 */
#define E_NOENT  (-2)   /* 对象不存在 */
#define E_NOSPC  (-3)   /* 空间不足 */
#define E_TYPE   (-4)   /* 句柄类型不匹配 */
#define E_BADH   (-5)   /* 句柄非法（越界或已释放） */
#define E_EXIST  (-6)   /* 已存在 */
#define E_FULL   (-7)   /* 表已满 */
#define E_ACCES  (-8)   /* 权限不足 */
#define E_BUSY   (-9)   /* 对象仍被引用/占用（例如删除打开中的文件） */

/* 阻塞系统调用被未屏蔽信号打断：返回 -EINTR（errno=4，与 Linux 一致）。
 * 调用方返回用户态前 proc_check_signals() 会投递 handler/SIGKILL，不吞信号。 */
#define EINTR     4

#endif
