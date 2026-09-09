# 走 A、停火与移动开火

## 结论

走 A 已完整放在 `MassBattleSingleTurret` 插件内，没有修改 `MassBattleFrame`。它不是项目级二选一，而是由每个炮塔单位 AgentConfig 内嵌的 `UMBSTMobileFireProfile` 决定，因此不同策略可以处于同一个 Mass 世界和同一条移动命令中。

三种 `MobilityPolicy`：

| 策略 | 行军瞄准 | 射击时移动 | 典型用途 |
| --- | --- | --- | --- |
| `StopTurnChassisAndFire` | 车体转向 | 停车 | 固定炮、无独立炮塔车辆 |
| `AimWhileMovingStopToFire` | 炮塔跟踪 | 获得战斗授权后请求停车，满足自身稳定条件再射击 | 坦克停车射击、火炮 |
| `AimAndFireWhileMoving` | 炮塔跟踪 | 继续移动 | 稳定炮塔坦克、轻型载具 |

Profile 同时说明单位是否具备“普通移动中自动开火”的能力；MassBattle 已有的 `FMoving::bMoveToLocked` 仍用于区分普通 Move 与可打断的 Attack-Move：

| 当前命令 | `bMoveToLocked` | 炮塔跟踪 | 开火/停车规则 |
| --- | --- | --- | --- |
| 普通 Move | `true` | 允许 | 仅 `AimAndFireWhileMoving` 可自动开火，且不得停车或战斗转车体；其余策略只跟踪 |
| A / Attack-Move | `false`（活动的可打断 MoveTo） | 允许 | 按单位 Profile：停车型可临时制动，移动射击型继续移动 |
| 无排他移动或显式 Attack | `false` | 允许 | 按单位 Profile/攻击命令 |

因此，扫描到目标不会偷换或取消普通 Move。普通 Move 中两种炮塔单位都可以索敌和转炮塔；只有明确配置 `AimAndFireWhileMoving` 的单位会在保持锁定 MoveTo、保持车体运动且不请求停车的情况下自动开火。停车型和固定炮型只跟踪，必须等普通 Move 结束或获得 Attack/Attack-Move 战斗授权后才能射击。A/Attack-Move 中，移动开火单位仍边走边打，停车开火单位临时制动后打。“停车走 A”不会取消当前 MoveTo：它只在原 Move Processor 执行前暂时把 `FMove.XY.bStopActiveMovement` 置为真，并在 Move 后立刻恢复原值，所以冷却/恢复结束后单位继续原目的地。

## AgentConfig 如何得到能力

推荐入口仍是插件的 Actor→单位转换：

1. 源 Actor 用 `MBST Single Turret Authoring` 标出炮塔 Yaw、可选 Pitch 和 Muzzle。
2. 在转换设置中选择 `MobileFireProfile`；不选择则只生成普通炮塔单位。
3. 转换器新建 AgentConfig，并自动写入：

```text
ExtraData.Tags                   += FMBSTMobileFireTag
ExtraData.Fragments              += FMBSTMobileFireState
ExtraData.MutableSharedFragments += FMBSTMobileFireShared(Profile snapshot)
FMBSTSingleTurretState.bExternalMotionDriver = true
```

Profile 的 `bDisableBuiltInAttack` 和 `bDisableBuiltInChase` 默认开启。转换器只会关闭**新 AgentConfig** 的对应功能，避免原 Behavior 再发一发或 Chase 改写走 A 的 MoveTo；不会修改模板和普通单位。

已有炮塔 AgentConfig 也可以在编辑器蓝图/C++调用：

```text
Configure Agent Config Mobile Fire
Validate Agent Config Mobile Fire
```

Profile 被编辑后，应再次执行 Configure，让 AgentConfig 中的缓存友好共享快照同步。关闭插件契约时，函数不会猜测并恢复以前的 Attack/Chase 开关，应按单位原设计手动恢复。

## 运行时 Processor 顺序

原 Processor 必须保持启用，插件没有复制它们：

```text
MBST Movement Gate
  -> MassBattle Move
  -> MBST Movement Gate Restore
  -> MassBattle Trace
  -> MassBattle Behavior
  -> MBST Mobile Fire Combat
  -> MBST Turret Pack (FrameEnd)
```

Combat 默认读取 `FTracing::TraceResult` 作为目标。也可对单个实体调用：

```text
Set Mobile Fire Target Entity
Set Mobile Fire Target World Location
Clear Mobile Fire Target Override
Set Mobile Fire Enabled
```

## 发射、伤害与特效

Profile 有两种输出，可单独或同时启用：

- `bSpawnMassBattleProjectile`：使用指定、已经验证过的 `ProjectileConfig` 生成 MassBattle projectile；
- `bEmitFireRequest`：把一次被接受的射击写成 `FMBSTFireRequest`，含 Shooter、Target、瞄准点、目标速度和炮口世界变换，供弹道、hitscan、伤害、声音和特效系统消费。

大量单位应在 C++ 中订阅 `NativeOnFireRequest` 或每帧批量调用 `DrainMobileFireRequests`；若开启请求输出但长期不 Drain，队列会增长。`bBroadcastBlueprintFireEvent` 是方便用的逐发动态委托，默认关闭。

