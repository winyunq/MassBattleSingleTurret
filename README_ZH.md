# MassBattle Single Turret (单实体 VAT 炮塔插件)

`MassBattleSingleTurret` 是专为 Unreal Engine 5.8 + MassBattle 框架设计的独立插件。它能够将任意基于 Actor/Component 组件化装配的坦克或载具，转换为**单实体 (Single Mass Entity) + 单 Niagara 粒子 + 单合并 VAT 静态网格**的无 Actor 炮塔单位。

当前版本：**0.2.3-mobile-fire**。瞄准与炮口计算已统一使用模型根坐标，
修正碰撞体中心与模型底部之间的偏移。详见[正式坦克瞄准与开火验证](Docs/AimBeforeFireValidation.md)。

材质修复保留：修正 Masked 材质的炮塔深度绘制，
同步框架 DP0 解码，并保留转换前的 UV 通道。项目里已经复制出来的材质也需要升级；
只更新插件模板不会自动修正这些副本。详见[黑炮塔原因与材质升级](Docs/BlackTurretFix.md)。

---

## 🌟 核心优势

- **零 Actor / 零子 Agent 开销**：传统方式需要“车体 Entity + 炮塔子 Entity + 炮管子 Entity”或完整 Actor 绑定，本插件运行时仅需 **1 个 Mass Entity** 驱动。
- **顶点色关节与 32 位状态压缩**：Mass/Niagara 只传一个 32-bit PackedState；Niagara GPU 每粒子解包并预计算三角函数，材质顶点阶段只做刚性关节变换。
- **零开销过滤**：通过内嵌的 `FMBSTSingleTurretTag` 进行精确匹配，普通无炮塔单位完全不进入热路径处理。
- **机械单位默认军团路径**：坦克、火炮、防空炮等“一个根刚体 + 少量刚性关节”的单位优先走本插件，避免 Actor、子 Agent 与父子 Transform 传播。保留资产原生 `MinLOD=0` 和 LOD `0..4` 槽位 ABI 时，最终 10,000 单位 DX12 三次独立进程中位数为 **71.26 FPS**；炮塔与普通单实体 Mass 的 GPU 差值为 **-0.022 ms（测量噪声内）**。

---

## 🎯 重点：怎么标记炮塔 (How to Mark Turrets)

源 Actor 不需要继承专用的坦克基类，只需要通过组件或 Tag 标出**炮塔旋转轴、炮管俯仰轴与炮口位置**。标记仅用于编辑期/转换期，**不会保留在运行时 Entity 上**。

### 方法一：添加 `UMBSTSingleTurretAuthoringComponent` 组件（推荐）

在源 Actor 蓝图中添加 `UMBSTSingleTurretAuthoringComponent` 组件，并在 Details 面板中直接指定绑定的 SceneComponent：

| 标记属性 | 类型 | 作用描述 |
| :--- | :--- | :--- |
| **TurretYawPivot** | `USceneComponent*` | **必填**。控制炮塔水平 Yaw 旋转的枢轴组件（0° ~ 360°）。 |
| **BarrelPitchPivot** | `USceneComponent*` | **可选**。控制炮管垂直 Pitch 俯仰的枢轴组件（-90° ~ 90°）。若未指定，仍作为单 Yaw 轴炮塔。 |
| **Muzzle** | `USceneComponent*` | **可选**。炮口枢轴/Socket 组件。用于 CPU 重建炮口世界坐标、发射子弹与开火特效。 |
| **VATDriver** | `USkeletalMeshComponent*` | **可选**。若车体有骨骼动画（如履带/悬挂 VAT），在此指定动画驱动组件。 |

---

### 方法二：使用 Component Tag 标记（Tag Fallback）

如果您不想或无法修改蓝图代码直接引用组件，可在对应的 `USceneComponent` 的 **Component Tags** 数组中添加以下预设 Tag：

- `MBST_TurretYawPivot`：标记该组件为炮塔 Yaw 旋转轴。
- `MBST_BarrelPitchPivot`：标记该组件为炮管 Pitch 俯仰轴。
- `MBST_Muzzle`：标记该组件为炮口位置。
- `MBST_VATDriver`：标记该组件为车体 VAT 驱动器。

> **提示**：转换器识别到 Pivot 后，会自动将位于该 Pivot 子树下的所有 Mesh Component 划分到对应的部件组（Body / Turret / Barrel）。

---

### 转换时的顶点色自动编码约定

转换器在生成合并 StaticMesh 时，会自动将各部件的关节控制掩码写入**顶点色 (VertexColor)** 中：

| 部件 (Sub-mesh) | R (Yaw) | G (Pitch) | B (Body VAT) | A (Recoil) |
| :--- | :---: | :---: | :---: | :---: |
| **车体 (Body)** | `0` | `0` | `0` 或 `1` | `0` |
| **炮塔 (Turret)** | `1` | `0` | `0` | `0` |
| **炮管 (Barrel)** | `1` | `1` | `0` | `1` |

---

## 🚀 完整使用指南 (Step-by-Step Usage)

### 第一步：标记源 Actor

参照上方【怎么标记炮塔】小节，在源 Actor 中使用 `UMBSTSingleTurretAuthoringComponent` 或 `Component Tag` 完成标注。

---

### 第二步：执行 Actor 转换

在编辑器蓝图或 C++ 中调用转换入口：
`Convert Actor To Single Turret VAT`

