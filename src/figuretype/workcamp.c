#include "workcamp.h"

#include "assets/assets.h"
#include "building/building.h"
#include "building/model.h"
#include "building/monument.h"
#include "building/storage.h"
#include "building/warehouse.h"
#include "city/gods.h"
#include "city/resource.h"
#include "core/calc.h"
#include "core/image.h"
#include "core/random.h"
#include "figure/action.h"
#include "figure/combat.h"
#include "figure/image.h"
#include "figure/movement.h"
#include "figure/route.h"
#include "map/figure.h"
#include "map/grid.h"

#define VALID_MONUMENT_RECHECK_TICKS 60

static int create_slave_workers(int leader_id, int first_figure_id)
{
    figure *f = figure_get(first_figure_id);
    figure *slave = figure_create(FIGURE_WORK_CAMP_SLAVE, f->x, f->y, 0);
    f = figure_get(first_figure_id);
    slave->leading_figure_id = leader_id;
    slave->collecting_item_id = f->collecting_item_id;
    slave->building_id = f->building_id;
    slave->destination_building_id = f->destination_building_id;
    slave->destination_x = f->destination_x;
    slave->destination_y = f->destination_y;
    slave->action_state = FIGURE_ACTION_209_WORK_CAMP_SLAVE_FOLLOWING;
    slave->wait_ticks = VALID_MONUMENT_RECHECK_TICKS;
    building_monument_add_delivery(slave->destination_building_id, slave->id, slave->collecting_item_id, 1);
    return slave->id;
}

static int take_resource_from_warehouse(figure *f, int warehouse_id)
{
    int resource = f->collecting_item_id;
    building *warehouse = building_get(warehouse_id);
    building *monument = building_get(f->destination_building_id);
    int resources_needed = monument->resources[resource] - building_monument_resource_in_delivery(monument, resource);
    int num_loads;
    int stored = building_warehouse_get_amount(warehouse, resource);
    if (stored <= CARTLOADS_PER_MONUMENT_DELIVERY) {
        num_loads = stored;
    } else {
        num_loads = CARTLOADS_PER_MONUMENT_DELIVERY;
    }
    if (num_loads > resources_needed) {
        num_loads = resources_needed;
    }

    if (num_loads <= 0) {
        return 0;
    }

    building_warehouse_remove_resource(warehouse, resource, num_loads);

    // create slave workers
    int slave = f->id;
    for (int i = 0; i < num_loads; i++) {
        slave = create_slave_workers(slave, f->id);
    }
    return 1;
}

static int has_valid_monument_destination(figure *f)
{
    if (f->wait_ticks++ >= VALID_MONUMENT_RECHECK_TICKS) {
        if (!building_monument_has_delivery_for_worker(f->id)) {
            return 0;
        }
        f->wait_ticks = 0;
    }
    return 1;
}

static void workcamp_worker_image_update(figure *f)
{
    int dir = figure_image_normalize_direction(f->direction < 8 ? f->direction : f->previous_tile_direction);
    if (f->action_state == FIGURE_ACTION_149_CORPSE) {
        f->image_id = assets_get_image_id("Walkers", "overseer_death_01") +
            figure_image_corpse_offset(f);
    } else {
        f->image_id = assets_get_image_id("Walkers", "overseer_ne_01") + dir * 12 + f->image_offset;
    }
}

