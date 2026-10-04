/* Pins input-script exit semantics: a timed-out wait or failed assert must
 * not let a desynced script report success, and relative screenshot names
 * honour NESRECOMP_SHOT_DIR. */
#include "input_script.h"
#include "foreign_controller.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Runtime symbols input_script.c links against. */
uint8_t g_ram[0x800];
uint8_t g_sram[0x2000];
uint8_t g_ppu_nt[0x1000];
int g_nes_expected_exit;
int g_zapper_x, g_zapper_y, g_zapper_trigger;
int savestate_save(const char *path) { (void)path; return 1; }
int savestate_load(const char *path) { (void)path; return 1; }
void save_ram_mark_dirty(void) {}
const ForeignController *nes_foreign_active(void) { return NULL; }
ForeignState *nes_foreign_state(void) { return NULL; }

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static const char *write_script(const char *name, const char *body) {
    static char path[256];
    snprintf(path, sizeof(path), "%s", name);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(body, f);
    fclose(f);
    return path;
}

/* Run a loaded script until it exits (or a frame budget elapses). */
static int run(const char *name, const char *body) {
    memset(g_ram, 0, sizeof(g_ram));
    CHECK(script_load(write_script(name, body)));
    for (uint64_t frame = 1; frame < 10000; frame++) {
        script_tick(frame, g_ram);
        int ec = script_check_exit();
        if (ec >= 0) { remove(name); return ec; }
    }
    remove(name);
    return -1;
}

static void set_shot_dir(const char *dir) {
#ifdef _WIN32
    _putenv_s("NESRECOMP_SHOT_DIR", dir ? dir : "");
#else
    if (dir) setenv("NESRECOMP_SHOT_DIR", dir, 1);
    else unsetenv("NESRECOMP_SHOT_DIR");
#endif
}

static void test_clean_script_exits_zero(void) {
    CHECK(run("is_clean.script", "WAIT 2\nASSERT_RAM8 0010 00\nEXIT 0\n") == 0);
    CHECK(run("is_end.script", "WAIT 2\n") == 0);
}

static void test_wait_timeout_fails(void) {
    CHECK(run("is_wait.script", "WAIT_RAM8 0012 05\nEXIT 0\n") == 3);
    CHECK(run("is_wait_end.script", "WAIT_RAM8 0012 05\n") == 3);
}

static void test_assert_failure_fails(void) {
    CHECK(run("is_assert.script", "ASSERT_RAM8 0010 01 expect\nEXIT 0\n") == 3);
}

static void test_explicit_nonzero_exit_kept(void) {
    CHECK(run("is_exit7.script", "WAIT_RAM8 0012 05\nEXIT 7\n") == 7);
}

static void test_failures_reset_per_load(void) {
    CHECK(run("is_wait2.script", "WAIT_RAM8 0012 05\nEXIT 0\n") == 3);
    CHECK(run("is_clean2.script", "EXIT 0\n") == 0);
}

static void test_screenshot_dir(void) {
    char buf[256];
    set_shot_dir(NULL);
    CHECK(run("is_shot.script", "SCREENSHOT a.png\nWAIT 1\nEXIT 0\n") == 0);
    /* run() exits before the runner would consume it; consume directly. */
    CHECK(script_load(write_script("is_shot2.script", "SCREENSHOT a.png\nWAIT 5\n")));
    script_tick(1, g_ram);
    CHECK(script_wants_screenshot(buf, sizeof(buf)) && strcmp(buf, "C:/temp/a.png") == 0);

    set_shot_dir("out/shots");
    CHECK(script_load(write_script("is_shot2.script", "SCREENSHOT b.png\nWAIT 5\n")));
    script_tick(1, g_ram);
    CHECK(script_wants_screenshot(buf, sizeof(buf)) && strcmp(buf, "out/shots/b.png") == 0);

    set_shot_dir("out/shots/");
    CHECK(script_load(write_script("is_shot2.script", "SCREENSHOT c.png\nWAIT 5\n")));
    script_tick(1, g_ram);
    CHECK(script_wants_screenshot(buf, sizeof(buf)) && strcmp(buf, "out/shots/c.png") == 0);

    CHECK(script_load(write_script("is_shot2.script", "SCREENSHOT D:/abs/d.png\nWAIT 5\n")));
    script_tick(1, g_ram);
    CHECK(script_wants_screenshot(buf, sizeof(buf)) && strcmp(buf, "D:/abs/d.png") == 0);
    remove("is_shot2.script");
    set_shot_dir(NULL);
}

int main(void) {
    test_clean_script_exits_zero();
    test_wait_timeout_fails();
    test_assert_failure_fails();
    test_explicit_nonzero_exit_kept();
    test_failures_reset_per_load();
    test_screenshot_dir();
    if (failures) {
        fprintf(stderr, "input_script_selftest: %d failure(s)\n", failures);
        return 1;
    }
    printf("input_script_selftest: OK\n");
    return 0;
}
