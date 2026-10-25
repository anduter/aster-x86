#ifndef CORE_SPLASH_H
#define CORE_SPLASH_H

/* 在 framebuffer 上绘制启动画面；必须在 fb_init() 之后、console_init() 之前调用。 */
void boot_splash(void);

#endif
