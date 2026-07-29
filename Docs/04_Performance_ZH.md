# 性能设计与对照方法

## 已验证的结构差异

MassBattleFrame Demo 的 `BP_TankActor` 含 4 个 `MassBattleAgentComponent`：

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

它会重新读取旧 Blueprint 的 Agent 组件数，并验证新 AgentConfig 中恰好有一份 Tag/State/Shared 契约。

## CPU 热路径

只查询 `FMBSTSingleTurretTag`，每个匹配实体执行固定长度逻辑：

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

新方案减少表现实例和实体关系，但合并 Mesh 的顶点/Section 并不会消失。当前 Beta 材质在每个顶点执行状态恢复、解包、sin/cos 和最多两次刚性旋转。实际瓶颈仍可能来自：

1. LOD 顶点数与屏幕覆盖；
2. Material Section；
3. 动态阴影；
4. Niagara `StyleArray` 全量提交；
5. 材质顶点阶段的解包和三角函数；
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

### 自行调整数量并复测

插件提供运行脚本，默认就是上述 500 辆、3 次独立运行：

```powershell
./Tools/run_native_tracking_benchmark.ps1
```

例如改为 2,000 辆、5 秒预热、15 秒采样、重复 3 次：

```powershell
./Tools/run_native_tracking_benchmark.ps1 -Units 2000 -WarmupSeconds 5 -SampleSeconds 15 -Repetitions 3
```

底层命令行参数也可直接使用：

```text
-MBSTScenario=actor|mass|turret
-MBSTUnits=500
-MBSTWarmup=10
-MBSTSample=20
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
UMBSTSingleTurretPackProcessor
Niagara GT / RT / GPU
GPU BasePass / ShadowDepths
Draw Calls / Instances / Triangles
FrameTime P50 / P95 / P99
```

## 后续可选优化

若 Profile 证明瓶颈成立，可只改本插件：Niagara 预计算 sin/cos、远 LOD 禁用炮塔 WPO、减少 Section，或改用持久 Structured Buffer/自定义 Niagara DI。Actor 标记、新 AgentConfig 自动注入和运行时控制 API 无需改变。
