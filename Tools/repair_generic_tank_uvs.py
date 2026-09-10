"""Restore the discarded UV1 on the project's existing generic tank meshes.

Run in an editor with PIE stopped and source control disabled. ``prepare()``
creates unsaved donors and copies, then checks every selected mesh before any
formal asset changes. Call ``apply_prepared()`` and inspect its report before
``save_prepared()``. A bare script run prepares only; it never saves assets.

The donor supplies UV1 only. Geometry, vertex colors (including casemate masks),
normals, tangents, sections and build settings come from a copy of the existing
formal mesh. Unit configs, layouts, materials and source meshes are not edited.
"""

import hashlib
import json
import math
from pathlib import Path
import uuid

import unreal


_PLAN = None  # Retains unsaved donor/working/rollback assets until verification.
_ROOT = Path(unreal.Paths.project_dir()).resolve()
_SUB = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
_LIB = unreal.MBSTSingleTurretEditorLibrary
_TOOLS = unreal.AssetToolsHelpers.get_asset_tools()


def _require(value, message):
    if not value:
        raise RuntimeError(message)
    return value


def _load_mesh(path):
    value = unreal.load_asset(path)
    _require(isinstance(value, unreal.StaticMesh), "Missing StaticMesh: " + path)
    return value


def _id_value(value):
    return value.get_editor_property("id_value")


def _vec(value):
    return (float(value.x), float(value.y), float(value.z))


def _uv(value):
    result = (float(value.x), float(value.y))
    _require(all(math.isfinite(x) for x in result), "Non-finite UV")
    return result


def _description(mesh, channel=0):
    desc = _require(mesh.get_static_mesh_description(0), "Missing LOD0 source data")
    vertices = []
    for index in range(desc.get_vertex_instance_count()):
        instance = unreal.VertexInstanceID(index)
        _require(desc.is_vertex_instance_valid(instance), "Sparse vertex IDs require manual review")
        vertex = desc.get_vertex_instance_vertex(instance)
        vertices.append((_id_value(vertex), _vec(desc.get_vertex_position(vertex)),
                         _uv(desc.get_vertex_instance_uv(instance, channel))))
    triangles = []
    for index in range(desc.get_triangle_count()):
        triangle = unreal.TriangleID(index)
        _require(desc.is_triangle_valid(triangle), "Sparse triangle IDs require manual review")
        triangles.append((_id_value(desc.get_triangle_polygon_group(triangle)),
                          tuple(_id_value(x) for x in desc.get_triangle_vertex_instances(triangle))))
    return {"vertices": vertices, "triangles": triangles,
            "vertex_count": desc.get_vertex_count(), "edge_count": desc.get_edge_count(),
            "polygon_count": desc.get_polygon_count(), "group_count": desc.get_polygon_group_count()}


def _fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode("utf-8")).hexdigest()


def _settings(mesh):
    slots = list(mesh.get_editor_property("static_materials"))
    materials = [slot.material_interface.get_path_name() if slot.material_interface else None
                 for slot in slots]
    _require(None not in materials and len(set(materials)) == len(materials),
             "Material slots must contain distinct materials for lossless SetLodFromStaticMesh")
    sockets = []
    # Sockets is a protected, non-editable property. The converter tags each
    # articulation socket, and this public API reads those without reflection.
    # SetLodFromStaticMesh does not modify any socket array or socket objects.
    for socket in mesh.get_sockets_by_tag("MassBattleSingleTurret"):
        sockets.append({"name": str(socket.get_editor_property("socket_name")),
                        "tag": socket.get_editor_property("tag"),
                        "location": socket.get_editor_property("relative_location").export_text(),
                        "rotation": socket.get_editor_property("relative_rotation").export_text(),
                        "scale": socket.get_editor_property("relative_scale").export_text()})
    mask = _LIB.validate_generated_articulated_mesh(mesh, False)
    return {
        "lod_count": _SUB.get_lod_count(mesh),
        "build": _SUB.get_lod_build_settings(mesh, 0).export_text(),
        "reduction": _SUB.get_lod_reduction_settings(mesh, 0).export_text(),
        # Auto-generated UV density may change when UV1 is restored. Preserve
        # the slot identities and any explicit density override instead.
        "slots": [{"material": materials[index],
                   "name": str(slot.material_slot_name),
                   "imported_name": str(slot.get_editor_property("ImportedMaterialSlotName")),
                   "overlay": slot.overlay_material_interface.get_path_name()
                   if slot.overlay_material_interface else None,
                   "density_override": slot.get_editor_property("UVChannelData").export_text()
                   if slot.get_editor_property("UVChannelData").get_editor_property("bOverrideDensities") else None}
                  for index, slot in enumerate(slots)],
        "sections": [_SUB.get_lod_material_slot(mesh, 0, section)
                     for section in range(mesh.get_num_sections(0))],
        "articulation_sockets": sockets,
        "bounds_positive": _vec(mesh.get_editor_property("positive_bounds_extension")),
        "bounds_negative": _vec(mesh.get_editor_property("negative_bounds_extension")),
        "lightmap_channel": mesh.get_editor_property("light_map_coordinate_index"),
        "nanite": _SUB.get_nanite_settings(mesh).export_text(),
        "mask_valid": bool(mask.valid), "mask_messages": list(mask.messages),
    }


