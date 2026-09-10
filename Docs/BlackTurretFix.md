# Black turret material fix (0.2.2)

UE 5.8 can compile a Masked material with an effective constant opacity mask of
1 as `bUsesOpacityMask=false`. Its depth pixel shader is then omitted, but the
runtime Masked depth pass still requests that shader. The depth pass falls back
to the default material, which does not apply the turret's World Position Offset.
The color pass continues to rotate the turret. The mismatched depth and color
surfaces produce black silhouettes as the turret moves away from its rest pose.

The plugin wraps the **final** opacity mask with a Custom `return Mask;`
expression. This retains the required shader permutation while preserving the
original mask value, clipping threshold, blend mode, static switches, and other
material outputs. Material Attributes are handled after their final output.
The root input's `UseConstant` flag is cleared only after preserving its effective
value. Repeated configuration does not add duplicate wrappers.

`ConfigureArticulationBaseMaterial` and its precomputed-articulation caller apply
this migration. Run the migration on project-owned copies as well as the plugin
master, with PIE stopped:

```python
import runpy
from pathlib import Path
import unreal

tool = Path(unreal.Paths.project_plugins_dir()) / "MassBattleSingleTurret/Tools/upgrade_material_decode.py"
migration = runpy.run_path(str(tool), run_name="mbst_upgrade")
upgrade = migration["upgrade_materials"]
upgrade([
    "/MassBattleSingleTurret/Materials/M_MBST_VATSingleTurret",
    "/Game/WW2Generic/TankLine/Shared/M_TankLine_ArticulatedSurface",
])
```

The tool compiles and checks each material before saving it. Reload existing
editor/game sessions after saving, so they use the upgraded assets. When
splitting Material Attributes into individual pins, retain OpacityMask and clear
stale inline-constant flags before calling the configurator again.

Two additional authoring compatibility repairs are included:

- The copied DP0.w decoder now calls `/MassBattle/MassBattle_MaterialDecode.ush`
  instead of hardcoding the old team/dissolve/LOD/selection bit layout.
- Vertex-mask painting uses `FStaticMeshAttributes::Register(true)` to retain
  imported UV channels. Existing meshes with a discarded UV1 need source-based
  restoration; the converter change alone cannot reconstruct missing data.

Regression tests inspect compiled mask usage before and after migration, cover
inline constants and Material Attributes/static switches, and check conversion
preserves source UVs. Runtime verification must also use actual game materials
and original textures with rotating turrets; a pure-color diagnostic or a
successful compile by itself is insufficient.

## Verified on 2026-09-10

- The editor module and its required build actions completed successfully.
- `FrameworkDecodeCompatibility`, `MaskedDepthInputs`,
  `MaskedDepthAttributesAndSwitches`, and `PreservesSurfaceUVChannels` passed.
- Both production masters were saved, then read back in a new editor process.
  All 14 generic tank meshes retained their original material references and
  restored UV1. The recovery tool checked every source vertex/UV0 and triangle
  mapping before saving only UV1 changes to the existing meshes.
- The existing `/Game/Map/EastAsia/64` map ran its normal synchronized level
  initialization with 14 unsaved extra UnitHere placements. Its two existing
  local tanks brought the sample to 16 entities across all 14 tank types.
  No AgentConfig or map changes were saved for this test.
- With original textures and project defaults (`r.VelocityOutputPass=0`,
  `r.EarlyZPass=3`, `r.EarlyZPassOnlyMaterialMasking=1`), turrets rotated for
  305 seconds. Garbage collection ran at 90 and 210 seconds. Captures at 10,
  60, 120 and 300 seconds showed no black turret silhouettes. The final readback
  found all 16 turret states and all 14 renderers active, with no failed angle
  updates and no diagnostic material references.

This is a bounded rendering regression check, not an assertion that every
possible long-running gameplay condition has been covered.
