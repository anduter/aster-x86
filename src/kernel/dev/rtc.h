#ifndef CORE_RTC_H
#define CORE_RTC_H

#include "../include/types.h"

/* CMOS 实时时钟：返回从 1970-01-01 起的秒数（UTC） */
uint32_t rtc_now(void);

/* 把 epoch 秒格式化成 "2026-10-22 21:55:00"（UTC），返回写入长度 */
int rtc_format(uint32_t epoch, char *buf, uint32_t max);

#endif
