# MassBattle Single Turret

这是一个独立的 MassBattle 单炮塔单位插件。它把任意可解析的 Blueprint Actor 装配转换成一个直接可生成的 Mass 单位：

```text
1 个新 AgentConfig
1 个 Mass Entity
1 个 Niagara Particle
1 个合并 StaticMesh
0 个运行时 Actor/AuthoringComponent
0 个 Host Entity
0 个炮塔子 Agent
```

## 最重要的边界

炮塔数据只在调用本插件的 `Convert Actor To Single Turret VAT` 时写入。

转换器每次都新建一个 AgentConfig，并自动把以下契约写进新资产：

```text
AgentConfig.ExtraData.Tags                  += FMBSTSingleTurretTag
AgentConfig.ExtraData.Fragments             += FMBSTSingleTurretState
AgentConfig.ExtraData.MutableSharedFragments += FMBSTSingleTurretShared(Layout)
```

`AgentConfigTemplate` 只是可选的普通单位模板，用来继承移动、战斗等配置；插件复制它，绝不原地修改。没有经过本插件 Actor→单位入口的普通 AgentConfig 不会得到炮塔 Tag/Fragment，也不会进入炮塔 Processor。

因此，用新 AgentConfig 走 MassBattle 原有的按 Config 生成流程时，生成出来的就是带炮塔功能的 Mass 实体，不需要生成后再挂 Actor Component，也不需要再迁移 Archetype。

## Actor 如何标记炮塔

在源 Actor Blueprint 上添加编辑期组件：

```text
MBST Single Turret Authoring
```

然后绑定任意现有 SceneComponent：

- `TurretYawPivot`：炮塔水平旋转节点，必需；
- `BarrelPitchPivot`：炮管俯仰节点，可选；
- `Muzzle`：炮口节点，可选；
- `VATDriver`：车体 VAT 的 SkeletalMeshComponent，可选。

也可以使用组件 Tag 作为后备标记：

```text
MBST_TurretYaw
MBST_BarrelPitch
MBST_Muzzle
MBST_VATDriver
MBST_Ignore
```

AuthoringComponent 只负责告诉转换器“哪里是炮塔”。它不会被带入 Mass 运行时；运行时能力来自新 AgentConfig 中的 Tag + Fragment + Shared Fragment。

## 转换输出

一次成功转换必定返回：

```text
ArticulatedMesh   合并网格，顶点色保存 Body/Turret/Barrel 掩码
LayoutAsset       Pivot、轴、角度限制、速度、Muzzle 等共享布局
AgentConfig       自动含炮塔运行时契约的新单位配置
VATDataAsset      仅在启用 VAT 时生成
```

如果 AgentConfig 创建或注入失败，整个转换结果会失败，不会把“只有 Mesh、没有炮塔实体契约”的半成品报告为成功。

## 已生成的坦克 Demo

插件已经用 MassBattleFrame Demo 的 `BP_TankActor` 生成并接好完整资产链：

```text
/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret
/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_SingleTurret
/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig
/MassBattleSingleTurret/Demo/Tank/Renderer_Tank_SingleTurret
/MassBattleSingleTurret/Demo/Tank/NS_Tank_SingleTurret
/MassBattleSingleTurret/Materials/M_MBST_VATSingleTurret
```

新 AgentConfig 已指向插件 Renderer；Renderer 已指向合并 Mesh 和插件 Niagara。源 `BP_TankActor` 与 MassBattleFrame 源码/资产均未修改。

## 性能结论的准确说法

这不是“炮塔计算完全零开销”。准确说法是：

- 普通单位零炮塔开销：查询要求 `FMBSTSingleTurretTag`；
- 炮塔单位没有 Actor、Host、父子实体和额外完整 Agent 的开销；
- 每个炮塔实体仍有一个紧凑 State Fragment、一次固定长度 CPU 更新/打包，以及材质顶点关节计算；
- 复用现有 `FStyleType.Index → User.StyleArray`，每实体额外渲染载荷只有一个已存在通道中的 32 位值。

Demo 旧坦克有 4 个 `MassBattleAgentComponent`（Vehicle/Turret/MachineGun/Cannon），新坦克是 1 个 Mass 实体，结构上减少 75% 的实体数量；这不是 FPS 声明。可重复的微基准与正式场景对照方法见 [性能设计](Docs/04_Performance_ZH.md)。

插件还包含两张互不混跑的 5000 对 5000 基准地图：

```text
/MassBattleSingleTurret/Demo/Benchmark/Map_MBST_Legacy_5000v5000
/MassBattleSingleTurret/Demo/Benchmark/Map_MBST_SingleTurret_5000v5000
```

两边固定同一队形、相机、分辨率、20 秒墙钟采样和每帧炮塔更新；炮塔按 `±75° / 8 s` 强制往复。为避免死亡减员和武器/寻路掩盖结构差异，计时窗口关闭攻击、Trace、移动和调试，表现保持开启；视觉齐射只出现在预热截图中。当前机器的正式结果为插件 `P50 167.418 ms / P95 174.929 ms`，旧 Demo `P50 1250.564 ms / P95 1576.489 ms`。完整条件、命令和限制见性能文档。

## 验证

静态验证：

```powershell
python Tools/validate_plugin.py
python Tools/check_massbattle_contract.py D:\UE5Project\Winyunq\Plugins\MassBattleFrame
```

编辑器自动化：

```text
MassBattle.SingleTurret.Authoring.GenerateDemoTank
MassBattle.SingleTurret.Authoring.ConfigureDemoNiagaraStyleArray
MassBattle.SingleTurret.Performance.DemoTankStructure
MassBattle.SingleTurret.Performance.PackingMicrobenchmark
MassBattle.SingleTurret.Benchmark.CreateDemoMaps
```

## 当前范围

支持一个 Yaw 炮塔、可选一个 Pitch 炮管和可选 Recoil。暂不支持第二个独立炮塔、运行时动态关节数组、GPU 碰撞或多炮塔增减。

## 文档

- [Actor 与单位转换](Docs/01_Actor_Conversion_ZH.md)
- [Niagara 与材质接线](Docs/02_Niagara_Material_ZH.md)
- [运行时流程](Docs/03_Runtime_ZH.md)
- [性能设计与对照方法](Docs/04_Performance_ZH.md)
- [排错清单](Docs/05_Troubleshooting_ZH.md)
