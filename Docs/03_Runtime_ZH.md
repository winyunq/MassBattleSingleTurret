# 运行时流程

## 直接按新 AgentConfig 生成

标准路径是：

```text
Actor 编辑期标记
→ 本插件 Convert Actor To Single Turret VAT
→ 新建并自动注入的 AgentConfig
→ MassBattle 原有 Spawn by Config
→ 直接得到带炮塔 Tag/Fragment 的单个 Mass Entity
```

不需要在运行时保留 Actor/AuthoringComponent，也不需要生成后再调用 `Add Single Turret To Existing Agent`。后者仅保留给调试或少量动态改装场景，因为它会产生 Archetype 迁移。

`Make Single Turret Template From Agent Config` 仍可用于程序化模板工作流，但对本插件转换器生成的新 Config 不是必需步骤；Config 自己已经带完整契约。

## Entity 数据

每个炮塔单位：

```text
FMBSTSingleTurretTag
FMBSTSingleTurretState
FStyleType（MassBattle 原有）
```

每个共享布局值：

```text
FMBSTSingleTurretShared
→ LayoutAsset
→ Pivot / Axis / Limits / Speed / Recoil
```

普通单位没有 `FMBSTSingleTurretTag`，不会匹配 Processor 查询。

## Processor

`UMBSTSingleTurretPackProcessor` 在 `FrameEnd`、`MassBattleAgentRenderProcessor` 之前运行：

```text
按共享限制 Clamp
→ 可选 FixedTurn/FInterpConstantTo
→ Recoil 衰减
→ 量化成 32 位 PackedState
→ 写入原 FStyleType.Index
→ 原 Renderer 上传 User.StyleArray
```

它不遍历 Actor 层级、不查 Host、不更新炮塔子 Entity，也不修改 MassBattleFrame Processor。

## 控制 API

```text
Set Turret Target Angles
Aim Turret At World Location
Trigger Turret Recoil
Get Single Turret Pose
```

`Get Single Turret Pose` 在 CPU 使用相同布局和角度重建 TurretPivot、BarrelPivot、Muzzle 世界变换，可直接供 Projectile、Trace、Muzzle FX、音效和服务器判定使用，无需 GPU 回读。

## “零开销”的准确边界

普通单位对本插件是零查询实体：没有 Tag 就不进入热路径。炮塔单位不是零计算；它仍需状态更新、32 位打包、Niagara 上传和顶点 WPO。优化点是消除了 Actor/Component、Host、子 Agent、父子 Transform 传播和第二套完整表现数据。
