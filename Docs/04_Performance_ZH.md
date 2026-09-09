# 性能设计与对照方法

## 已验证的结构差异

历史 MassBattleFrame Demo 的 `BP_TankActor` 原设计含 4 个 `MassBattleAgentComponent`：

```text
Vehicle
Turret
MachineGun
Cannon
```

插件生成的 `Tank_SingleTurret_AgentConfig` 产生 1 个直接带炮塔数据的 Mass 实体。因此实体结构由 4 降到 1，减少 75%。旧 Demo 的 Vehicle 与 Turret 至少是两个主要表现部件；新资产合并成一个 Mesh/Particle，所以主要表现实例至少由 2 降到 1。

这两项是资产/实体结构事实，不是 FPS 或 GPU 时间结论。

| 项目 | 旧 Demo 复合坦克 | 插件坦克 |
|---|---:|---:|
| Mass Agent 组件/目标实体 | 4 | 1 |
| 炮塔 Host/子 Agent | 需要复合关系 | 0 |
| 主要车体+炮塔表现实例 | 至少 2 | 1 |
| 父子 Entity Transform 传播 | 有 | 无 |
| 炮塔状态渲染载荷 | 子单位完整数据 | 复用 1 个 int32 |

自动化测试：

```text
MassBattle.SingleTurret.Performance.DemoTankStructure
```

它验证插件 AgentConfig 中恰好一份 Tag/State/Shared 契约、3 个网格 LOD，以及 `User.AgentMesh` 五个 slot 保持 LOD `0..4` ABI；测试只报告作者当前的 MinLOD，不规定其数值。MassBattleFrame 1.19.7 的旧 `BP_TankActor` 当前引用已删除的 `_Trash` AgentConfig，插件测试不再加载这个外部损坏资产；上面的 4 组件数据只作为历史结构说明。

## CPU 热路径

只查询 `FMBSTSingleTurretTag`，并在 `PostCombat` 逻辑子帧每完整逻辑帧运行一次；每个匹配实体执行固定长度逻辑：

```text
Clamp
FixedTurn/FInterpConstantTo
Recoil decay
Quantize/Pack
FStyleType write
```

没有动态炮塔数组、Offset/Count、Host 生命周期、父实体随机读取、子实体随机写入或 Attachment 图遍历。

打包微基准：

```text
MassBattle.SingleTurret.Performance.PackingMicrobenchmark
```

测试包含一次预热和 5 个样本，每个样本 4,194,304 次打包，报告中位 ns/entity、推算的一百万次打包毫秒数，以及 `sizeof(State/Shared)`。它只隔离 `MBSTPacking::Pack` 算术，不包括 Mass Chunk 迭代、角度插值、Niagara 上传、GPU 或整帧开销。

2026-07-15 在当前 Development Editor 构建上的一次结果：

```text
打包中位数                   18.091 ns/entity
推算 1,000,000 次打包        18.091 ms
FMBSTSingleTurretState        24 B/entity
Packed render payload         4 B/entity（复用 FStyleType）
FMBSTSingleTurretShared       40 B/shared layout value
```

这是机器与构建相关的微基准样本，应在目标硬件上重跑，不能直接换算成可承载单位数。

## GPU 与渲染

新方案减少表现实例和实体关系，但合并 Mesh 的顶点/Section 并不会消失。当前材质由 Niagara GPU 每粒子解包并预计算三角函数，顶点阶段只执行最多两次刚性旋转；默认军团 permutation 使用 `VertexNormalWS + Fully Rough`，关闭动态阴影和关节运动矢量，但不改变资产 LOD 选择。实际瓶颈仍可能来自：

1. LOD 顶点数与屏幕覆盖；
2. Material Section；
3. 动态阴影；
4. Niagara `StyleArray` 全量提交；
5. MassBattleFrame 普通 Agent 数组的全量复制与多 Data Interface 上传；
6. 旧方案是否对炮塔/武器使用独立剔除。

## 原生移动目标三方案对照

当前基准使用同一张中立地图，由命令行分别启动三个独立 UE 进程：

```text
/MassBattleSingleTurret/Demo/Benchmark/Map_MBST_NativeTrackingBenchmark
```

### 编辑器自由观察

直接在编辑器中打开该地图并点击 Play，PIE 会自动启用 `Free Observation`：PlayerController 保持控制 `SpectatorPawn`，不会被 Wide/Close 截图相机接管，也不会在采样完成后自动退出。点击视口后使用鼠标转向、`WASD` 飞行。关卡中的 `MBSTSingleTurretBenchmarkActor` 可调整 `Unit Count` 和 `Free Observation Speed`；命令行等价开关为 `-MBSTFreeObserve -MBSTFreeObserveSpeed=6000`。此模式用于人工观察，不应拿它的计时结果与固定相机正式样本混用。

