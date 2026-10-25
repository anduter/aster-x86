#ifndef CORE_DEMO_H
#define CORE_DEMO_H

/* 启动自检：验证对象模型的三条基本约束
 *   1. 程序是注册项（不是文件路径）
 *   2. 文件只有数据（可写可读）
 *   3. 句柄带类型（程序句柄不能当文件/设备用）
 * 注意：这不是 shell，也不接受输入；只是内核启动时跑一次的自检。 */
void demo_run(void);

#endif
