#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include "net_job.h"

// stack 是 noinit，在 DRAM region 1
static K_THREAD_STACK_DEFINE(net_wq_stack, CONFIG_NET_JOB_STACK_SIZE);
static struct k_work_q net_wq;

struct k_work_q *net_job_queue(void)
{
    return &net_wq;
}

// 比所有 APPLICATION 等級的 SYS_INIT 早，wlan / net_time 初始化時就能排工作
static int net_job_init(void)
{
    const struct k_work_queue_config cfg = { .name = "net_wq" };

    k_work_queue_start(&net_wq, net_wq_stack, K_THREAD_STACK_SIZEOF(net_wq_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, &cfg);
    return 0;
}

SYS_INIT(net_job_init, POST_KERNEL, 99);
