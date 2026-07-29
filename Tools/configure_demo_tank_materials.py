"""Assign and configure the demo's articulation material instances."""

import unreal


MESH_PATH = "/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret"
LAYOUT_PATH = "/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_SingleTurret"
BASE_MATERIAL_PATH = "/MassBattleSingleTurret/Materials/M_MBST_VATSingleTurret"
MATERIAL_PATHS = (
    "/MassBattleSingleTurret/Demo/Tank/Materials/MI_Tank_Vehicle_LOD0_MBST",
    "/MassBattleSingleTurret/Demo/Tank/Materials/MI_Tank_Vehicle_LOD1_MBST",
    "/MassBattleSingleTurret/Demo/Tank/Materials/MI_Tank_Vehicle_LOD2_MBST",
    "/MassBattleSingleTurret/Demo/Tank/Materials/MI_Tank_Turret_LOD0_MBST",
    "/MassBattleSingleTurret/Demo/Tank/Materials/MI_Tank_Turret_LOD1_MBST",
    "/MassBattleSingleTurret/Demo/Tank/Materials/MI_Tank_Turret_LOD2_MBST",
)


def main():
    mesh = unreal.load_asset(MESH_PATH)
    layout = unreal.load_asset(LAYOUT_PATH)
    base_material = unreal.load_asset(BASE_MATERIAL_PATH)
    if not mesh or not layout or not base_material:
        raise RuntimeError("Generated demo mesh, layout or MBST base material is missing")

    result = unreal.MBSTSingleTurretEditorLibrary.configure_articulation_base_material(
        base_material,
    )
    if isinstance(result, tuple):
        configured, message = result
    elif isinstance(result, str):
        configured, message = True, result
    else:
        configured, message = bool(result), str(result)
    unreal.log_warning(
        f"MBST_BASE_MATERIAL material={base_material.get_path_name()} "
        f"configured={configured} message={message}"
    )
    if not configured:
        raise RuntimeError(message)

    materials = [unreal.load_asset(path) for path in MATERIAL_PATHS]
    if any(material is None for material in materials):
        raise RuntimeError("One or more demo articulation material instances are missing")

    for slot_index, material in enumerate(materials):
        result = unreal.MBSTSingleTurretEditorLibrary.configure_articulation_material_instance(
            material,
            layout,
        )
        if isinstance(result, tuple):
            configured, message = result
        elif isinstance(result, str):
            configured, message = True, result
        else:
            configured, message = bool(result), str(result)
        unreal.log_warning(
            f"MBST_MATERIAL slot={slot_index} material={material.get_path_name()} "
            f"configured={configured} message={message}"
        )
        if not configured:
            raise RuntimeError(message)
        mesh.set_material(slot_index, material)

    mesh.modify()
    asset_subsystem = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    if not asset_subsystem.save_loaded_assets([base_material, mesh, *materials], False):
        raise RuntimeError("Could not save the configured base material, mesh or material instances")


main()