def _assert_same_geometry(formal, donor):
    _require(formal["triangles"] == donor["triangles"], "Donor triangle topology/sections differ")
    for key in ("vertex_count", "edge_count", "polygon_count", "group_count"):
        _require(formal[key] == donor[key], "Donor " + key + " differs")
    _require(len(formal["vertices"]) == len(donor["vertices"]), "Donor vertex-instance count differs")
    for index, (left, right) in enumerate(zip(formal["vertices"], donor["vertices"])):
        _require(left[0] == right[0], f"Donor vertex-instance mapping differs at {index}")
        _require(max(abs(a - b) for a, b in zip(left[1], right[1])) <= 0.0001,
                 f"Donor position differs at {index}")
        _require(max(abs(a - b) for a, b in zip(left[2], right[2])) <= 0.000001,
                 f"Donor UV0 differs at {index}")


def _donor(unit, folder):
    source_id = unit["source_asset_id"]

    def part(role):
        name = source_id + "_" + role
        path = f"/Game/WW2Generic/TankLine/MechanicalParts/{source_id}/{role}/{name}/StaticMeshes/{name}"
        mesh = _load_mesh(path)
        _require(_SUB.get_num_uv_channels(mesh, 0) == 2, "Expected exactly two source UV channels: " + path)
        return mesh

    body = part("Body")
    has_turret = bool(unit["parts"]["Turret"]["triangles"])
    turret = part("Turret" if has_turret else "Barrel")
    barrel = part("Barrel") if has_turret and unit["parts"]["Barrel"]["triangles"] else None
    settings = unreal.MBSTActorToSingleTurretSettings()
    settings.agent_config_template = _require(
        unreal.load_asset("/MassBattleUnitGallery/Units/AgentConfig_GalleryInvisibleSandbag"),
        "Missing established converter template")
    settings.create_vat_data_asset = False
    settings.keep_intermediate_meshes = False
    settings.generate_lightmap_u_vs = False
    settings.bounds_extension = 100
    settings.create_pivot_sockets = True
    result = _LIB.convert_static_meshes_to_single_turret(
        body, turret, barrel, None, unreal.Vector(*unit["yaw_pivot_cm"]),
        unreal.Vector(*unit["pitch_pivot_cm"]),
        unreal.Vector(*[x * 100 for x in unit["muzzles"][0]["pivot"]]),
        0.0, 45.0, 30.0, folder, "SM_Donor_" + source_id, settings)
    _require(result.succeeded, "Donor conversion failed: " + str(list(result.messages)))
    _require(_SUB.get_num_uv_channels(result.articulated_mesh, 0) == 2,
             "Converter still discards UV1; load the fixed plugin binary first")
    return result


def _report(plan):
    return {"stage": plan["stage"], "temporary_root": plan["root"],
            "units": [{"unit": item["unit"], "mesh": item["mesh"].get_path_name(),
                       "action": item["action"], "uv0_sha256": _fingerprint(item["geometry"]),
                       "uv1_sha256": _fingerprint(item["uv1"]),
                       "vertex_instances": len(item["uv1"])} for item in plan["items"]]}


