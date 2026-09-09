# MassBattle Single Turret Plugin

`MassBattleSingleTurret` is an independent plugin for Unreal Engine 5.8 and the MassBattle framework. It converts an arbitrary component-based Actor/vehicle into a **Single Entity + Single Niagara Particle + Single Merged VAT StaticMesh** turret unit without runtime Actor components or sub-agent child entities.

👉 **[中文文档 / Read in Chinese (README_ZH.md)](README_ZH.md)**

---

## 🌟 Key Advantages

- **Zero Actor & Zero Sub-agent Overhead**: Replaces 3-entity compositions (Body + Turret + Barrel) or Actor bindings with **1 single Mass Entity**.
- **Vertex Color Joint Masking & 32-bit State Packing**: Turret yaw, barrel pitch, and recoil use one 32-bit Mass/Niagara payload. Niagara decodes it once per GPU particle and sends compact sin/cos/recoil parameters to the material; vertex code only performs rigid articulation.
- **Zero-Overhead Query Filtering**: Extends Mass queries via `FMBSTSingleTurretTag`. Ordinary non-turret units completely bypass the turret processing pipeline.
- **Mechanical-unit fast path**: Tanks, artillery, anti-air and other one-root/few-joint units should prefer this single-entity army route over Actor or child-Agent compositions. With the asset-authored `MinLOD=0` and LOD `0..4` slot ABI preserved, the final 10,000-unit DX12 benchmark sustained a **71.26 FPS median** across three independent runs; the GPU delta versus ordinary one-entity Mass was **-0.022 ms (within measurement noise)** on the test machine.

---

## 🎯 Focus: How to Mark Turrets (Authoring Phase)

Source Actors do **NOT** need to derive from a specific C++ class. You only need to identify the **Turret Yaw Pivot, Barrel Pitch Pivot, and Muzzle Location** during authoring.

> ⚠️ **Note**: Marking is an authoring/conversion step only. Authoring components/tags do **NOT** persist on runtime Mass Entities.

### Method 1: Add `UMBSTSingleTurretAuthoringComponent` (Recommended)

Add `UMBSTSingleTurretAuthoringComponent` to your source Actor Blueprint, and bind the scene components in the Details panel:

| Property | Type | Description |
| :--- | :--- | :--- |
| **TurretYawPivot** | `USceneComponent*` | **Required**. SceneComponent specifying the turret horizontal Yaw axis (0° ~ 360°). |
| **BarrelPitchPivot** | `USceneComponent*` | **Optional**. SceneComponent specifying the barrel vertical Pitch axis (-90° ~ 90°). If omitted, unit behaves as a Yaw-only turret. |
| **Muzzle** | `USceneComponent*` | **Optional**. SceneComponent / Socket for muzzle origin, trajectory calculations, and fire VFX. |
| **VATDriver** | `USkeletalMeshComponent*` | **Optional**. Skeletal mesh driving body VAT animation (e.g. tracks/suspension). |

---

### Method 2: Component Tag Fallback

If you prefer not to reference components directly in Blueprint code, add the following tags to the **Component Tags** array of your `USceneComponent`s:

- `MBST_TurretYawPivot`: Marks component as the Turret Yaw Pivot.
- `MBST_BarrelPitchPivot`: Marks component as the Barrel Pitch Pivot.
- `MBST_Muzzle`: Marks component as the Muzzle location.
- `MBST_VATDriver`: Marks component as the VAT Driver.

> **Note**: Mesh components under the specified Pivot subtree are automatically categorized into Body, Turret, or Barrel sub-meshes.

---

### Vertex Color Masking Convention

During mesh merging, vertex joint masks are automatically written into the merged mesh's **Vertex Color**:

| Sub-mesh | R (Yaw) | G (Pitch) | B (Body VAT) | A (Recoil) |
| :--- | :---: | :---: | :---: | :---: |
| **Body** | `0` | `0` | `0` or `1` | `0` |
| **Turret** | `1` | `0` | `0` | `0` |
| **Barrel** | `1` | `1` | `0` | `1` |

