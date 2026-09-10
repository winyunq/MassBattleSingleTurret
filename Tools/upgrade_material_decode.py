"""Upgrade MBST material decoding and masked WPO depth rendering in Unreal.

Run directly for the plugin master, or call upgrade_materials([asset_paths])
for project-owned copies. Each material is compiled and checked before saving.
"""

import unreal


BASE_MATERIAL = "/MassBattleSingleTurret/Materials/M_MBST_VATSingleTurret"
DECODE_INCLUDE = "/MassBattle/MassBattle_MaterialDecode.ush"


def upgrade_material(material_path):
    material = unreal.load_asset(material_path)
    if not material:
        raise RuntimeError(f"Missing turret material: {material_path}")
    result = unreal.MBSTSingleTurretEditorLibrary.configure_articulation_base_material(material)
    if isinstance(result, tuple):
        success, message = result
    else:
        success, message = isinstance(result, str), str(result)
    if not success:
        raise RuntimeError(message)

    decoders = [
        expression
        for expression in unreal.MaterialEditingLibrary.get_material_expressions(material)
        if isinstance(expression, unreal.MaterialExpressionCustom)
        and "MassBattle_UnpackDP0W(" in expression.get_editor_property("code")
    ]
    if len(decoders) != 1 or DECODE_INCLUDE not in decoders[0].get_editor_property("include_file_paths"):
        raise RuntimeError("Expected exactly one canonical framework DP0.w decoder")
    wrappers = [
        expression
        for expression in unreal.MaterialEditingLibrary.get_material_expressions(material)
        if isinstance(expression, unreal.MaterialExpressionCustom)
        and expression.get_editor_property("desc") == "MBST masked depth: preserve final opacity mask"
        and expression.get_editor_property("code") == "return Mask;"
    ]
    if not wrappers:
        raise RuntimeError("The final opacity mask compatibility expression is missing")
    errors = unreal.MaterialEditingLibrary.recompile_material(material)
    if errors:
        raise RuntimeError(f"Material compile failed: {errors}")
    if not unreal.get_editor_subsystem(unreal.EditorAssetSubsystem).save_loaded_asset(material, False):
        raise RuntimeError("Could not save the migrated turret material")
    unreal.log("MBST_MATERIAL_MIGRATION: compiled and saved " + material.get_path_name())
    return material.get_path_name()


def upgrade_materials(material_paths):
    return [upgrade_material(path) for path in dict.fromkeys(material_paths)]


if __name__ == "__main__":
    upgrade_materials([BASE_MATERIAL])
