# Actor 与单位转换

## 标记不是运行时功能组件

`UMBSTSingleTurretAuthoringComponent` 只在编辑/转换阶段回答四个问题：炮塔 Pivot 在哪、炮管 Pivot 在哪、炮口在哪、谁是 VAT Driver。它可以直接绑定任意现有 SceneComponent，也可以用 `MBST_*` Component Tag 查找。

它不会保留在 Mass 实体上。真正的运行时炮塔能力由转换器写入新 AgentConfig 的 Mass 数据组成。

## 原子转换事务

`Convert Actor To Single Turret VAT` 的处理顺序是：

```text
读取 SourceActor 当前可见装配
→ 根据 Authoring 标记划分 Body / Turret / Barrel
→ 分组转 StaticMesh 并写顶点色关节掩码
→ 合并成一个 Mesh，写 Pivot/Muzzle Socket 和扩展 Bounds
→ 新建共享 LayoutAsset
→ 可选新建/烘焙 VATDataAsset
→ 新建 AgentConfig（可从普通模板复制）
→ 自动注入 Tag + State Fragment + Mutable Shared Fragment
→ 所有关键输出有效才返回成功
```

自动写入的位置固定为：

```text
ExtraData.Tags                   : FMBSTSingleTurretTag
ExtraData.Fragments              : FMBSTSingleTurretState
ExtraData.MutableSharedFragments : FMBSTSingleTurretShared
```

转换器会先清掉错误槽位或重复的同类型数据，再各写一份。`Validate Agent Config Single Turret` 要求三者都恰好为 1，且 Shared Fragment 指向本次生成的 Layout。

## 模板语义

`Settings.AgentConfigTemplate` 是可选输入：

- 有模板：复制一个新资产，继承普通单位的移动、攻击、阵营、可视化等配置；
- 无模板：从 MassBattle 默认值创建新资产；
- 两种情况都会在新资产中自动写入炮塔契约；
- 原模板永远不被修改。

已存在同名输出时转换会拒绝覆盖，避免误改资产。要重新生成，应明确删除或换一个输出名。

## 任意 Blueprint Actor 的要求

源 Actor 不需要继承专用坦克基类，但至少需要：

- 可收集的 Body Mesh；
- 可收集的 Turret Mesh；
- 能解析到的 `TurretYawPivot`；
- 炮塔视觉组件位于该 Pivot 子树，或在临时转换实例中按实际装配关系重新挂接。

`BarrelPitchPivot` 可省略；省略时仍是一个可 Yaw 的单炮塔单位。

## Slot 与 Construction Script

默认：

```text
bIncludeHiddenComponents = false
bRerunConstructionScripts = false
```

推荐先在临时/预览 Actor 上调用其装备切换逻辑，确认当前可见装配，再转换。默认不重跑 Construction Script，避免刚选择的 Slot 被重置。源 Blueprint 资产不会因临时实例调整而改变。

## 顶点色约定

| 部件 | R: Yaw | G: Pitch | B: Body VAT | A: Recoil |
|---|---:|---:|---:|---:|
| Body | 0 | 0 | 0 或 1 | 0 |
| Turret | 1 | 0 | 0 | 0 |
| Barrel | 1 | 1 | 0 | 1 |

## VAT、ISM 与 Bounds

纯静态坦克可关闭 VAT。车体有骨骼动画时指定 VAT Driver 和模板；身体 VAT 位移必须乘 `VertexColor.B`，避免炮塔错误继承车体变形。

转换器当前不展开 ISM/HISM 的每个实例，需要先变成普通 MeshComponent。WPO 不会自动扩大 CPU 剔除边界，长炮管应增大 `BoundsExtension`。