---

## 🚀 Step-by-Step Usage Guide

### Step 1: Mark the Source Actor
Annotate your Actor using `UMBSTSingleTurretAuthoringComponent` or `Component Tags` as described above.

### Step 2: Convert Actor to Single Turret VAT
Call the conversion function in Blueprint or C++:
`Convert Actor To Single Turret VAT`

**Conversion Pipeline**:
```text
Read SourceActor Visible Meshes
 └─► Categorize Body / Turret / Barrel by Authoring Pivots
      └─► Export & Merge Meshes with Vertex Color Joint Masks
           └─► Create LayoutAsset & Pivot Sockets
                └─► Duplicate AgentConfig Template & Inject SingleTurret Contract
                     └─► Wire Renderer Class & Niagara Style Array Data Interface
```

**Generated Contract in `AgentConfig`**:
- `ExtraData.Tags`: `FMBSTSingleTurretTag`
- `ExtraData.Fragments`: `FMBSTSingleTurretState`
- `ExtraData.MutableSharedFragments`: `FMBSTSingleTurretShared`

### Step 3: Spawn Mass Entities at Runtime
Use MassBattle's standard **`Spawn by Config`** node, passing the generated `AgentConfig`. Units spawn as single Mass Entities with full turret capabilities and zero Actor overhead.

### Step 4: Runtime Aiming & Control APIs
Control turrets programmatically using C++ or Blueprint:

```cpp
// 1. Aim at World Location
Aim Turret At World Location(TargetLocation);

// 2. Set Target Angles Directly
Set Turret Target Angles(TargetYaw, TargetPitch);

// 3. Trigger Recoil Animation
Trigger Turret Recoil();

// 4. Reconstruct CPU World Transforms (for projectiles, traces, and VFX)
Get Single Turret Pose(OutTurretTransform, OutBarrelTransform, OutMuzzleTransform);
```

### Step 5: Mobile Fire & Attack-Move Profiles (`UMBSTMobileFireProfile`)
Configure unit combat behavior via `UMBSTMobileFireProfile`:

- **`StopTurnChassisAndFire`**: Stop and align chassis to fire (casemate tanks/artillery).
- **`AimWhileMovingStopToFire`**: Track target while moving, apply temporary movement hold to fire, then resume move.
- **`AimAndFireWhileMoving`**: Continuous aiming and firing on the move without slowing down (stabilized modern tanks/IFVs).

---

## 🛠️ Demo Maps & Verification

Explore the demo content in `/MassBattleSingleTurret/Demo/`:

- **Tank Demo**: `/MassBattleSingleTurret/Demo/Tank`
- **Mobile Fire Demo**: `/MassBattleSingleTurret/Demo/MobileFire/Map_MBST_MobileFire`
- **RTS Battle Demo**: `/MassBattleSingleTurret/Demo/RTS/Map_MBST_RTS_MobileFire` — copied from the MassBattleFrame RTS scene and configured for 5,000 player plus 5,000 enemy single-entity GPU-turret tanks.
- **Performance Benchmark**: `/MassBattleSingleTurret/Demo/Benchmark/Map_MBST_NativeTrackingBenchmark`

Run functional and benchmark scripts via PowerShell:
```powershell
Tools/run_mobile_fire_functional.ps1 -Units 64
Tools/run_native_tracking_benchmark.ps1 -Units 500
```

---

## 📄 Documentation Links

- [01 Actor Conversion (中文)](Docs/01_Actor_Conversion_ZH.md)
- [02 Niagara & Materials (中文)](Docs/02_Niagara_Material_ZH.md)
- [03 Runtime Architecture (中文)](Docs/03_Runtime_ZH.md)
- [04 Performance & Benchmarks (中文)](Docs/04_Performance_ZH.md)
- [06 Mobile Fire & Attack-Move (中文)](Docs/06_MobileFire_ZH.md)

---

## 📜 License & Contribution

Released under the [MIT License](LICENSE). Issues and pull requests are welcome!