Demo 为了只验证走 A 和炮塔逻辑，启用了 FireRequest 并画曳光线，没有绑定实际 ProjectileConfig，也没有施加伤害。这避免把未验证的弹丸资产误当成已完成武器配置。

## 手工观察关卡

在 Content Browser 打开：

```text
/MassBattleSingleTurret/Demo/MobileFire/Map_MBST_MobileFire
```

点击 Play：

- 橙色曳光：`Tank_StopToFire_AgentConfig`，边走边瞄准，射击时降到阈值内；
- 青色曳光：`Tank_FireWhileMoving_AgentConfig`，不中断移动并持续射击；
- 两组都调用同一个原生 `UMassBattleBPTaskAgentsMoveTo`；
- 默认每组 64 辆。选择关卡里的 `MBSTMobileFireDemoActor` 可修改 `Units Per Policy`；
- 默认是自由 Spectator，点击视口后鼠标观察、`WASD` 飞行。

要查看单位是否携带走 A，在两个 AgentConfig 的 Details 中展开 `Extra Data`，检查 MobileFire Tag、State、Shared；同时可打开两个 `DA_MBST_*` Profile 对照策略。

RTS 手动验证关卡：

```text
/MassBattleSingleTurret/Demo/RTS/Map_MBST_RTS_MobileFire
```

手动 RTS 关卡复制自 MassBattleFrame 的 `GameMap_RTS`，保留原生 RTS 相机、框选和右键移动。默认生成 `5,000` 己方 + `5,000` 敌方 `AimAndFireWhileMoving` 单实体 GPU 炮塔坦克；原图的 6 个 Soldier/Tank/Helicopter Spawner 已从复制地图移除，避免旧单位或损坏的 `_Trash` AgentConfig 混入。万人默认关闭逐发 Debug Tracer、世界标签和范围圈，这些调试绘制不是炮塔参数链的一部分，开启后会形成显著的 CPU/绘制次级成本。

两军使用 `500 cm` 阵距并分置于地图两侧。运行时验证标记应为：

```text
MBST_RTS_MOBILE_FIRE_READY: success=1 moving_fire_only=1 player_stop=0 player_move=5000 enemy=5000 ... native_rts_commands=1 turret_profiles_chassis_aim=0
```

框选后会在屏幕及 Output Log 打印一次变化诊断：

```text
MBST_RTS_SELECTION: selected=... | stop_to_fire=... moving_fire=... no_turret=... actor_compound=... | move_order=... attack_move=... free=...
```

框选己方坦克并下普通 Move：炮塔应自动锁敌和跟踪，MoveTo 不应被目标打断，车体不应被战斗强转，同时弹数应持续增加。`AimAndFireWhileMoving` 必须在 `bMoveToLocked=true`、原 MoveTo 仍活动且没有申请 movement hold 时开火。停车型策略仍由独立功能关卡和自动回归验证：普通 Move 只跟踪，Attack-Move 才申请 movement hold、满足自身稳定条件后开火。速度只用于各 Profile 的物理稳定限制和诊断，不用于区分命令或单位类型。

## 原生功能回归

运行脚本：

```powershell
Tools/run_mobile_fire_functional.ps1 -Units 64
```

脚本用 UE 独立进程测逻辑，不用 Blueprint/Python 计时。它要求日志出现 `MBST_MOBILE_FIRE_FUNCTIONAL_RESULT: PASS`。判定依据是命令状态、movement hold 和发射请求，不再用任意的 `100 cm/s` 作为玩法定义。

```text
stop_hold_peak>0, stop_shots>0
stop_fire_speed_max<=profile.stop_fire_speed_limit
move_hold_peak=0, move_shots>0
move_fire_during_active_move>0
```

这证明停车组在 Attack-Move 中确实请求停车并仅在自己的稳定条件内开火；移动开火组从未申请停车，并在原生可打断 MoveTo 仍活动时成功开火。RTS 场景另有 `-MBSTRtsLockedMoveProbe`：两组都必须在 `bMoveToLocked=true` 时锁敌；停车组发数必须为 0，移动射击组必须在没有 movement hold 且速度大于 0 时发射。

最新 RTS 场景回归结果：

```text
LOCKED MOVE: PASS  stop_acquired=1 stop_shots=0 move_acquired=1 move_shots=33 move_shot_without_hold=1 move_fire_speed_max=700.0
ATTACK MOVE: PASS  stop_shot_with_hold=1 move_shot_during_attack_move=1 move_shot_without_hold=1
```

三个 RTS AgentConfig 没有各自复制一套坦克网格或坐标系。原始配置、停车配置和移动开火配置都引用同一个 `Renderer_Tank_SingleTurret`、同一个 SingleTurret Layout，并继承相同的 Visualize Transform；RTS 配置只增加/更新走 A 的 Profile 契约和测试血量。运行时样本也满足 `世界目标方位 ≈ 车体 Yaw + 炮塔本地 Yaw`，未发现固定 `+90°/-90°` 偏差。

自动化测试：

```text
MassBattle.SingleTurret.MobileFire.AgentConfigContract
MassBattle.SingleTurret.MobileFire.PolicyGate
MassBattle.SingleTurret.MobileFire.CreateDemoAssets
```

这些回归不关闭或替换 MassBattle 的原 Processor，也没有修改 `MassBattleFrame` 源码。
