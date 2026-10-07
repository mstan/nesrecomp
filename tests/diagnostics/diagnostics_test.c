/* A separate fault-drill executable. No deliberate crash switches ship in games. */
#include "cyc_diagnostics.h"
#include "../../runner/include/mod_runtime.h"
#include "recomp_launcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static int selected, fail_commit;
int nes_mod_feature_selected(const char *package, const char *feature) {
    return selected && !strcmp(package, "nesrecomp.developer.diagnostics") && !strcmp(feature, "diagnostics");
}
int nes_mod_register_activation_plugin(const char *id, NESModActivationCallback callback) {
    return id && callback;
}
static int enable_feature(void *ctx, const char *package, const char *feature, int enabled) {
    (void)ctx; (void)package; (void)feature; selected = enabled; return 1;
}
static int commit(void *ctx, const char *path) {
    (void)ctx; (void)path;
    assert(cyc_diagnostics_active()); /* first enable catches failures BEFORE commit returns */
    return !fail_commit;
}
static const char *last_error(void *ctx) { (void)ctx; return "injected commit failure"; }
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static unsigned exhaust_stack(unsigned depth) {
    volatile unsigned char block[8192]; memset((void *)block, (int)depth, sizeof(block));
    unsigned next = exhaust_stack(depth + 1);
    return next + block[depth & 8191];
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "off")) {
        cyc_diagnostics_sync(); cyc_diagnostics_note("must not create files");
        assert(!cyc_diagnostics_active()); return 0;
    }
    if (!strcmp(argv[1], "io-failure")) {
        assert(!cyc_diagnostics_enable()); assert(!cyc_diagnostics_active()); return 0;
    }
    if (!strcmp(argv[1], "gui-streams")) {
#ifdef _WIN32
        freopen("NUL", "w", stdout); freopen("NUL", "w", stderr);
#else
        freopen("/dev/null", "w", stdout); freopen("/dev/null", "w", stderr);
#endif
    }
    RecompLauncherCModProvider original = {0};
    original.feature_enable = enable_feature; original.commit = commit; original.last_error = last_error;
    const RecompLauncherCModProvider *wrapped = cyc_diagnostics_provider(&original);
    assert(!cyc_diagnostics_active());
    assert(wrapped->feature_enable(NULL, "nesrecomp.developer.diagnostics", "diagnostics", 1));
    assert(cyc_diagnostics_active());
    fail_commit = !strcmp(argv[1], "commit-fail");
    assert(wrapped->commit(NULL, "unused.nes") == !fail_commit);
    cyc_diagnostics_frame(300, 9000000, 0xABCD);
    printf("stdout preserved\n"); fprintf(stderr, "stderr preserved\n");
    cyc_diagnostics_error("known startup error\n");
    if (!strcmp(argv[1], "fault")) { *(volatile unsigned *)0 = 42; }
    if (!strcmp(argv[1], "abort")) abort();
    if (!strcmp(argv[1], "stack")) return (int)exhaust_stack(1);
    if (!strcmp(argv[1], "cycle")) {
        wrapped->feature_enable(NULL, "nesrecomp.developer.diagnostics", "diagnostics", 0);
        assert(!cyc_diagnostics_active());
        wrapped->feature_enable(NULL, "nesrecomp.developer.diagnostics", "diagnostics", 1);
        assert(cyc_diagnostics_active());
    }
    cyc_diagnostics_exit(0); cyc_diagnostics_disable();
    assert(!cyc_diagnostics_active()); return 0;
}