**转换流程图**：
```text
读取 SourceActor 当前可见装配
 └─► 根据 Authoring 标记划分 Body / Turret / Barrel
      └─► 分组提取 Mesh 并生成包含顶点色掩码的合并 StaticMesh
           └─► 生成 Pivot Sockets、Sockets 及 LayoutAsset
                └─► 复制 AgentConfig 模板并注入 SingleTurret 契约
                     └─► 自动配置 Renderer Class 与 Niagara 数据接口
```

**生成的契约数据自动写入新 AgentConfig 的指定位置**：
- `ExtraData.Tags` ◄─ `FMBSTSingleTurretTag`
- `ExtraData.Fragments` ◄─ `FMBSTSingleTurretState`
- `ExtraData.MutableSharedFragments` ◄─ `FMBSTSingleTurretShared` (包含 Layout 引用与旋转速度限制)

---

### 第三步：运行时生成单位

使用 MassBattle 标准的 **`Spawn by Config`** 蓝图节点或 C++ 接口，传入第二步生成的 `AgentConfig` 资产即可批量生成炮塔单位。

- 生成的实体直接携带单实体炮塔 Tag 与 Fragment。
- 零 Actor 依赖，场景中无需保留原 Actor 蓝图。

---

### 第四步：运行时瞄准与控制 API

在游戏过程中，可通过 C++ / 蓝图 API 对指定 Mass 实体控制炮塔：

```cpp
// 1. 世界坐标瞄准：使炮塔和炮管指向目标世界位置
Aim Turret At World Location(TargetLocation);

// 2. 角度控制：直接指定目标 Yaw 与 Pitch 角度
Set Turret Target Angles(TargetYaw, TargetPitch);

// 3. 触发开火后坐力：触发炮管后坐力动画（自动按时间衰减）
Trigger Turret Recoil();

// 4. CPU 重建世界变换：实时计算炮塔、炮管与炮口的世界位置/朝向（供子弹/特效/音效使用）
Get Single Turret Pose(OutTurretTransform, OutBarrelTransform, OutMuzzleTransform);
```

---

### 第五步：走 A 与移动开火策略配置 (`UMBSTMobileFireProfile`)

插件内置了完善的走 A（Attack-Move）与移动开火系统，通过在 `AgentConfig` 中关联 `UMBSTMobileFireProfile` 数据资产决定单位行为：

| 移动策略 (MobilityPolicy) | 行军瞄准 | 射击时移动 | 典型适用单位 |
| :--- | :--- | :--- | :--- |
| **`StopTurnChassisAndFire`** | 车体转向 | 停车射击 | 无旋转炮塔坦克、突击炮、固定火炮 |
| **`AimWhileMovingStopToFire`** | 炮塔跟踪 | 获得目标后临时制动，稳定后射击，随后继续移动 | 普通主战坦克、重型火炮 |
| **`AimAndFireWhileMoving`** | 炮塔跟踪 | 保持行进速度，边走边开火 | 稳定炮塔现代坦克、步兵战车、轻型载具 |

- **普通 Move 命令 (`bMoveToLocked = true`)**：单位保持锁定移动，炮塔自动跟踪敌人；仅 `AimAndFireWhileMoving` 会自动开火。
- **A / Attack-Move 命令 (`bMoveToLocked = false`)**：停车型策略通过临时 Movement Gate 制动并射击，射击完成后恢复原本目的地移动，**不偷换也不取消原 MoveTo 路径**。

---

## 🛠️ 示例关卡与测试

插件在 `/MassBattleSingleTurret/Demo/` 目录下提供了完整的验证关卡：

1. **基础坦克示例**：`/MassBattleSingleTurret/Demo/Tank`
   包含生成的 Mesh、Layout、AgentConfig、Renderer BP 及 Niagara 材质。
2. **移动开火/走 A 演示**：`/MassBattleSingleTurret/Demo/MobileFire/Map_MBST_MobileFire`
   可直观观察停车射击组（橙色曳光）与移动射击组（青色曳光）的移动与射击表现。
3. **RTS 框选实战关卡**：`/MassBattleSingleTurret/Demo/RTS/Map_MBST_RTS_MobileFire`
   复制自 MassBattleFrame RTS 场景，默认生成 5,000 己方 + 5,000 敌方单实体 GPU 炮塔坦克；支持框选单位、右键移动与 Attack-Move 规则验证。
4. **性能基准测试关卡**：`/MassBattleSingleTurret/Demo/Benchmark/Map_MBST_NativeTrackingBenchmark`

可以通过 Powershell 自动化脚本运行回归测试：
```powershell
# 运行移动开火功能回归测试
Tools/run_mobile_fire_functional.ps1 -Units 64

# 运行 500 单位移动追踪性能基准
Tools/run_native_tracking_benchmark.ps1 -Units 500
```

---

## 📄 关联文档

- [Actor 与单位转换机制](Docs/01_Actor_Conversion_ZH.md)
- [Niagara 与材质解包说明](Docs/02_Niagara_Material_ZH.md)
- [运行时流程与状态压缩](Docs/03_Runtime_ZH.md)
- [性能对比与 Benchmark](Docs/04_Performance_ZH.md)
- [移动开火与走 A 策略详述](Docs/06_MobileFire_ZH.md)

---

## 📜 许可与贡献

本插件遵循 [MIT License](LICENSE) 开源协议。欢迎提交 Issue 与 Pull Request！