场景内有一个队伍 2 的 Mass 目标实体，生命值 `1,000,000,000` 且锁定；一个原生 `StaticMeshActor` 只作为它的可见沙袋代理。目标以固定半径和周期环绕队形，因此所有测试单位必须持续更新朝向。单位更新由 `UMBSTSingleTurretBenchmarkDriveProcessor` 完成，没有 Blueprint Tick，也没有 Python 计时。

三个方案为：

| Token | 实际结构 | 瞄准方式 |
|---|---|---|
| `actor` | 原 `/MassBattle/Test/CompoundUnitAsset/BP_TankActor`；每车 1 Actor + 4 Mass 实体 | 旋转原 Turret 子实体 |
| `mass` | 1 个普通 Mass 实体；移除炮塔 Tag/State/Shared | 整个根单位旋转 |
| `turret` | 插件生成的 1 个炮塔 Mass 实体 | 根 Transform 固定，只更新炮塔 State 并在 GPU 关节旋转 |

`mass` 与 `turret` 故意使用同一个 Renderer、Niagara、合并网格和材质，用于隔离 CPU 状态更新差异；因此 `mass` 不是“静态编译掉关节材质”的刚体 GPU 基线。要测纯关节渲染增量，必须另建同网格、同 Section/LOD/阴影但不暴露 `StyleArray`、不执行关节 WPO 的 Rigid 资产。

正式计时直接读取 UE 原生计数器：

```text
GGameThreadTime
GRenderThreadTime
RHIGetGPUFrameCycles()
FPlatformTime 墙钟帧间隔
```

攻击、Trace、移动、弹丸和 FX 在三边同样关闭，以隔离“结构 + 持续瞄准 + 表现”开销；目标 Actor、HUD、地图、相机、分辨率和 RHI 完全相同。调试线和截图只在预热/结果阶段启用，不进入正式样本。

### 当前机器 500 辆结果

2026-07-16，Development Editor、DX12、1280×720、VSync 关闭、10 秒预热、20 秒采样；每个方案运行 3 个独立进程，下表取三次结果的中位数：

| 方案 | Actor | Mass 实体 | 平均帧时 | FPS | 帧 P95 | Game Thread | Render Thread | GPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 原 BP_TankActor | 500 | 2,000 | 22.931 ms | 43.61 | 26.586 ms | 22.903 ms | 4.533 ms | 7.242 ms |
| 普通单实体 Mass（整车转向） | 0 | 500 | 7.947 ms | 125.84 | 9.631 ms | 7.933 ms | 3.821 ms | 3.588 ms |
| 单实体炮塔 Mass | 0 | 500 | 6.950 ms | 143.90 | 8.504 ms | 6.936 ms | 4.016 ms | 3.556 ms |

在这个“目标持续运动”的工作负载中，炮塔 Mass 相对原 Actor 方案平均帧时低 `69.7%`，FPS 为 `3.30x`；相对整车每帧转向的普通 Mass，平均帧时低 `12.6%`。后一个差异主要出现在 Game Thread：炮塔方案保持根 Transform 不变，只更新紧凑状态；它的 Render Thread 中位平均值则比普通 Mass 高约 `5.1%`，GPU 两者接近。不能把这些倍率解释成单个 Fragment 本身的速度，也不能外推到含武器、寻路或不同模型的项目场景。

原始 JSON 与三次中位数汇总位于：

```text
Saved/MassBattleSingleTurret/NativeTrackingBenchmark500
```

### MassBattleFrame 1.19.7 / 10,000 单位最终结果

2026-08-06，Development Editor、DX12、1280×720、VSync 关闭、15 Hz 权威逻辑 + 四子帧 spreading、10,000 槽单批；普通 Mass 与炮塔各运行 3 个独立 UE 进程，每次预热 8 秒、采样 15 秒，下表取中位数。此轮明确保留资产作者的 `MinLOD=0`，`User.AgentMesh` 五个槽位按原 ABI 使用 LOD `0/1/2/3/4`，炮塔路径没有强制或改写 LOD：

| 方案 | 实体 | Actor | 平均帧时 | FPS | Game Thread | Render Thread | GPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| 普通单实体 Mass（整车转向） | 10,000 | 0 | 14.453 ms | 69.19 | 8.633 ms | 14.445 ms | 12.863 ms |
| 单实体 GPU 炮塔 | 10,000 | 0 | 14.032 ms | 71.26 | 7.407 ms | 14.022 ms | 12.841 ms |

