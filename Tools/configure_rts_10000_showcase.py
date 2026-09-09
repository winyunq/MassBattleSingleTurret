"""Configure the copied MassBattle RTS map as a 10,000-unit turret showcase."""

import unreal


MAP_PATH = "/MassBattleSingleTurret/Demo/RTS/Map_MBST_RTS_MobileFire"
PLAYER_UNITS = 5000
ENEMY_UNITS = 5000
FORMATION_SPACING = 500.0
PLAYER_ORIGIN = unreal.Vector(25000.0, -25000.0, 1124.0)
ENEMY_ORIGIN = unreal.Vector(-15000.0, 15000.0, 1124.0)


def _class_name(actor):
    actor_class = actor.get_class()
    return actor_class.get_name() if actor_class else ""


def main():
    level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level_subsystem.load_level(MAP_PATH):
        raise RuntimeError(f"Could not load RTS showcase map: {MAP_PATH}")

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = actor_subsystem.get_all_level_actors()
    demo_actors = [
        actor for actor in actors
        if isinstance(actor, unreal.MBSTRtsMobileFireDemoActor)
    ]
    if len(demo_actors) != 1:
        raise RuntimeError(
            f"Expected exactly one MBSTRtsMobileFireDemoActor, found {len(demo_actors)}"
        )

    demo = demo_actors[0]
    demo.set_editor_property("stop_to_fire_player_count", 1)
    demo.set_editor_property("fire_while_moving_player_count", PLAYER_UNITS)
    demo.set_editor_property("enemy_count", ENEMY_UNITS)
    demo.set_editor_property("formation_spacing", FORMATION_SPACING)
    demo.set_editor_property("fire_while_moving_player_origin", PLAYER_ORIGIN)
    demo.set_editor_property("enemy_origin", ENEMY_ORIGIN)
    demo.set_editor_property("spawn_only_moving_fire_in_manual_play", True)
    demo.set_editor_property("draw_world_labels", False)
    demo.set_editor_property("draw_range_guides", False)
    demo.set_editor_property("draw_fire_request_tracers", False)
    demo.set_editor_property("auto_engage_for_testing", False)

    removed_source_spawners = 0
    for actor in actors:
        if not _class_name(actor).startswith("BP_Spawner_RTS_C"):
            continue
        actor_path = actor.get_path_name()
        if not actor_subsystem.destroy_actor(actor):
            raise RuntimeError(
                f"Could not remove source RTS spawner from copied map: {actor_path}"
            )
        removed_source_spawners += 1

    if not level_subsystem.save_current_level():
        raise RuntimeError("Could not save the configured RTS showcase map")

    total = (
        demo.get_editor_property("fire_while_moving_player_count")
        + demo.get_editor_property("enemy_count")
    )
    moving_only = demo.get_editor_property("spawn_only_moving_fire_in_manual_play")
    tracers = demo.get_editor_property("draw_fire_request_tracers")
    spacing = demo.get_editor_property("formation_spacing")
    if total != 10000 or not moving_only or tracers or spacing != FORMATION_SPACING:
        raise RuntimeError(
            f"RTS showcase readback failed: total={total} moving_only={moving_only} "
            f"tracers={tracers} spacing={spacing}"
        )

    unreal.log_warning(
        "MBST_RTS_10000_CONFIGURED "
        f"map={MAP_PATH} player={PLAYER_UNITS} enemy={ENEMY_UNITS} total={total} "
        f"spacing={spacing} player_origin={PLAYER_ORIGIN} enemy_origin={ENEMY_ORIGIN} "
        f"removed_source_spawners={removed_source_spawners} "
        "single_entity_turrets=1 debug_tracers=0"
    )


main()
