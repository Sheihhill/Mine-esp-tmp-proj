#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ST7789 显示硬件并配置 LVGL 开发环境（同步函数）
 *
 * 完成内容：SPI 总线、ST7789 面板（含厂商初始化序列）、背光、
 * LVGL 核心（1ms tick 定时器 + taskLVGL 刷新任务）、注册 LVGL 显示设备。
 * 需要在 FreeRTOS 任务上下文中调用，返回时 LVGL 环境已就绪。
 */
void user_display_start(void);

/**
 * @brief 创建 LVGL UI 任务，在屏幕上绘制居中的 "Hello World" 文本
 */
void user_ui_start(void);

#ifdef __cplusplus
}
#endif
