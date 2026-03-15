/*
 * games/super-mario-bros/extras.c — Super Mario Bros. runner hooks
 *
 * No game-specific features yet. Implements the empty stubs required
 * by game_extras.h so the runner links cleanly.
 */
#include "game_extras.h"
#include "nes_runtime.h"

const char *game_get_name(void)                          { return "Super Mario Bros."; }
void        game_on_init(void)                           {}
void        game_on_frame(uint64_t frame_count) {
    (void)frame_count;
}
int         game_handle_arg(const char *key, const char *val) { (void)key; (void)val; return 0; }
const char *game_arg_usage(void)                         { return NULL; }
