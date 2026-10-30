#ifndef DEMOAPP_H
#define DEMOAPP_H

/* P7 程序层 SDK 示例（src/user/demoapp.c）：
 *   demoapp_main()     起本地演示服务（若无）→ connect → ping/pong，打印 sdk: demo ok
 *   demoapp_selftest() -Test 自检：connect/rpc/noent/reconnect/demo 六行结论
 * 一份源码两种用法：链接进 shell 镜像（-DDEMOAPP_EMBEDDED，`svc-demo` 命令）
 * 与独立 demoapp.elf（正常 _start 入口，可被组件加载器 exec）。 */

int demoapp_main(void);
int demoapp_selftest(void);

#endif
