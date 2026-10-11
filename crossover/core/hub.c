/* hub.c - shared crossover engine implementation. See hub.h. */
#include "hub.h"
#include <string.h>

void hub_init(hub *h) {
    memset(h, 0, sizeof *h);
}

int hub_add_machine(hub *h, int world, int (*step)(void*), void *state) {
    if (h->n_machines >= HUB_WORLDS || !step) return -1;
    h->machines[h->n_machines].world = world;
    h->machines[h->n_machines].step  = step;
    h->machines[h->n_machines].state = state;
    return h->n_machines++;
}

int hub_add_entity(hub *h, float x, float y, uint8_t world, uint8_t kind) {
    if (h->n_ents >= HUB_MAX_ENTITIES) return -1;
    int i = h->n_ents++;
    h->ents[i].x = x; h->ents[i].y = y;
    h->ents[i].vx = 0; h->ents[i].vy = 0;
    h->ents[i].world = world; h->ents[i].kind = kind;
    h->ents[i].alive = true;
    return i;
}

bool hub_warp_entity(hub *h, int ent, uint8_t to_world) {
    if (ent < 0 || ent >= h->n_ents || to_world >= HUB_WORLDS) return false;
    h->ents[ent].world = to_world;
    return true;
}

void hub_tick(hub *h) {
    /* one instruction per machine */
    for (int i = 0; i < h->n_machines; i++) {
        h->machines[i].step(h->machines[i].state);
    }
    /* integrate entities */
    for (int i = 0; i < h->n_ents; i++) {
        if (!h->ents[i].alive) continue;
        h->ents[i].x += h->ents[i].vx;
        h->ents[i].y += h->ents[i].vy;
    }
    h->cycles++;
}

void hub_run(hub *h, int steps_per_machine) {
    for (int i = 0; i < steps_per_machine; i++) hub_tick(h);
}

bool hub_set_tile(hub *h, int x, int y, uint8_t v) {
    if (x < 0 || x >= HUB_MAP_W || y < 0 || y >= HUB_MAP_H) return false;
    h->map[y * HUB_MAP_W + x] = v;
    return true;
}

uint8_t hub_get_tile(const hub *h, int x, int y) {
    if (x < 0 || x >= HUB_MAP_W || y < 0 || y >= HUB_MAP_H) return 0;
    return h->map[y * HUB_MAP_W + x];
}

/* ---- uniform step adapters for the two cores ---- */
int hub_step_lr(void *state)  { return lr_step((lr_cpu*)state); }
int hub_step_w(void *state)   { return w_step((w_cpu*)state);   }
