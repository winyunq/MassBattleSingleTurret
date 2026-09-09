# Niagara 与材质接线

## 已交付的 Demo 链路

插件包含可直接检查的资产：

```text
NS_Tank_SingleTurret
Renderer_Tank_SingleTurret
M_MBST_VATSingleTurret
MI_Tank_Vehicle_LOD0/1/2_MBST
MI_Tank_Turret_LOD0/1/2_MBST
```

Renderer 使用生成的 `Tank_SingleTurret` 网格和 `NS_Tank_SingleTurret`。新 AgentConfig 已引用该 Renderer Class。

## StyleArray 数据通道

插件不修改 MassBattleFrame Render Processor，而是复用原通道：

```text
FMBSTSingleTurretPackProcessor
→ FStyleType.Index
→ MassBattleAgentRenderProcessor
→ User.StyleArray : Niagara Array Int32
```

`Ensure Niagara Style Array` 会在 Niagara System 上建立独立的 `UNiagaraDataInterfaceArrayInt32`，验证器同时检查参数名、精确类型和独立 Data Interface 实例。

32 位布局：

| Bits | 数据 |
|---|---|
| 0–7 | VisualStyle |
| 8–19 | Yaw，-180°～180° |
| 20–27 | Pitch，-90°～90° |
| 28–31 | Recoil，0～1 |

## 为什么在 Niagara 粒子阶段解包

CPU/Mass 侧仍只保留一个精确的 `int32`；相对不暴露 `StyleArray` 的普通 Renderer，每个逻辑槽只新增 4 B。Demo Niagara 在 GPU Spawn 和 Update 中各对每个粒子解包一次，并写入：

```hlsl
Particles.DynamicMaterialParameter1 = float4(
    sin(Yaw),
    cos(Yaw),
    sin(Pitch),
    RecoilNormalized);
```

Pitch 的 wire 范围固定为 `[-90°, 90°]`，所以材质可无歧义恢复非负的 `cos(Pitch)`：

```hlsl
float CosPitch = sqrt(saturate(1 - SinPitch * SinPitch));
```

不要把整个有符号 int32 转成单个 float。也不要把 packed halves、位移、反量化或 `sin/cos` 放回材质 Custom 节点；那会让工作随顶点数和渲染 Pass 重复。

## 材质关节

插件材质复制自 MassBattle VAT Master，只在插件副本中增加：

```text
读取 DynamicMaterialParameter1
→ MBST_ExpandCompactArticulation
→ 围绕 BarrelPivot 执行 Pitch
→ 围绕 TurretPivot 执行 Yaw
→ Local Delta 转 World
→ 加到原 VAT World Position Offset
```

静态参数来自 Layout：

```text
MBST_TurretPivot
MBST_TurretAxis
MBST_BarrelPivot
MBST_BarrelAxis
MBST_BarrelForward
MBST_MaxRecoilDistance
```

顶点掩码来自生成 Mesh 的 VertexColor。当前 Demo 已在 Niagara GPU 粒子阶段预计算三角函数；材质 Position/Normal 路径只做关节旋转。Dynamic Parameter 会增加一个 16 B GPU 粒子参数向量，但 CPU→Niagara 的机械状态增量仍是 4 B/槽。

## 军团级材质、LOD 与运动矢量策略

默认量产 permutation 使用 `MBST_HighQualityArticulatedNormals=false`，直接采用插值后的 `VertexNormalWS`，并启用 `Fully Rough`：保留炮塔/炮管位置 WPO，但完全裁掉法线贴图采样、切线变换和像素阶段双 Rodrigues 旋转。近景英雄单位可把静态开关设为 `true`，恢复完整法线贴图与关节法线；必须作为独立近景材质预算。

炮塔参数路径不强制或重写资产 LOD。Demo 网格保持作者设置的 `MinLOD=0`，MassBattle `AgentRenderer_VAT` 的 `User.AgentMesh` slot 继续按原 ABI 使用 LOD `0/1/2/3/4`。LOD、Section 和几何复杂度属于单位资产/Scalability 策略，不能作为炮塔传参优化成立的前提。

性能 Niagara Mesh Renderer 默认关闭动态阴影。大量机械单位应使用低成本 blob/contact-shadow FX，或仅让近景质量变体投射动态阴影，避免 WPO 在每个 Shadow View 重放。

所有 Niagara Renderer 都禁用 Motion Vector；Renderer 所有者把批 Niagara Component 设为 `Stationary`，并在机械 Renderer 存活期间关闭 vertex-deformation velocity。组件本身不移动，粒子位置仍正常变化。代价是该军团路径不提供关节 WPO 的运动模糊/TSR 速度；近景高质量变体应使用另一套 Renderer，而不是重新打开 10K 批次的完整 Velocity Pass。

一个合并 Mesh 也不自动等于一个 Draw Call；Section 数仍由材质槽决定，远 LOD 应尽量 Atlas 化。
