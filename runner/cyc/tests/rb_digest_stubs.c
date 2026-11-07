/* Small deterministic machine fixture for rollback image/input ownership. */
#include "cyc_state.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
uint8_t g_logical_input[4];
uint32_t g_game_input[4];
unsigned rb_test_loaded;
bool cyc_state_save(uint8_t **out,size_t *len,char *err,size_t err_len) {
    (void)err;(void)err_len;*len=32;*out=calloc(1,*len);return *out!=NULL;
}
bool cyc_state_load(const uint8_t *buf,size_t len,char *err,size_t err_len) {
    (void)buf;(void)err;(void)err_len;return len==32;
}
void cyc_session_state_loaded(void) {rb_test_loaded++;}
