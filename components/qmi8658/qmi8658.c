#include "qmi8658.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "qmi8658";

static const float acc_sensitivity_table[] = {
    [QMI8658_ACC_SCALE_2G]  = 16384.0f,
    [QMI8658_ACC_SCALE_4G]  = 8192.0f,
    [QMI8658_ACC_SCALE_8G]  = 4096.0f,
    [QMI8658_ACC_SCALE_16G] = 2048.0f,
};

static const float gyro_sensitivity_table[] = {
    [QMI8658_GYRO_SCALE_16DPS]   = 2048.0f,
    [QMI8658_GYRO_SCALE_32DPS]   = 1024.0f,
    [QMI8658_GYRO_SCALE_64DPS]   = 512.0f,
    [QMI8658_GYRO_SCALE_128DPS]  = 256.0f,
    [QMI8658_GYRO_SCALE_256DPS]  = 128.0f,
    [QMI8658_GYRO_SCALE_512DPS]  = 64.0f,
    [QMI8658_GYRO_SCALE_1024DPS] = 32.0f,
    [QMI8658_GYRO_SCALE_2048DPS] = 16.0f,
};

static esp_err_t qmi8658_write_reg(qmi8658_handle_t *handle, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(handle->i2c_dev, buf, 2, 100);
}

static esp_err_t qmi8658_read_reg(qmi8658_handle_t *handle, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, val, 1, 100);
}

static esp_err_t qmi8658_read_regs(qmi8658_handle_t *handle, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, buf, len, 100);
}

esp_err_t qmi8658_init(qmi8658_handle_t *handle, i2c_master_bus_handle_t bus,
                       const qmi8658_config_t *config)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &handle->i2c_dev),
                        TAG, "Failed to add QMI8658 to I2C bus");

    uint8_t who_am_i = 0;
    ESP_RETURN_ON_ERROR(qmi8658_read_reg(handle, QMI8658_REG_WHO_AM_I, &who_am_i),
                        TAG, "Failed to read WHO_AM_I");

    if (who_am_i != QMI8658_WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "WHO_AM_I mismatch: expected 0x%02X, got 0x%02X",
                 QMI8658_WHO_AM_I_VALUE, who_am_i);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t revision = 0;
    qmi8658_read_reg(handle, QMI8658_REG_REVISION, &revision);
    ESP_LOGI(TAG, "QMI8658 found (WHO_AM_I=0x%02X, revision=0x%02X)", who_am_i, revision);

    // Soft reset
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(handle, QMI8658_REG_RESET, 0xB0),
                        TAG, "Reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    // CTRL1: SPI 4-wire, I2C, address auto-increment, INT active high
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(handle, QMI8658_REG_CTRL1, 0x40),
                        TAG, "CTRL1 write failed");

    // CTRL2: accelerometer scale + ODR
    uint8_t ctrl2 = ((uint8_t)config->acc_scale << 4) | (uint8_t)config->acc_odr;
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(handle, QMI8658_REG_CTRL2, ctrl2),
                        TAG, "CTRL2 write failed");

    // CTRL3: gyroscope scale + ODR
    uint8_t ctrl3 = ((uint8_t)config->gyro_scale << 4) | (uint8_t)config->gyro_odr;
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(handle, QMI8658_REG_CTRL3, ctrl3),
                        TAG, "CTRL3 write failed");

    // CTRL5: LPF disabled for both accel and gyro (widest bandwidth)
    // Bits [1:0] = accel LPF mode, [3:2] = accel LPF enable
    // Bits [5:4] = gyro LPF mode, [7:6] = gyro LPF enable
    // 0x00 = all disabled = widest bandwidth (no filtering)
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(handle, QMI8658_REG_CTRL5, 0x00),
                        TAG, "CTRL5 write failed");

    // CTRL7: enable both accelerometer and gyroscope
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(handle, QMI8658_REG_CTRL7, 0x03),
                        TAG, "CTRL7 write failed");

    handle->acc_sensitivity = acc_sensitivity_table[config->acc_scale];
    handle->gyro_sensitivity = gyro_sensitivity_table[config->gyro_scale];

    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "QMI8658 configured: acc=%dG@ODR%d, gyro=%dDPS@ODR%d",
             2 << config->acc_scale,
             8000 >> config->acc_odr,
             16 << config->gyro_scale,
             8000 >> config->gyro_odr);

    return ESP_OK;
}

esp_err_t qmi8658_read_data(qmi8658_handle_t *handle, qmi8658_data_t *data)
{
    // Burst read: temp(2) + accel(6) + gyro(6) = 14 bytes from 0x33
    uint8_t buf[14];
    ESP_RETURN_ON_ERROR(qmi8658_read_regs(handle, QMI8658_REG_TEMP_L, buf, 14),
                        TAG, "Data read failed");

    int16_t temp_raw = (int16_t)((buf[1] << 8) | buf[0]);
    data->temperature = (float)temp_raw / 256.0f;

    int16_t ax = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t ay = (int16_t)((buf[5] << 8) | buf[4]);
    int16_t az = (int16_t)((buf[7] << 8) | buf[6]);
    data->acc.x = (float)ax / handle->acc_sensitivity;
    data->acc.y = (float)ay / handle->acc_sensitivity;
    data->acc.z = (float)az / handle->acc_sensitivity;

    int16_t gx = (int16_t)((buf[9] << 8) | buf[8]);
    int16_t gy = (int16_t)((buf[11] << 8) | buf[10]);
    int16_t gz = (int16_t)((buf[13] << 8) | buf[12]);
    data->gyro.x = (float)gx / handle->gyro_sensitivity;
    data->gyro.y = (float)gy / handle->gyro_sensitivity;
    data->gyro.z = (float)gz / handle->gyro_sensitivity;

    // Check for gyro clipping: raw value at ±32767 means full-scale
    data->gyro_clipped = (gx == 32767 || gx == -32768 ||
                          gy == 32767 || gy == -32768 ||
                          gz == 32767 || gz == -32768);

    return ESP_OK;
}

esp_err_t qmi8658_deinit(qmi8658_handle_t *handle)
{
    qmi8658_write_reg(handle, QMI8658_REG_CTRL7, 0x00);
    return i2c_master_bus_rm_device(handle->i2c_dev);
}
