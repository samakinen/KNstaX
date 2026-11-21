/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GD32F1 reference port: vector table, Reset_Handler and the C runtime start.
 *
 * Reset_Handler calls gd32f1_early_init() before anything else runs (before
 * the clock, .data, .bss or C++ constructors), so a product drives its
 * outputs to a safe level first. It then keeps the debug clocks on in sleep
 * and freezes the watchdog while the core is halted, so the probe can attach
 * at any time (without that, a core in WFI stops the clock the debug port
 * needs).
 *
 * Every interrupt has a weak handler with the STM32F103 medium-density name;
 * a product defines the ones it uses. Unused ones loop: the watchdog or the
 * debugger takes over, and the next boot reports the reset cause.
 */
#include <stdint.h>

#include "gd32f1_system.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void (*__preinit_array_start[])(void);
extern void (*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
int main(void);
void Reset_Handler(void);

#define REG(addr) (*(volatile uint32_t*)(addr))

void Reset_Handler(void)
{
    gd32f1_early_init();

    /* Debug: clocks on in sleep, stop and standby; watchdog frozen while halted. */
    REG(0xE0042004u) |= (1u << 0) | (1u << 1) | (1u << 2) | (1u << 8);

    /* C runtime. .data holds .ramfunc (the flash erase wait). */
    for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;) {
        *d++ = *s++;
    }
    for (uint32_t* d = &_sbss; d < &_ebss;) {
        *d++ = 0;
    }
    for (void (**f)(void) = __preinit_array_start; f < __preinit_array_end; ++f) {
        (*f)();
    }
    for (void (**f)(void) = __init_array_start; f < __init_array_end; ++f) {
        (*f)();
    }
    main();
    for (;;) {
    }
}

static void Default_Handler(void)
{
    for (;;) {
    }
}

#define W __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) W;
void HardFault_Handler(void) W;
void MemManage_Handler(void) W;
void BusFault_Handler(void) W;
void UsageFault_Handler(void) W;
void SVC_Handler(void) W;
void DebugMon_Handler(void) W;
void PendSV_Handler(void) W;
void SysTick_Handler(void) W;
void WWDG_IRQHandler(void) W;
void PVD_IRQHandler(void) W;
void TAMPER_IRQHandler(void) W;
void RTC_IRQHandler(void) W;
void FLASH_IRQHandler(void) W;
void RCC_IRQHandler(void) W;
void EXTI0_IRQHandler(void) W;
void EXTI1_IRQHandler(void) W;
void EXTI2_IRQHandler(void) W;
void EXTI3_IRQHandler(void) W;
void EXTI4_IRQHandler(void) W;
void DMA1_Channel1_IRQHandler(void) W;
void DMA1_Channel2_IRQHandler(void) W;
void DMA1_Channel3_IRQHandler(void) W;
void DMA1_Channel4_IRQHandler(void) W;
void DMA1_Channel5_IRQHandler(void) W;
void DMA1_Channel6_IRQHandler(void) W;
void DMA1_Channel7_IRQHandler(void) W;
void ADC1_2_IRQHandler(void) W;
void USB_HP_CAN1_TX_IRQHandler(void) W;
void USB_LP_CAN1_RX0_IRQHandler(void) W;
void CAN1_RX1_IRQHandler(void) W;
void CAN1_SCE_IRQHandler(void) W;
void EXTI9_5_IRQHandler(void) W;
void TIM1_BRK_IRQHandler(void) W;
void TIM1_UP_IRQHandler(void) W;
void TIM1_TRG_COM_IRQHandler(void) W;
void TIM1_CC_IRQHandler(void) W;
void TIM2_IRQHandler(void) W;
void TIM3_IRQHandler(void) W;
void TIM4_IRQHandler(void) W;
void I2C1_EV_IRQHandler(void) W;
void I2C1_ER_IRQHandler(void) W;
void I2C2_EV_IRQHandler(void) W;
void I2C2_ER_IRQHandler(void) W;
void SPI1_IRQHandler(void) W;
void SPI2_IRQHandler(void) W;
void USART1_IRQHandler(void) W;
void USART2_IRQHandler(void) W;
void USART3_IRQHandler(void) W;
void EXTI15_10_IRQHandler(void) W;
void RTC_Alarm_IRQHandler(void) W;
void USBWakeUp_IRQHandler(void) W;

#define IRQ_COUNT 43u /* medium density */

__attribute__((section(".isr_vector"), used)) void (*const gd32f1_vector_table[16u + IRQ_COUNT])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0,
    0,
    0,
    0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    WWDG_IRQHandler,
    PVD_IRQHandler,
    TAMPER_IRQHandler,
    RTC_IRQHandler,
    FLASH_IRQHandler,
    RCC_IRQHandler,
    EXTI0_IRQHandler,
    EXTI1_IRQHandler,
    EXTI2_IRQHandler,
    EXTI3_IRQHandler,
    EXTI4_IRQHandler,
    DMA1_Channel1_IRQHandler,
    DMA1_Channel2_IRQHandler,
    DMA1_Channel3_IRQHandler,
    DMA1_Channel4_IRQHandler,
    DMA1_Channel5_IRQHandler,
    DMA1_Channel6_IRQHandler,
    DMA1_Channel7_IRQHandler,
    ADC1_2_IRQHandler,
    USB_HP_CAN1_TX_IRQHandler,
    USB_LP_CAN1_RX0_IRQHandler,
    CAN1_RX1_IRQHandler,
    CAN1_SCE_IRQHandler,
    EXTI9_5_IRQHandler,
    TIM1_BRK_IRQHandler,
    TIM1_UP_IRQHandler,
    TIM1_TRG_COM_IRQHandler,
    TIM1_CC_IRQHandler,
    TIM2_IRQHandler,
    TIM3_IRQHandler,
    TIM4_IRQHandler,
    I2C1_EV_IRQHandler,
    I2C1_ER_IRQHandler,
    I2C2_EV_IRQHandler,
    I2C2_ER_IRQHandler,
    SPI1_IRQHandler,
    SPI2_IRQHandler,
    USART1_IRQHandler,
    USART2_IRQHandler,
    USART3_IRQHandler,
    EXTI15_10_IRQHandler,
    RTC_Alarm_IRQHandler,
    USBWakeUp_IRQHandler,
};