void figure_workcamp_worker_action(figure *f)
{
    f->terrain_usage = TERRAIN_USAGE_ROADS_HIGHWAY;
    building *b = building_get(f->building_id);
    int monument_id;
    int warehouse_id;
    map_point dst;
    if (b->state != BUILDING_STATE_IN_USE || b->figure_id != f->id) {
        f->state = FIGURE_STATE_DEAD;
    }

    figure_image_increase_offset(f, 12);
    switch (f->action_state) {
        case FIGURE_ACTION_150_ATTACK:
            figure_combat_handle_attack(f);
            break;
        case FIGURE_ACTION_149_CORPSE:
            figure_combat_handle_corpse(f);
            break;
        case FIGURE_ACTION_203_WORK_CAMP_WORKER_CREATED:
            if (!building_monument_has_unfinished_monuments()) {
                f->state = FIGURE_STATE_DEAD;
                break;
            }
            for (int resource = RESOURCE_MIN; resource < RESOURCE_MAX; resource++) {
                if (city_resource_is_stockpiled(resource) || !resource_is_storable(resource)) {
                    continue;
                }
                monument_id = building_monument_get_monument(b->x, b->y, resource, b->road_network_id, 0);
                if (!monument_id) {
                    continue;
                }
                warehouse_id = building_warehouse_with_resource(f->x, f->y, resource, b->road_network_id, 0, &dst, BUILDING_STORAGE_PERMISSION_WORKCAMP);
                if (!warehouse_id) {
                    continue;
                }

                f->collecting_item_id = resource;
                f->destination_building_id = warehouse_id;
                f->destination_x = dst.x;
                f->destination_y = dst.y;
                f->wait_ticks = VALID_MONUMENT_RECHECK_TICKS;
                f->action_state = FIGURE_ACTION_204_WORK_CAMP_WORKER_GETTING_RESOURCES;
                building *monument = building_get(monument_id);
                int resources_needed = monument->resources[resource] - building_monument_resource_in_delivery(monument, resource);
                resources_needed = calc_bound(resources_needed, 0, CARTLOADS_PER_MONUMENT_DELIVERY);
                building_monument_add_delivery(monument_id, f->id, resource, resources_needed);
                break;
            }
            if (!f->destination_building_id) {
                f->state = FIGURE_STATE_DEAD;
            }
            break;

        case FIGURE_ACTION_204_WORK_CAMP_WORKER_GETTING_RESOURCES:
            if (!has_valid_monument_destination(f)) {
                f->state = FIGURE_STATE_DEAD;
                break;
            }
            figure_movement_move_ticks(f, 1);
            if (f->direction == DIR_FIGURE_AT_DESTINATION) {
                building_monument_remove_delivery(f->id);
                warehouse_id = f->destination_building_id;
                monument_id = building_monument_get_monument(b->x, b->y, f->collecting_item_id, b->road_network_id, &dst);
                f->action_state = FIGURE_ACTION_205_WORK_CAMP_WORKER_GOING_TO_MONUMENT;
                f->destination_building_id = monument_id;
                f->destination_x = dst.x;
                f->destination_y = dst.y;
                f->previous_tile_x = f->x;
                f->previous_tile_y = f->y;
                f->wait_ticks = VALID_MONUMENT_RECHECK_TICKS;
                if (!monument_id) {
                    f->state = FIGURE_STATE_DEAD;
                } else if (!take_resource_from_warehouse(f, warehouse_id)) {
                    f->state = FIGURE_STATE_DEAD;
                } else {
                    // Placeholder delivery
                    building_monument_add_delivery(monument_id, f->id, f->collecting_item_id, 0);
                }
            } else if (f->direction == DIR_FIGURE_REROUTE || f->direction == DIR_FIGURE_LOST) {
                f->state = FIGURE_STATE_DEAD;
            }
            break;

        case FIGURE_ACTION_205_WORK_CAMP_WORKER_GOING_TO_MONUMENT:
            if (!has_valid_monument_destination(f)) {
                f->state = FIGURE_STATE_DEAD;
                break;
            }
            figure_movement_move_ticks(f, 1);
            if (f->direction == DIR_FIGURE_AT_DESTINATION || f->direction == DIR_FIGURE_LOST) {
                f->wait_ticks = VALID_MONUMENT_RECHECK_TICKS;
                f->action_state = FIGURE_ACTION_216_WORK_CAMP_WORKER_ENTERING_MONUMENT;
                building *monument = building_get(f->destination_building_id);
                if (!building_monument_access_point(monument, &dst)) {
                    f->state = FIGURE_STATE_DEAD;
                }
                figure_movement_set_cross_country_destination(f, dst.x, dst.y);
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            }
            break;

        case FIGURE_ACTION_216_WORK_CAMP_WORKER_ENTERING_MONUMENT:
            if (!has_valid_monument_destination(f)) {
                f->state = FIGURE_STATE_DEAD;
                break;
            }
            f->terrain_usage = TERRAIN_USAGE_ANY;
            f->use_cross_country = 1;
            f->dont_draw_elevated = 1;
            if (figure_movement_move_ticks_cross_country(f, 1)) {
                f->state = FIGURE_STATE_DEAD;
            } else {
                if (f->direction == DIR_FIGURE_REROUTE) {
                    figure_route_remove(f);
                }
            }
            break;
    }

    workcamp_worker_image_update(f);
}

