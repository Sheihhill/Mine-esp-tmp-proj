/**
 * @file user_display.c
 * @brief ST7789 SPI 显示屏驱动、LVGL 开发环境初始化与 UI 任务
 *
 * 配置项（引脚、时钟、颜色反转等）在 menuconfig 的
 * "Example Configuration -> Display Configuration (ST7789 SPI)" 中修改。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "sdkconfig.h"
#include "user_display.h"

static const char *TAG = "user_display";

/* ---------------- Kconfig 引脚与参数 ---------------- */
#define DISP_SCK_GPIO       CONFIG_DISPLAY_SPI_SCK_GPIO //18
#define DISP_MOSI_GPIO      CONFIG_DISPLAY_SPI_MOSI_GPIO //23
#define DISP_CS_GPIO        CONFIG_DISPLAY_SPI_CS_GPIO //27
#define DISP_DC_GPIO        CONFIG_DISPLAY_DC_GPIO //26
#define DISP_RST_GPIO       CONFIG_DISPLAY_RST_GPIO //4
#define DISP_BL_GPIO        CONFIG_DISPLAY_BL_GPIO //21
#define DISP_BL_ON_LEVEL    CONFIG_DISPLAY_BL_ON_LEVEL    //1
#define DISP_SPI_CLOCK_HZ   (CONFIG_DISPLAY_SPI_CLOCK_MHZ * 1000 * 1000)
#define DISP_OFFSET_X       CONFIG_DISPLAY_OFFSET_X
#define DISP_OFFSET_Y       CONFIG_DISPLAY_OFFSET_Y
#define DISP_INVERT_COLOR   CONFIG_DISPLAY_INVERT_COLOR
#ifdef CONFIG_DISPLAY_BGR
#define DISP_RGB_ELE_ORDER  LCD_RGB_ELEMENT_ORDER_BGR
#else
#define DISP_RGB_ELE_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#endif

/* ---------------- 屏幕参数（240x240 ST7789） ---------------- */
#define LCD_H_RES           240
#define LCD_V_RES           240
#define LCD_CMD_BITS        8
#define LCD_PARAM_BITS      8

/* UI 任务参数 */
#define UI_TASK_STACK       4096
#define UI_TASK_PRIO        5

/* ---------------- 厂商初始化序列（对照厂家示例代码移植） ----------------
 * esp_lcd 内置 st7789 驱动只发送 SLPOUT/MADCTL/COLMOD/RAMCTRL，
 * 以下电源配置与伽马曲线来自厂家提供的初始化流程，
 * 在 esp_lcd_panel_init() 之后通过 esp_lcd_panel_io_tx_param() 补发。
 */
