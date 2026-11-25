#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <ff.h>
#include "file.h"

LOG_MODULE_REGISTER(file, CONFIG_CUBE_LOG_LEVEL);

static FATFS sd_fatfs;
static struct fs_mount_t sd_mnt = {
    .type = FS_FATFS,
    .mnt_point = FILE_ROOT,
    .fs_data = &sd_fatfs,
    .storage_dev = (void *)"SD",
};

bool file_exists(const char *path)
{
    // app thread、shell 和 net_wq 都會呼叫，不能共用 static（帶著 256 bytes 的檔名）
    struct fs_dirent entry;
    return fs_stat(path, &entry) == 0;
}

static int file_init(void)
{
    int rc;

    rc = fs_mount(&sd_mnt);
    if (rc < 0) {
        LOG_ERR("Failed to mount SD card (%d)", rc);
        return rc;
    }

    LOG_INF("SD card mounted at %s", sd_mnt.mnt_point);
    return 0;
}

SYS_INIT(file_init, APPLICATION, CONFIG_FILE_INIT_PRIORITY);
