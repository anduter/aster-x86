/* =============================================================================
 * verify_entry.c - loader 进入 32 位保护模式后的内核签名验证入口
 *
 * 链接地址 0x9800；loader 已把 kernel.bin 放在 0x100000。
 * 验证通过后跳到内核入口 0x100008；失败向 COM1 报错并停机。
 * ============================================================================= */
#include "../kernel/core/trust.h"
#include "verify_support.h"

extern const uint8_t kernel_signature[256];
extern const uint8_t kernel_modulus[256];

void boot_verify_entry(void)
{
    const uint8_t *image = (const uint8_t *)0x00100000U;
    uint32_t size = *(volatile uint32_t *)(uintptr_t)(0x00100004U);

    if (!trust_verify_image(image, size, kernel_signature, kernel_modulus)) {
        boot_verify_fail();
    }
    boot_verify_ok();

    ((void (*)(void))(uintptr_t)0x00100008U)();
}
