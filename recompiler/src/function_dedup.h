#pragma once

#include <stdbool.h>

#include "game_config.h"

bool function_dedup_run(const char *const *paths, int path_count,
                        const GameConfig *cfg, int fixed_bank,
                        const char *manifest_path);
