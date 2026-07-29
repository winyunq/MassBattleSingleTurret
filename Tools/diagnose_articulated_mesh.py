import unreal


MESH_PATH = "/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret"
LAYOUT_PATH = "/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_SingleTurret"
SOURCE_BLUEPRINT_PATH = "/MassBattle/Test/CompoundUnitAsset/BP_TankActor"


def log_value(label, value):
    unreal.log_warning(f"MBST_DIAG {label}={value}")


mesh = unreal.load_asset(MESH_PATH)
layout = unreal.load_asset(LAYOUT_PATH)
log_value("mesh", mesh)
log_value("layout", layout)

if mesh:
    validation = unreal.MBSTSingleTurretEditorLibrary.validate_generated_articulated_mesh(mesh, False)
    log_value("mesh_validation", validation)
    log_value("mesh_lods", mesh.get_num_lods())
    try:
        log_value("static_materials", mesh.get_editor_property("static_materials"))
    except Exception as exc:
        log_value("static_materials_error", exc)

if layout:
    for property_name in (
        "turret_pivot_object_space",
        "turret_axis_object_space",
        "barrel_pivot_object_space",
        "barrel_axis_object_space",
        "barrel_forward_axis_object_space",
        "body_source_components",
        "turret_source_components",
        "barrel_source_components",
    ):
        try:
            log_value(property_name, layout.get_editor_property(property_name))
        except Exception as exc:
            log_value(f"{property_name}_error", exc)

source_blueprint = unreal.load_asset(SOURCE_BLUEPRINT_PATH)
if source_blueprint:
    try:
        editor_world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
        source_actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
            source_blueprint.generated_class(), unreal.Vector(), unreal.Rotator())
        log_value("source_actor", source_actor)
        for component in source_actor.get_components_by_class(unreal.SceneComponent):
            parent = component.get_attach_parent()
            mesh_value = None
            if isinstance(component, unreal.StaticMeshComponent):
                mesh_value = component.get_editor_property("static_mesh")
            elif isinstance(component, unreal.SkeletalMeshComponent):
                mesh_value = component.get_editor_property("skeletal_mesh_asset")
            log_value(
                "component",
                {
                    "name": component.get_name(),
                    "class": component.get_class().get_name(),
                    "parent": parent.get_name() if parent else None,
                    "relative": component.get_relative_transform(),
                    "world": component.get_world_transform(),
                    "mesh": str(mesh_value) if mesh_value else None,
                    "visible": component.is_visible(),
                },
            )
        source_actor.destroy_actor()
    except Exception as exc:
        log_value("source_actor_error", exc)
