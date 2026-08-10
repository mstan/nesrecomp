#include "foreign_controller.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int alpha_private;
static int beta_private;
static int alpha_get_calls;
static int alpha_set_calls;
static int beta_get_calls;
static int beta_set_calls;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void reset_alpha(ForeignState *state) {
    alpha_private = 11;
    state->state = 101;
    state->x = 1.0;
    state->facing = 1.0f;
}

static void reset_beta(ForeignState *state) {
    beta_private = 22;
    state->state = 202;
    state->x = 2.0;
    state->facing = -1.0f;
}

static void tick(ForeignState *state, const ForeignInput *input,
                 ForeignMoveResult *out) {
    (void)input;
    out->state = state->state;
}

static void resolve(ForeignState *state, const ForeignCollisionResult *hit) {
    (void)state;
    (void)hit;
}

static int get_alpha(const ForeignState *state, uint8_t *buf, int cap) {
    (void)state;
    alpha_get_calls++;
    if (cap < 1) return -1;
    buf[0] = (uint8_t)alpha_private;
    return 1;
}

static int set_alpha(ForeignState *state, const uint8_t *buf, int len) {
    (void)state;
    alpha_set_calls++;
    if (len != 1 || buf[0] == 0xff) return 0;
    alpha_private = buf[0];
    return 1;
}

static int get_beta(const ForeignState *state, uint8_t *buf, int cap) {
    (void)state;
    beta_get_calls++;
    if (cap < 1) return -1;
    buf[0] = (uint8_t)beta_private;
    return 1;
}

static int set_beta(ForeignState *state, const uint8_t *buf, int len) {
    (void)state;
    beta_set_calls++;
    if (len != 1 || buf[0] == 0xff) return 0;
    beta_private = buf[0];
    return 1;
}

static int get_empty(const ForeignState *state, uint8_t *buf, int cap) {
    (void)state;
    (void)buf;
    (void)cap;
    return 0;
}

static int set_empty(ForeignState *state, const uint8_t *buf, int len) {
    (void)state;
    (void)buf;
    return len == 0;
}

/* Six fields intentionally prove source compatibility with old positional
 * ForeignController initializers. It has no private callbacks. */
static const ForeignController legacy = {
    "legacy", "Legacy", reset_alpha, tick, resolve, NULL
};

static const ForeignController alpha = {
    "alpha", "Alpha", reset_alpha, tick, resolve, NULL
};

static const ForeignController beta = {
    "beta", "Beta", reset_beta, tick, resolve, NULL
};

static const ForeignController empty = {
    "empty", "Empty", reset_alpha, tick, resolve, NULL
};

static void copy_state(ForeignState *out) {
    ForeignState *state = nes_foreign_state();
    CHECK(state != NULL);
    if (state) *out = *state;
}

