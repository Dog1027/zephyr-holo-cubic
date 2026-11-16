#pragma once

#include "app.h"

// Add an app, kept sorted by prio (called by APP_REGISTER at boot)
int app_list_insert(const app_t *app);

// App at index (0 is the background app), NULL past the end
const app_t *app_list_get(int index);
