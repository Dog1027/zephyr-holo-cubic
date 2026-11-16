#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "app_list.h"

LOG_MODULE_REGISTER(app_list, CONFIG_CUBE_LOG_LEVEL);

static const app_t *apps[CONFIG_APP_MAX];
static int app_count;

int app_list_insert(const app_t *app)
{
    if (app == NULL) {
        return -EINVAL;
    }
    if (app_count >= CONFIG_APP_MAX) {
        LOG_ERR("Too many apps, raise CONFIG_APP_MAX (%s not added)", app->name);
        return -ENOMEM;
    }

    int i = app_count;
    while (i > 0 && apps[i - 1]->prio > app->prio) {
        apps[i] = apps[i - 1];
        i--;
    }
    apps[i] = app;
    app_count++;
    return 0;
}

const app_t *app_list_get(int index)
{
    return (index >= 0 && index < app_count) ? apps[index] : NULL;
}
