#pragma once

#include <zephyr/kernel.h>

/*
 * One background work queue for everything network related that blocks: the Wi-Fi connect
 * request, DNS, SNTP, HTTP. Apps must not block in run(), so they queue work here and poll
 * the result. One shared thread instead of one per feature keeps RAM down; the jobs are
 * short (a few seconds at most) and run one after another.
 */
struct k_work_q *net_job_queue(void);