int main(void) {
    uint8_t record[512];
    uint8_t bad[512];
    ForeignState saved_alpha, beta_before, alpha_before;
    int length;

    CHECK(nes_foreign_register(&legacy));
    CHECK(nes_foreign_register(&alpha));
    CHECK(nes_foreign_register(&beta));
    CHECK(nes_foreign_register(&empty));
    CHECK(!nes_foreign_register_private_state("unknown", get_alpha, set_alpha));
    CHECK(!nes_foreign_register_private_state("alpha", get_alpha, NULL));
    CHECK(nes_foreign_register_private_state("alpha", get_alpha, set_alpha));
    CHECK(nes_foreign_register_private_state("beta", get_beta, set_beta));
    CHECK(nes_foreign_register_private_state("empty", get_empty, set_empty));

    /* Legacy callers still register/select/tick and serialize only the
     * engine-owned state. */
    CHECK(nes_foreign_select("legacy"));
    nes_foreign_state()->state = 77;
    nes_foreign_state()->x = 9.5;
    length = nes_foreign_serialize_active(record, (int)sizeof(record));
    CHECK(length > 0);
    CHECK(nes_foreign_state()->state == 77);
    nes_foreign_state()->state = 0;
    nes_foreign_state()->x = 0.0;
    CHECK(nes_foreign_deserialize_active(record, length));
    CHECK(nes_foreign_state()->state == 77);
    CHECK(nes_foreign_state()->x == 9.5);

    /* A valid-looking private payload cannot be injected into an old
     * controller that deliberately has no private deserializer. */
    memcpy(bad, record, (size_t)length);
    bad[4] = 1;
    bad[6] = 1;
    bad[length] = 0x44;
    CHECK(!nes_foreign_deserialize_active(bad, length + 1));
    CHECK(nes_foreign_state()->state == 77);

    /* Presence is explicit, so a valid controller-private extension may
     * intentionally carry zero bytes. */
    CHECK(nes_foreign_select("empty"));
    nes_foreign_state()->state = 606;
    length = nes_foreign_serialize_active(record, (int)sizeof(record));
    CHECK(length > 0);
    nes_foreign_state()->state = 0;
    CHECK(nes_foreign_deserialize_active(record, length));
    CHECK(nes_foreign_state()->state == 606);

    CHECK(nes_foreign_select("alpha"));
    nes_foreign_state()->state = 313;
    nes_foreign_state()->x = 41.25;
    nes_foreign_state()->vy = -7.0;
    alpha_private = 42;
    copy_state(&saved_alpha);
    length = nes_foreign_serialize_active(record, (int)sizeof(record));
    CHECK(length > 0);
    CHECK(alpha_get_calls == 1);
    CHECK(beta_get_calls == 0);

    /* A record must never select another controller or restore into one. */
    CHECK(nes_foreign_select("beta"));
    beta_private = 77;
    nes_foreign_state()->state = 909;
    nes_foreign_state()->x = -3.0;
    copy_state(&beta_before);
    CHECK(!nes_foreign_deserialize_active(record, length));
    CHECK(beta_set_calls == 0);
    CHECK(beta_private == 77);
    CHECK(memcmp(nes_foreign_state(), &beta_before, sizeof(beta_before)) == 0);

    /* Structural corruption is rejected before a callback can mutate. */
    memcpy(bad, record, (size_t)length);
    bad[0] = 99;
    CHECK(!nes_foreign_deserialize_active(bad, length));
    CHECK(beta_set_calls == 0);
    CHECK(!nes_foreign_deserialize_active(record, length - 1));
    CHECK(beta_set_calls == 0);

    CHECK(nes_foreign_select("alpha"));
    alpha_private = 3;
    nes_foreign_state()->state = 1;
    nes_foreign_state()->x = 2.0;
    CHECK(nes_foreign_deserialize_active(record, length));
    CHECK(alpha_set_calls == 1);
    CHECK(alpha_private == 42);
    CHECK(memcmp(nes_foreign_state(), &saved_alpha, sizeof(saved_alpha)) == 0);

    /* A syntactically valid private payload that the controller rejects must
     * leave engine state intact. */
    copy_state(&alpha_before);
    memcpy(bad, record, (size_t)length);
    bad[length - 1] = 0xff;
    alpha_private = 55;
    CHECK(!nes_foreign_deserialize_active(bad, length));
    CHECK(alpha_set_calls == 2);
    CHECK(alpha_private == 55);
    CHECK(memcmp(nes_foreign_state(), &alpha_before, sizeof(alpha_before)) == 0);

    CHECK(nes_foreign_select(NULL));
    CHECK(nes_foreign_serialize_active(record, (int)sizeof(record)) == 0);
    CHECK(!nes_foreign_deserialize_active(record, length));

    if (failures) {
        fprintf(stderr, "%d foreign-controller selftest failure(s)\n", failures);
        return 1;
    }
    printf("foreign-controller selftest: PASS\n");
    return 0;
}
