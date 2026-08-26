#ifndef __COMMON_H
#define __COMMON_H

#include <stdint.h>
#include <time.h>

typedef struct {
    char uid[9];
    char name[32];
    char phone[20];
    double balance;
    char face_path[128];
    char face_feature[2048];
    char password[61];   /* bcrypt hash ($2b$12$...), max 60 chars */
    int member_type;    /* 0=normal customer, 1=admin */
    time_t create_time;
} member_info_t;

typedef struct {
    int id;
    char name[64];
    double price;
    char unit[16];
    int stock;
    time_t create_time;
    int version;
} goods_info_t;

typedef struct {
    int goods_id;
    char goods_name[64];
    int num;
    double price;
    double subtotal;
} order_item_t;

typedef struct {
    int id;
    char order_id[32];
    char member_uid[9];
    char goods_list[4096];
    double total;
    int pay_status;     /* 0=unpaid, 1=paid */
    time_t create_time;
} order_info_t;

/*
 * OTA types:
 *   APP    : lightweight app-level OTA (replaces executables under /opt/retail,
 *            tar.gz backup for rollback)
 *   SYSTEM : full system-level OTA (A/B partitions + swupdate + libubootenv,
 *            U-Boot bootcount rollback)
 */
typedef enum {
    OTA_TYPE_APP    = 0,
    OTA_TYPE_SYSTEM = 1
} ota_type_t;

typedef struct {
    int id;
    char version[32];
    char filename[128];
    char sha256[65];
    int file_size;
    char description[256];
    char upload_time[32];
    int force_update;
    ota_type_t type;   /* 0=APP, 1=SYSTEM; default APP for backward compat */
} ota_version_t;

#endif