void figure_workcamp_slave_action(figure *f)
{
    f->is_ghost = 0;
    f->terrain_usage = TERRAIN_USAGE_ROADS_HIGHWAY;
    figure_image_increase_offset(f, 12);
    f->cart_image_id = 0;
    figure *leader = figure_get(f->leading_figure_id);
    map_point dst;
    switch (f->action_state) {
        case FIGURE_ACTION_150_ATTACK:
            figure_combat_handle_attack(f);
            break;
        case FIGURE_ACTION_149_CORPSE:
            figure_combat_handle_corpse(f);
            break;
        case FIGURE_ACTION_209_WORK_CAMP_SLAVE_FOLLOWING:
            if (!has_valid_monument_destination(f)) {
                f->state = FIGURE_STATE_DEAD;
                break;
            }
            if (f->leading_figure_id <= 0 || leader->action_state == FIGURE_ACTION_149_CORPSE) {
                f->state = FIGURE_STATE_DEAD;
            } else {
                if (leader->state == FIGURE_STATE_ALIVE) {
                    if (leader->type == FIGURE_WORK_CAMP_WORKER || leader->type == FIGURE_WORK_CAMP_SLAVE) {
                        figure_movement_follow_ticks(f, 1);
                        if (leader->action_state == FIGURE_ACTION_210_WORK_CAMP_SLAVE_GOING_TO_MONUMENT ||
                            leader->action_state == FIGURE_ACTION_216_WORK_CAMP_WORKER_ENTERING_MONUMENT) {
                            f->action_state = FIGURE_ACTION_210_WORK_CAMP_SLAVE_GOING_TO_MONUMENT;
                            f->wait_ticks = VALID_MONUMENT_RECHECK_TICKS;
                        }
                    } else {
                        f->state = FIGURE_STATE_DEAD;
                    }
                } else { // leader arrived at the monument, continue on your own
                    f->action_state = FIGURE_ACTION_210_WORK_CAMP_SLAVE_GOING_TO_MONUMENT;
                    f->wait_ticks = VALID_MONUMENT_RECHECK_TICKS;
                }
            }
            if (leader->is_ghost && !leader->height_adjusted_ticks) {
                f->is_ghost = 1;
            }
            break;

        case FIGURE_ACTION_210_WORK_CAMP_SLAVE_GOING_TO_MONUMENT:
            if (!has_valid_monument_destination(f)) {
                f->state = FIGURE_STATE_DEAD;
                break;
            }
            figure_movement_move_ticks(f, 1);
            if (f->direction == DIR_FIGURE_AT_DESTINATION || f->direction == DIR_FIGURE_LOST) {
                f->action_state = FIGURE_ACTION_211_WORK_CAMP_SLAVE_DELIVERING_RESOURCES;
                building *monument = building_get(f->destination_building_id);
                if (!building_monument_access_point(monument, &dst)) {
                    f->state = FIGURE_STATE_DEAD;
                }
                f->wait_ticks = VALID_MONUMENT_RECHECK_TICKS;
                figure_movement_set_cross_country_destination(f, dst.x, dst.y);
            } else if (f->direction == DIR_FIGURE_REROUTE) {
                figure_route_remove(f);
            }
            break;

        case FIGURE_ACTION_211_WORK_CAMP_SLAVE_DELIVERING_RESOURCES:
            if (!has_valid_monument_destination(f)) {
                f->state = FIGURE_STATE_DEAD;
                break;
            }
            f->terrain_usage = TERRAIN_USAGE_ANY;
            f->use_cross_country = 1;
            f->dont_draw_elevated = 1;
            if (figure_movement_move_ticks_cross_country(f, 1)) {
                building *monument = building_get(f->destination_building_id);
                building_monument_deliver_resource(monument, f->collecting_item_id);
                f->state = FIGURE_STATE_DEAD;
            } else {
                if (f->direction == DIR_FIGURE_REROUTE) {
                    figure_route_remove(f);
                }
            }
            break;
    }

    int dir = figure_image_normalize_direction(f->direction < 8 ? f->direction : f->previous_tile_direction);
    if (f->action_state == FIGURE_ACTION_149_CORPSE) {
        f->image_id = assets_get_image_id("Walkers", "Slave death 01") +
            figure_image_corpse_offset(f);
    } else {
        f->image_id = assets_get_image_id("Walkers", "Slave NE 01") + dir * 12 +
            f->image_offset;
    }
}

