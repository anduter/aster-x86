#ifndef VERSION_H
#define VERSION_H

/* 系统名称与版本号：改版本只改这里，日志、标题栏、README 都引用它 */
#define OS_NAME     "x86 OS"
#define OS_VERSION  "0.45.0"

/* 编译时间戳（由编译器内置宏展开） */
#define OS_BUILD    __DATE__ " " __TIME__

#endif
