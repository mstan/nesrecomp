/* Standalone whole-machine host for HD pipeline fixtures, with no generated
 * ROM identity or game-specific code. Also usable for inspecting a local pack. */
#include "cyc_host_extras.h"
#include "cyc_presentation.h"
#include "cyc_hdpack.h"
#include "cyc_core.h"
#include <string.h>
static void power_on(void *ctx){(void)ctx;cyc_presentation_sync();cyc_hdpack_power_on();}
static const uint32_t *present(void *ctx,int *w,int *h){(void)ctx;cyc_presentation_sync();return cyc_hdpack_present(cyc_frame_argb(),w,h);}
static bool option(void *ctx,const char *name,const char *value){(void)ctx;(void)name;cyc_hdpack_config(strcmp(value,"off")!=0,strcmp(value,"off")?value:"");return true;}
const CycHostExtras *cyc_host_extras(void){
    static const CycHostOption options[]={{"--hdpack",true,"Texture pack folder, or off"}};
    static const CycHostExtras extras={.power_on=power_on,.present=present,.options=options,.option_count=1,.option=option};return &extras;
}