炮塔三次分别为 `71.26 / 70.92 / 71.49 FPS`，不是最好一次过线。普通 Mass 三次为 `53.05 / 69.19 / 69.83 FPS`，首轮存在离群抖动，因此汇总按预先约定取三次独立进程的中位数。炮塔相对普通 Mass 的中位 GPU 差值为 `-0.022 ms`（约 `-0.17%`），小于测量噪声，不能识别出炮塔参数链的额外 GPU 成本；Render Thread 同样没有可辨认的新增成本。Game Thread 更低来自普通方案持续改根 Transform，而炮塔方案只改紧凑状态，不能解释成所有工作负载下都必然更快。

此前 GPU Visualizer 找到的公共瓶颈不是 Niagara compute，而是完整 WPO 网格在 `BasePass` 与独立 `RenderVelocities(Opaque)` 中重复提交。当前军团路径通过禁用该批次的 Motion Vector/vertex-deformation velocity 消除重复重绘；本轮原生 LOD 三轮中位 GPU 为 `12.841 ms`。实例契约仍是一个 Renderer、一个 Batch、10,000 instances。

最终军团路径的必要条件：一次 CPU pack、10K 单批、Niagara 每粒子解包/预计算、全 Renderer 禁用 Motion Vector、批组件 `Stationary`、关闭 vertex-deformation velocity、无动态阴影，以及默认 `VertexNormalWS + Fully Rough`。它不强制 LOD；画质交换仅包括万人批次不提供关节运动模糊、动态阴影或近景法线贴图。

结论：炮塔相对普通 Agent Renderer 的传输增量仍只有 `10,000 × 4 B = 40 KB/逻辑帧`，实测未发现炮塔参数链的额外 GPU 成本。坦克、火炮、防空炮等“一个根刚体 + 少量刚性关节”的机械单位默认走本插件单实体军团路径；LOD 完全服从单位资产和 Scalability 策略，近景英雄单位另用高质量 Renderer，不要让英雄材质、阴影和 Velocity 设置污染 10K 批次。

原始 JSON 与中位数汇总位于：

```text
Saved/MassBattleSingleTurret/Final_10K_AuthoredLOD_3Run
```

### 自行调整数量并复测

插件提供运行脚本。默认只跑真正相关的 `mass/turret`、交替顺序、3 次独立进程，并用 `-DisablePlugins=FogOfWar` 隔离项目中替换 stock renderer 的外部处理器。基准使用 `15 Hz` 权威逻辑 + MassBattle 四子帧 frame spreading，目标是验证 `60 Hz` 显示：

```powershell
./Tools/run_native_tracking_benchmark.ps1
```

例如运行 10,000 辆、10 秒预热、30 秒采样、重复 3 次：

```powershell
./Tools/run_native_tracking_benchmark.ps1 -Units 10000 -LogicHz 15 -WarmupSeconds 10 -SampleSeconds 30 -Repetitions 3
```

底层命令行参数也可直接使用：

```text
-MBSTScenario=actor|mass|turret
-MBSTUnits=500
-MBSTWarmup=10
-MBSTSample=20
-MBSTLogicHz=15
-MBSTNoFrameSpreading
-MBSTTargetRadius=10000
-MBSTTargetPeriod=12
-MBSTTargetHealth=1000000000
-MBSTLegacySpawnPerFrame=100
-MBSTOutputDir=<absolute directory>
-MBSTSkipScreenshots
-MBSTNoExit
```

若做项目发布前的 Unreal Insights 对照，仍应至少采集：

```text
Game Thread / Mass Processing
MassBattleAgentRenderProcessor
MBSTSingleTurretLogicPack
Niagara GT / RT / GPU
GPU BasePass / ShadowDepths
Draw Calls / Instances / Triangles
FrameTime P50 / P95 / P99
```

## 后续可选优化

Niagara 每粒子预计算、逻辑帧门控、MobileFire 融合打包、运动矢量/阴影裁剪与默认顶点法线 permutation 已完成；LOD 完全保留资产/Scalability 原策略。在不修改 MassBattleFrame 的边界内，后续优先级是：减少/Atlas 化 Section、为超远距离增加 impostor，并建立真正的 Rigid/Transport/Position/Full 四级 A/B。当前最终 profile 的主要余量仍在共享 `PrePass/BasePass`，不是炮塔状态增量。
