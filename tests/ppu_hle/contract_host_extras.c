#include "cyc_host_extras.h"

/* A strong game-neutral definition avoids MinGW PE weak-alias linkage. */
const CycHostExtras *cyc_host_extras(void) { return NULL; }
