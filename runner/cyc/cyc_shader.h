#ifndef NESRECOMP_CYC_SHADER_H
#define NESRECOMP_CYC_SHADER_H

#include <stdbool.h>
#include <SDL.h>

typedef struct CycShader CycShader;

bool cyc_shader_available(void);
CycShader *cyc_shader_create(SDL_Renderer *renderer, const char *path);
/* Draws the game picture into the current window without acquiring the source
 * texture or changing SDL's logical viewport. False requests normal SDL copy. */
bool cyc_shader_draw(CycShader *shader, SDL_Window *window, SDL_Renderer *renderer,
                     SDL_Texture *source, const SDL_Rect *destination, bool linear);
void cyc_shader_destroy(CycShader *shader, bool context_lost);

#endif
