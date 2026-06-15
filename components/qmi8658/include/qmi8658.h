#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define QMI8658_I2C_ADDR        0x6B

#define QMI8658_REG_WHO_AM_I    0x00
#define QMI8658_REG_REVISION    0x01
#define QMI8658_REG_CTRL1       0x02
#define QMI8658_REG_CTRL2       0x03
#define QMI8658_REG_CTRL3       0x04
#define QMI8658_REG_CTRL5       0x06
#define QMI8658_REG_CTRL7       0x08
#define QMI8658_REG_CTRL9       0x0A
#define QMI8658_REG_TEMP_L      0x33
#define QMI8658_REG_TEMP_H      0x34
#define QMI8658_REG_AX_L        0x35
#define QMI8658_REG_AX_H        0x36
#define QMI8658_REG_AY_L        0x37
#define QMI8658_REG_AY_H        0x38
#define QMI8658_REG_AZ_L        0x39
#define QMI8658_REG_AZ_H        0x3A
#define QMI8658_REG_GX_L        0x3B
#define QMI8658_REG_GX_H        0x3C
#define QMI8658_REG_GY_L        0x3D
#define QMI8658_REG_GY_H        0x3E
#define QMI8658_REG_GZ_L        0x3F
#define QMI8658_REG_GZ_H        0x40
#define QMI8658_REG_STATUSINT   0x2D
#define QMI8658_REG_STATUS0     0x46
#define QMI8658_REG_STATUS1     0x47
#define QMI8658_REG_RESET       0x60

/* STATUSINT bits for gyro clipping */
#define QMI8658_GYRO_CLIP_X     (1 << 2)
#define QMI8658_GYRO_CLIP_Y     (1 << 1)
#define QMI8658_GYRO_CLIP_Z     (1 << 0)

#define QMI8658_WHO_AM_I_VALUE  0x05

typedef enum {
    QMI8658_ACC_SCALE_2G  = 0,
    QMI8658_ACC_SCALE_4G  = 1,
    QMI8658_ACC_SCALE_8G  = 2,
    QMI8658_ACC_SCALE_16G = 3,
} qmi8658_acc_scale_t;

typedef enum {
    QMI8658_GYRO_SCALE_16DPS   = 0,
    QMI8658_GYRO_SCALE_32DPS   = 1,
    QMI8658_GYRO_SCALE_64DPS   = 2,
    QMI8658_GYRO_SCALE_128DPS  = 3,
    QMI8658_GYRO_SCALE_256DPS  = 4,
    QMI8658_GYRO_SCALE_512DPS  = 5,
    QMI8658_GYRO_SCALE_1024DPS = 6,
    QMI8658_GYRO_SCALE_2048DPS = 7,
} qmi8658_gyro_scale_t;

typedef enum {
    QMI8658_ACC_ODR_8000  = 0,
    QMI8658_ACC_ODR_4000  = 1,
    QMI8658_ACC_ODR_2000  = 2,
    QMI8658_ACC_ODR_1000  = 3,
    QMI8658_ACC_ODR_500   = 4,
    QMI8658_ACC_ODR_250   = 5,
    QMI8658_ACC_ODR_125   = 6,
    QMI8658_ACC_ODR_62_5  = 7,
    QMI8658_ACC_ODR_31_25 = 8,
} qmi8658_acc_odr_t;

typedef enum {
    QMI8658_GYRO_ODR_8000  = 0,
    QMI8658_GYRO_ODR_4000  = 1,
    QMI8658_GYRO_ODR_2000  = 2,
    QMI8658_GYRO_ODR_1000  = 3,
    QMI8658_GYRO_ODR_500   = 4,
    QMI8658_GYRO_ODR_250   = 5,
    QMI8658_GYRO_ODR_125   = 6,
    QMI8658_GYRO_ODR_62_5  = 7,
    QMI8658_GYRO_ODR_31_25 = 8,
} qmi8658_gyro_odr_t;

typedef struct {
    float x, y, z;
} qmi8658_axes_t;

typedef struct {
    qmi8658_axes_t acc;
    qmi8658_axes_t gyro;
    float temperature;
    bool  gyro_clipped;   // true if any gyro axis hit full-scale
} qmi8658_data_t;

typedef struct {
    qmi8658_acc_scale_t  acc_scale;
    qmi8658_acc_odr_t    acc_odr;
    qmi8658_gyro_scale_t gyro_scale;
    qmi8658_gyro_odr_t   gyro_odr;
} qmi8658_config_t;

typedef struct {
    i2c_master_dev_handle_t i2c_dev;
    float acc_sensitivity;
    float gyro_sensitivity;
} qmi8658_handle_t;

esp_err_t qmi8658_init(qmi8658_handle_t *handle, i2c_master_bus_handle_t bus,
                       const qmi8658_config_t *config);
esp_err_t qmi8658_read_data(qmi8658_handle_t *handle, qmi8658_data_t *data);
esp_err_t qmi8658_deinit(qmi8658_handle_t *handle);
