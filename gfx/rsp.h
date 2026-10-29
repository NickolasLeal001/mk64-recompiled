#ifndef RSP_H
#define RSP_H

#include <stdint.h>
#include <stdio.h>

extern FILE *rsp_trace;  // set to trace every display list command

void rsp_reset(void);
// Runs a graphics task's display list (OSTask data_ptr) to completion.
void rsp_run_display_list(uint8_t *rdram, uint32_t dl);
// Runs the list of the last rsp_run_display_list again, from the same starting state, for display
// only, with each matrix blended (t = 0..1) from the matching matrix of the run before; RDRAM and
// the RSP/RDP state are left as they are.
void rsp_run_display_list_interp(uint8_t *rdram, uint32_t dl, float t);
extern uint64_t rsp_stats_tris;

// Tile sampled by the RDP: the G_TEXTURE tile for triangles, or a texrect's own tile.
int rsp_current_tile(void);
void rsp_set_rect_tile(int tile);  // -1 restores the G_TEXTURE tile

#endif