typedef struct {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_vendor_init_cmds[] = {
    { 0xB2, { 0x0C, 0x0C, 0x00, 0x33, 0x33 }, 5 },   /* PORCTRL  porch 设置 */
    { 0xB7, { 0x35 }, 1 },                            /* GCTRL    电压设置 */
    { 0xBB, { 0x19 }, 1 },                            /* VCOMS    VCOM 设置 */
    { 0xC0, { 0x2C }, 1 },                            /* LCMCTRL */
    { 0xC2, { 0x01 }, 1 },                            /* VDVVRHEN */
    { 0xC3, { 0x12 }, 1 },                            /* VRHS */
    { 0xC4, { 0x20 }, 1 },                            /* VDVS */
    { 0xC6, { 0x0F }, 1 },                            /* FRCTRL   60Hz 帧率 */
    { 0xD0, { 0xA4, 0xA1 }, 2 },                      /* PWCTRL1  电源控制 */
    /* PVGAMCTRL 正极伽马曲线 */
    { 0xE0, { 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23 }, 14 },
    /* NVGAMCTRL 负极伽马曲线 */
    { 0xE1, { 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23 }, 14 },
};

/**
 * @brief 发送厂商电源/伽马初始化序列
 */
static void display_vendor_init(esp_lcd_panel_io_handle_t io)
{
    for (size_t i = 0; i < sizeof(s_vendor_init_cmds) / sizeof(s_vendor_init_cmds[0]); i++) {
        esp_lcd_panel_io_tx_param(io,
                                  s_vendor_init_cmds[i].cmd,
                                  s_vendor_init_cmds[i].data,
                                  s_vendor_init_cmds[i].len);
    }
}

static esp_lcd_panel_handle_t s_panel = NULL;

/**
 * @brief 初始化背光 GPIO 并点亮
 */
static void display_backlight_init(void)
{
#if DISP_BL_GPIO >= 0
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISP_BL_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(DISP_BL_GPIO, DISP_BL_ON_LEVEL);
#endif
}

/**
 * @brief 初始化显示硬件并配置 LVGL 开发环境（同步函数，返回时 LVGL 已就绪）
 *
 * 需要在 FreeRTOS 任务上下文中调用。
 */
void user_display_start(void)
{
    /* 1. 初始化 SPI 总线 */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = DISP_SCK_GPIO,
        .mosi_io_num = DISP_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* 2. 创建 SPI Panel IO */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = DISP_DC_GPIO,
        .cs_gpio_num = DISP_CS_GPIO,
        .pclk_hz = DISP_SPI_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) SPI2_HOST, &io_cfg, &io_handle));

    /* 3. 创建 ST7789 面板 */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISP_RST_GPIO,
        .rgb_ele_order = DISP_RGB_ELE_ORDER,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));

    /* 4. 复位、初始化、开显示（顺序与厂家流程一致） */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* 补发厂家电源配置与伽马曲线（内置驱动不包含这些寄存器） */
    display_vendor_init(io_handle);
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, DISP_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, DISP_OFFSET_X, DISP_OFFSET_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "ST7789 initialized: %dx%d, SPI %dMHz, CS=%d DC=%d RST=%d BL=%d",
             LCD_H_RES, LCD_V_RES, CONFIG_DISPLAY_SPI_CLOCK_MHZ,
             DISP_CS_GPIO, DISP_DC_GPIO, DISP_RST_GPIO, DISP_BL_GPIO);

    /* 6. 配置 LVGL 开发环境。
     * lvgl_port_init() 内部会创建两样东西（LVGL 正常工作的关键）：
     *   a) esp_timer 周期定时器：每 timer_period_ms 毫秒调用一次 lv_tick_inc()，
     *      这就是 LVGL 的"心跳"，所有动画/定时器都以它为时间基准；
     *   b) "taskLVGL" 任务：循环调用 lv_timer_handler()，执行 LVGL 定时器
     *      回调并把画面刷新到屏幕。
     * 这里把心跳周期配置为 1ms。
     */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.timer_period_ms = 1;   /* LVGL 心跳：每 1ms 调用一次 lv_tick_inc() */
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* 7. 把 ST7789 注册为 LVGL 显示设备（双缓冲 + DMA + 字节交换） */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * 40,   /* 部分刷新缓冲（约 19KB/块） */
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,          /* ST7789 需要 RGB565 大端，字节交换 */
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) 
    {
        ESP_LOGE(TAG, "failed to add LVGL display");
        return;
    }
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();          // 9.x 的新名字，lv_scr_act() 是 v8 的叫法
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN); //src配置成黑色
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lvgl_port_unlock();
    lv_delay_ms(100); // 等待屏幕刷新完成
        /* 5. 点亮背光 */
    display_backlight_init();
    ESP_LOGI(TAG, "LVGL environment ready: tick=1ms, taskLVGL running, display registered");
}

/* ---------------- UI 任务：在屏幕上绘制 Hello World 并显示计数 ---------------- */

/**
 * @brief LVGL UI 任务：绘制居中的 "Hello World"，下方显示从 0 开始的秒计数
 *
 * LVGL API 不是线程安全的，操作 UI 对象必须包在
 * lvgl_port_lock()/lvgl_port_unlock() 之间；
 * 画面刷新由 esp_lvgl_port 的 taskLVGL 任务自动完成，无需手动刷新。
 */
static void user_ui_task(void *arg)
{
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();          // 9.x 的新名字，lv_scr_act() 是 v8 的叫法
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN); //src配置成黑色
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);       //src的背景不透明
    lv_obj_set_style_text_color(scr, lv_color_white(), LV_PART_MAIN);   //src的文字颜色白色

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello World");
    lv_obj_center(label);

    /* 计数标签：显示上电后累计秒数，从 0 开始累加 */
    lv_obj_t *count_label = lv_label_create(scr);
    lv_label_set_text(count_label, "0");
    lv_obj_align(count_label, LV_ALIGN_CENTER, 0, 30);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL UI: 'Hello World' 已绘制, 秒计数启动");

    /* esp_timer 从上电/复位起以微秒计时，这里以 UI 绘制完成时刻为 0 点，
     * 每秒 +1 并刷新到屏幕。若要严格按上电时刻起算，把 sec 改为
     * (uint32_t)(esp_timer_get_time() / 1000000ULL) 即可。 */
    int64_t start_us = esp_timer_get_time();
    uint32_t last_sec = 0;

    /* 保持任务存活，周期检查是否跨秒并刷新计数 */
    while (1) {
        uint32_t sec = (uint32_t)((esp_timer_get_time() - start_us) / 1000000ULL);
        if (sec != last_sec) {
            last_sec = sec;
            lvgl_port_lock(0);
            lv_label_set_text_fmt(count_label, "%u", (unsigned)sec);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/**
 * @brief 创建 LVGL UI 任务
 */
void user_ui_start(void)
{
    BaseType_t ret = xTaskCreate(user_ui_task,
                                 "user_ui",
                                 UI_TASK_STACK,
                                 NULL,
                                 UI_TASK_PRIO,
                                 NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create UI task");
    }
}
