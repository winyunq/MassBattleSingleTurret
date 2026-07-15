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

## 为什么拆成两个 16 位 float

材质 Dynamic Parameter 是 float，不能保证一个 float 精确保存所有 32 位整数。Demo Niagara 在 Spawn 和 Update 都执行：

```hlsl
Particles.DynamicMaterialParameter1 = float4(
    float(Particles.StyleType & 65535),
    float((Particles.StyleType >> 16) & 65535),
    0,
    0);
```

两个 0～65535 的整数都可被 float 精确表示。材质中再恢复：

```hlsl
uint Packed = (uint)round(PackedLowHigh.x)
    | ((uint)round(PackedLowHigh.y) << 16u);
int PackedState = asint(Packed);
```

不要直接把整个有符号 int32 转成单个 float，否则高位 Pitch/Recoil 可能丢失。

## 材质关节

插件材质复制自 MassBattle VAT Master，只在插件副本中增加：

```text
恢复 PackedState
→ MBST_DecodePackedStateSinCos
→ Recoil
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

顶点掩码来自生成 Mesh 的 VertexColor。当前 Demo 在材质顶点阶段恢复并解包状态，优先保证 32 位精确性和接线简单；若大型场景 Profile 证明三角函数成为瓶颈，可在 Niagara 粒子阶段预计算 sin/cos，再通过额外 Dynamic Parameter 传入。

## 法线限制

当前 Beta 材质已经旋转几何位置，但尚未把同一关节变换完整接入 VAT Master 的法线/切线链。大角度时可能出现高光方向不完全跟随。`MBSTSingleTurret.ush` 已提供 `MBST_ArticulateNormalObjectSpace`，后续应按项目实际切线空间接入。

一个合并 Mesh 也不自动等于一个 Draw Call；Section 数仍由材质槽决定，远 LOD 应尽量 Atlas 化。
