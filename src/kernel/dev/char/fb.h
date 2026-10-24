#ifndef FB_H
#define FB_H

/* loader 采集的系统信息表偏移，必须与 loader/sysinfo.asm 保持一致 */
#define SYSINFO         0x5000
#define MEM_TOTAL       (SYSINFO + 0x200U)
#define MEM_USED        (SYSINFO + 0x208U)
#define MEM_FREE        (SYSINFO + 0x210U)
#define FB_ADDR         (SYSINFO + 0x302U)
#define CPU_VENDOR      (SYSINFO + 0x600U)
#define VBE_MODE        (SYSINFO + 0x400U)

/* 颜色值格式 0x00RRGGBB */
#define C_BLACK     0x00000000U
#define C_WHITE     0x00FFFFFFU
#define C_GREEN     0x0000FF00U
#define C_YELLOW    0x00FFFF00U
#define C_DARK_BG   0x00102030U

/* 界面布局常量 */
#define TITLE_X     8U
#define TITLE_Y     8U
#define INFO_X      10U
#define VAL_X       150U
#define LINE_STEP   16U

/* 显存全局参数（对调用者只读） */
extern unsigned int fb_addr;     /* 显存物理地址 */
extern unsigned short fb_w, fb_h;/* 横向/纵向分辨率 */
extern unsigned char  fb_bpp;    /* 每像素位数 */
extern unsigned int fb_pitch;    /* 每行字节数 */
extern unsigned int fb_size;     /* pitch * height，实际占用的字节数 */

/* 对外接口 */
void fb_init(void);
void fb_pixel(unsigned int x, unsigned int y, unsigned int color);
void fb_clear(unsigned int color);
/* 填充矩形 [x0,x1) x [y0,y1)，超出屏幕自动裁剪 */
void fb_fill_rect(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1, unsigned int color);
void fb_char(unsigned int x, unsigned int y, char c, unsigned int fg, unsigned int bg);
/* scale > 1 时按整数倍放大（保持像素清晰），控制台字体放大用 */
void fb_char_scaled(unsigned int x, unsigned int y, char c,
                    unsigned int fg, unsigned int bg, unsigned int scale);
void fb_text(unsigned int x, unsigned int y, const char *s, unsigned int fg, unsigned int bg);
void fb_hex32(unsigned int x, unsigned int y, unsigned int val, unsigned int fg, unsigned int bg);
void fb_hex64(unsigned int x, unsigned int y, unsigned long long val, unsigned int fg, unsigned int bg);
/* 以十进制输出无符号整数 */
void fb_dec32(unsigned int x, unsigned int y, unsigned int val, unsigned int fg, unsigned int bg);

#endif
