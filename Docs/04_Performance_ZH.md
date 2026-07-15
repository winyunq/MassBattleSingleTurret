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

## 已实现的 5000 对 5000 场景对照

仓库内已经建立两个互不混跑的地图 Variant：

```text
/MassBattleSingleTurret/Demo/Benchmark/Map_MBST_Legacy_5000v5000
/MassBattleSingleTurret/Demo/Benchmark/Map_MBST_SingleTurret_5000v5000
```

两张地图都由插件自己的 `AMBSTSingleTurretBenchmarkActor` 驱动，不修改 MassBattleFrame。默认条件：

```text
两队各 5000 辆，50 x 100 队形，500 uu 间距
1920 x 1080，ScreenPercentage 100，VSync/MaxFPS/MotionBlur 关闭
炮塔每帧强制往复：±75°，8 秒周期
真实墙钟预热至少 10 秒；真实墙钟正式采样 20 秒
攻击、Trace、移动和调试关闭；Visualize 开启
视觉齐射仅用于预热截图，不进入正式采样
原蓝图 LogBlueprintUserMessages 关闭，避免磁盘日志污染
```

旧场景逐辆生成原 `/MassBattle/Test/CompoundUnitAsset/BP_TankActor`，每辆保留原来的 4 个 Agent 实体；插件场景直接从 `Tank_SingleTurret_AgentConfig` 生成，每辆 1 个实体。二者不在同一进程中混跑。

2026-07-16 当前机器 Development Editor、RenderOffscreen 的正式墙钟结果：

| 指标 | 旧 Demo 复合坦克 | 插件单实体坦克 | 旧/新帧时间倍率 |
|---|---:|---:|---:|
| 坦克数 | 10,000 | 10,000 | 1.00x |
| Mass 单位实体 | 40,000 | 10,000 | 4.00x |
| 样本数 | 16 | 119 | - |
| 平均帧时间 | 1306.829 ms | 169.152 ms | 7.73x |
| 平均 FPS | 0.765 | 5.912 | 7.73x |
| P50 | 1250.564 ms | 167.418 ms | 7.47x |
| P95 | 1576.489 ms | 174.929 ms | 9.01x |
| P99 | 2054.399 ms | 177.613 ms | 11.57x |
| Max | 2173.877 ms | 1016.550 ms | 2.14x |

JSON 中的 `frame_timing_source` 为 `FPlatformTime wall-clock interval`，避免低帧率时 UE 把 `DeltaSeconds` 截到 400 ms 而得到假结果。结果文件和截图默认写入：

```text
Saved/MassBattleSingleTurret/Benchmark5000
```

命令行可调参数：

```text
-MBSTTanksPerSide=5000
-MBSTWarmup=10
-MBSTSample=20
-MBSTLegacySpawnPerFrame=250
-MBSTOutputDir=<absolute directory>
-MBSTSkipScreenshots
-MBSTNoExit
```

这个结果衡量的是“实际旧 Demo 复合 Actor/多实体/多表现实例”与“插件直接生成的单实体炮塔坦克”的整体差异，不是只隔离炮塔算术的微基准。旧 Demo 还保留 10,000 个 Blueprint Actor，而插件路径运行时不保留 Actor；因此倍率不能解释成单个 Fragment 或单个 Processor 本身快了 7.73 倍。

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
