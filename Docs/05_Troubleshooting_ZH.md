# 排错清单

## 炮塔完全不动

- Niagara 是否暴露 `User.StyleArray : Niagara Array Int32`；
- 是否用与原位置数组相同的稳定 InstanceId 读取；
- Spawn 和 Update 是否都把 packed int32 拆成低/高 16 位写入 Dynamic Material Parameter；
- Agent 是否由本插件转换器生成的新 AgentConfig 生成；
- 新 AgentConfig 是否通过 `Validate Agent Config Single Turret`；
- Renderer Class 是否指向生成 Mesh 和改造后的 Niagara。

## 整个坦克一起旋转

顶点色丢失或材质没有读取 VertexColor.R/G。运行：

```text
Validate Generated Articulated Mesh
```

## 炮管不跟随炮塔

变换顺序错误。必须是：

```text
Recoil → Pitch → Yaw
```

## 炮塔绕错误位置旋转

- 检查选择的是实际 Pivot 节点；
- 运行 `Configure Articulation Material Instance`；
- 检查 Layout 中的对象空间 Pivot；
- 确认材质参数和输入位置都在 Object/Local Space。

## 旋转方向相反

将本地轴取反，例如 `(0,1,0)` 改为 `(0,-1,0)`。

## VAT 后炮塔变形

材质必须把身体 VAT 位移乘以 `VertexColor.B`。

## 屏幕边缘提前消失

增大 `BoundsExtension`。

## Style 错乱

不要把整个 PackedState 直接转成单个 float。应拆成两个 16 位整数传给材质，再恢复完整 uint32；VisualStyle 是恢复值的低 8 位。

## 编译 API 差异

优先检查：

- StaticMesh MeshDescription/Vertex Color API；
- AnimToTexture DataAsset 与 Bake 函数签名；
- Niagara Parameter Store API；
- UStaticMesh Bounds setter；
- Mass Shared Fragment 查询 API。

所有适配都应保留在本插件内，不修改 MassBattle 原源码。
