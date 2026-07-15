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

## 公平的 FPS/Unreal Insights 对照

不要把旧 BP Actor 数量与新 Mass Entity 数量混在同一场景。建议复制同一张性能地图，建立两个独立 Variant：

```text
A：原 BP_TankActor / 原 AgentConfig，数量 N
B：Tank_SingleTurret_AgentConfig，数量 N
```

两边必须固定：相同位置、相同相机路径、LOD/Cull、阴影、攻击开关、Tick Budget、预热时间和统计窗口。至少采集：

```text
Game Thread / Mass Processing
MassBattleAgentRenderProcessor
UMBSTSingleTurretPackProcessor
Niagara GT / RT / GPU
GPU BasePass / ShadowDepths
Draw Calls / Instances / Triangles
FrameTime P50 / P95 / P99
```

当前仓库给出结构验证和 CPU 打包微基准，但没有伪造场景 FPS。只有完成上述同场景对照后，才应宣称具体帧率收益。

## 后续可选优化

若 Profile 证明瓶颈成立，可只改本插件：Niagara 预计算 sin/cos、远 LOD 禁用炮塔 WPO、减少 Section，或改用持久 Structured Buffer/自定义 Niagara DI。Actor 标记、新 AgentConfig 自动注入和运行时控制 API 无需改变。