static void set_architect_graphic(figure *f, int working)
{
    int dir = figure_image_normalize_direction(f->direction < 8 ? f->direction : f->previous_tile_direction);

    if (f->action_state == FIGURE_ACTION_149_CORPSE) {
        f->image_id = assets_get_image_id("Walkers", "architect_death_01") +
            figure_image_corpse_offset(f);
    } else if (working) {
        f->image_id = assets_get_image_id("Walkers", "Architect 01") + f->image_offset;
    } else {
        f->image_id = assets_get_image_id("Walkers", "architect_ne_01") + dir * 12 +
            f->image_offset;
    }
}

void figure_workcamp_architect_action(figure *f)
{
    int working = 0;
    f->terrain_usage = TERRAIN_USAGE_ROADS_HIGHWAY;
    building *b = building_get(f->building_id);
    building *monument;
    map_point dst;
    if (b->state != BUILDING_STATE_IN_USE || b->figure_id != f->id) {
        f->state = FIGURE_STATE_DEAD;
    }
    figure_image_increase_offset(f, 12);
    switch (f->action_state) {
        case FIGURE_ACTION_150_ATTACK:
            figure_combat_handle_attack(f);
            break;
        case FIGURE_ACTION_149_CORPSE:
            figure_combat_handle_corpse(f);
            break;
        case FIGURE_ACTION_206_WORK_CAMP_ARCHITECT_CREATED:
            if (!building_monument_has_unfinished_monuments()) {
                f->state = FIGURE_STATE_DEAD;
            } else {
                int monument_id = building_monument_get_monument(b->x, b->y, RESOURCE_NONE, b->road_network_id, &dst);
                if (monument_id && !building_monument_is_construction_halted(building_get(monument_id))) {
                    f->destination_building_id = monument_id;
                    f->destination_x = dst.x;
                    f->destination_y = dst.y;
                    // Only send 1 architect
                    building_monument_add_delivery(f->destination_building_id, f->id, RESOURCE_NONE, 10);
                    f->action_state = FIGURE_ACTION_207_WORK_CAMP_ARCHITECT_GOING_TO_MONUMENT;
                    f->wait_ticks = VALID_MONUMENT_RECHECK_TICKS;
                    break;
                } else {
                    f->state = FIGURE_STATE_DEAD;
                }
            }
            break;

        case FIGURE_ACTION_207_WORK_CAMP_ARCHITECT_GOING_TO_MONUMENT:
            if (!has_valid_monument_destination(f)) {
                f->state = FIGURE_STATE_DEAD;
                break;
            }
            figure_movement_move_ticks(f, 1);
            monument = building_get(f->destination_building_id);
            if (monument->state == BUILDING_STATE_UNUSED || !building_monument_access_point(monument, &dst) ||
                b->monument.phase == MONUMENT_FINISHED) {
                f->state = FIGURE_STATE_DEAD;
            } else {
                if (f->direction == DIR_FIGURE_AT_DESTINATION || f->direction == DIR_FIGURE_LOST) {
                    f->action_state = FIGURE_ACTION_208_WORK_CAMP_ARCHITECT_WORKING_ON_MONUMENT;
                    figure_movement_set_cross_country_destination(f, dst.x, dst.y);
                    f->wait_ticks = 1;
                } else if (f->direction == DIR_FIGURE_REROUTE) {
                    figure_route_remove(f);
                }
            }
            break;
        case FIGURE_ACTION_208_WORK_CAMP_ARCHITECT_WORKING_ON_MONUMENT:
            f->terrain_usage = TERRAIN_USAGE_ANY;
            f->use_cross_country = 1;
            f->dont_draw_elevated = 1;
            if (figure_movement_move_ticks_cross_country(f, 1)) {
                if (f->wait_ticks >= 384) {
                    f->state = FIGURE_STATE_DEAD;
                    monument = building_get(f->destination_building_id);
                    monument->resources[RESOURCE_NONE]--;
                    if (monument->resources[RESOURCE_NONE] <= 0) {
                        building_monument_progress(monument);
                    }
                } else {
                    f->wait_ticks++;
                    working = 1;
                }
            } else {
                if (f->direction == DIR_FIGURE_REROUTE) {
                    figure_route_remove(f);
                }
            }
            break;
    }

    set_architect_graphic(f, working);
}