def prepare(unit_names=None):
    """Check all selected tanks and stage unsaved UV1-only replacements."""
    global _PLAN
    _require(not unreal.SourceControl.is_enabled(),
             "Source control must be disabled: AssetTools duplicate would otherwise save temporary assets")
    _require(not unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world(),
             "Stop PIE before preparing static meshes")
    _require(_PLAN is None, "A repair is already prepared in this Python module")
    units = json.loads((_ROOT / "Scripts/MassBattleUnits/generic_tank_line_units.json").read_text(encoding="utf-8"))["units"]
    if unit_names is not None:
        wanted = set(unit_names)
        units = [unit for unit in units if unit["unit_name"] in wanted]
        _require({unit["unit_name"] for unit in units} == wanted, "Unknown requested unit names")
    _require(units, "No units selected")
    dirty = {package.get_path_name() for package in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()}
    plan = {"stage": "preparing", "root": "/Game/Developers/MBSTSurfaceUVRepair/" + uuid.uuid4().hex,
            "items": []}
    for unit in units:
        mesh = _load_mesh(unit["package_path"] + "/SM_" + unit["unit_name"])
        _require(mesh.get_path_name().split(".")[0] not in dirty,
                 "Save or discard pre-existing mesh edits before repair: " + mesh.get_path_name())
        _require(_SUB.get_lod_count(mesh) == 1, "Only the known single-LOD tank assets are supported")
        channels = _SUB.get_num_uv_channels(mesh, 0)
        _require(channels in (1, 2), "Unexpected UV channel count")
        before = _description(mesh)
        settings = _settings(mesh)
        reduction = _SUB.get_lod_reduction_settings(mesh, 0)
        _require(reduction.base_lod_model == 0 and reduction.percent_triangles == 1.0
                 and reduction.percent_vertices == 1.0, "Active LOD reduction requires manual review")
        folder = plan["root"] + "/" + unit["source_asset_id"]
        donor_result = _donor(unit, folder)
        donor = donor_result.articulated_mesh
        _assert_same_geometry(before, _description(donor))
        uv1 = [entry[2] for entry in _description(donor, 1)["vertices"]]
        item = {"unit": unit["unit_name"], "mesh": mesh, "geometry": before,
                "settings": settings, "uv1": uv1, "donor_result": donor_result,
                "action": "already_correct" if channels == 2 else "restore_uv1"}
        if channels == 2:
            _require([entry[2] for entry in _description(mesh, 1)["vertices"]] == uv1,
                     "Existing UV1 differs from source; refusing to overwrite it")
        else:
            backup = _require(_TOOLS.duplicate_asset("SM_Original", folder, mesh), "Backup duplicate failed")
            working = _require(_TOOLS.duplicate_asset("SM_WithUV1", folder, mesh), "Working duplicate failed")
            _require(_description(backup) == before and _description(working) == before, "Duplicate geometry differs")
            working_settings = _settings(working)
            _require(working_settings == settings, "Duplicate settings differ: " + str({
                key: (settings[key], working_settings[key]) for key in settings
                if settings[key] != working_settings[key]}))
            _require(_SUB.add_uv_channel(working, 0), "Could not add working UV1")
            # This is the cached source description, not a detached copy.
            desc = working.get_static_mesh_description(0)
            for index, uv in enumerate(uv1):
                desc.set_vertex_instance_uv(unreal.VertexInstanceID(index), unreal.Vector2D(*uv), 1)
            _require(_description(working) == before, "UV1 editing changed original geometry/UV0")
            _require(_settings(working) == settings, "UV1 editing changed mesh settings/masks")
            _require([entry[2] for entry in _description(working, 1)["vertices"]] == uv1, "UV1 copy readback differs")
            item.update(backup=backup, working=working)
        plan["items"].append(item)
        unreal.log("MBST_UV_PREPARED " + unit["unit_name"] + " " + item["action"])
    plan["stage"] = "prepared"
    _PLAN = plan
    return _report(plan)


def _verify(item, expected_channels=2):
    mesh = item["mesh"]
    _require(_SUB.get_num_uv_channels(mesh, 0) == expected_channels, "UV channel count did not persist")
    _require(_description(mesh) == item["geometry"], "Formal geometry/UV0 changed")
    _require(_settings(mesh) == item["settings"], "Formal settings/materials/sockets/masks changed")
    if expected_channels == 2:
        _require([entry[2] for entry in _description(mesh, 1)["vertices"]] == item["uv1"], "Formal UV1 differs")


def apply_prepared():
    """Apply the fully checked plan in memory, rolling back on any failed check."""
    _require(_PLAN and _PLAN["stage"] == "prepared", "Call prepare() first")
    changed = []
    try:
        for item in _PLAN["items"]:
            if item["action"] == "already_correct":
                _verify(item)
                continue
            _verify(item, 1)
            changed.append(item)
            _require(_SUB.set_lod_from_static_mesh(item["mesh"], 0, item["working"], 0, True) == 0,
                     "SetLodFromStaticMesh failed")
            _verify(item)
    except Exception:
        for item in reversed(changed):
            _require(_SUB.set_lod_from_static_mesh(item["mesh"], 0, item["backup"], 0, True) == 0,
                     "Rollback failed; discard unsaved mesh edits")
            _verify(item, 1)
        raise
    _PLAN["stage"] = "applied_in_memory"
    return _report(_PLAN)


def save_prepared():
    """Recheck and save only the repaired formal meshes; never save temp assets."""
    _require(_PLAN and _PLAN["stage"] == "applied_in_memory", "Call apply_prepared() first")
    for item in _PLAN["items"]:
        _verify(item)
    for item in _PLAN["items"]:
        if item["action"] == "restore_uv1":
            _require(unreal.EditorAssetLibrary.save_loaded_asset(item["mesh"], False),
                     "Could not save " + item["mesh"].get_path_name())
    _PLAN["stage"] = "saved"
    report = _report(_PLAN)
    output = _ROOT / "Saved/BlackTurretInvestigation/generic_tank_uv_repair.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return report


if __name__ == "__main__":
    print(json.dumps(prepare(), ensure_ascii=False))
