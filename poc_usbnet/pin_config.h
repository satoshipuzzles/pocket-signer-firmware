#pragma once

// Waveshare ESP32-S3 Touch-AMOLED-1.8 (V2) — display + touch only.

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_WIDTH 368
#define LCD_HEIGHT 448

// Touch (CST816T behind the shared I2C bus)
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 21

// SD card (SDMMC 1-bit)
#define SDMMC_CLK 2
#define SDMMC_CMD 1
#define SDMMC_DATA 3
