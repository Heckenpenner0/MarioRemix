/* hub.h - the shared crossover engine.
 * One hub world hosts two game "worlds", each driven by a CPU core (the GB
 * world via LR35902, the SNES world via 65816) behind a common machine
 * interface. Entities and a tile map live at the hub level so content is
 * shared and can move between the two worlds. Portable C99.
 */
#ifndef HUB_H
#define HUB_H

#include <stdint.h>
#include <stdbool.h>
#include "lr35902.h"
#include "w65816.h"

#define HUB_WORLDS       2
#define HUB_MAX_ENTITIES 256
#define HUB_MAP_W        64
#define HUB_MAP_H        64

/* A machine wraps one CPU core behind a uniform step function so the hub does
 * not care which CPU a world uses. `step` runs one instruction, returns cycles. */
typedef struct {
    int   world;                 /* 0 = GB world, 1 = SNES world */
    int (*step)(void *state);    /* run one instruction */
    void *state;                 /* the concrete cpu struct */
} hub_machine;

typedef struct {
    float   x, y, vx, vy;
    uint8_t world;   /* which world it currently inhabits */
    uint8_t kind;    /* gameplay-defined type */
    bool    alive;
} hub_entity;

typedef struct {
    hub_machine machines[HUB_WORLDS];
    int         n_machines;

    hub_entity  ents[HUB_MAX_ENTITIES];
    int         n_ents;

    uint8_t     map[HUB_MAP_W * HUB_MAP_H];
    uint64_t    cycles;          /* total hub cycles executed */
} hub;

void hub_init(hub *h);
int  hub_add_machine(hub *h, int world, int (*step)(void*), void *state);
int  hub_add_entity(hub *h, float x, float y, uint8_t world, uint8_t kind);
bool hub_warp_entity(hub *h, int ent, uint8_t to_world);

/* Advance every machine one instruction and integrate entities. */
void hub_tick(hub *h);

/* Advance each machine n instructions (a frame's worth of CPU work). */
void hub_run(hub *h, int steps_per_machine);

/* Tile map access (bounds-checked). */
bool hub_set_tile(hub *h, int x, int y, uint8_t v);
uint8_t hub_get_tile(const hub *h, int x, int y);

#endif /* HUB_H */

/* Step adapters so the generic hub_machine can drive each concrete core. */
int hub_step_lr(void *state);  /* for lr_cpu  (GB world)   */
int hub_step_w(void *state);   /* for w_cpu   (SNES world) */
