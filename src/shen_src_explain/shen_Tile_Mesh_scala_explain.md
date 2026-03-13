# Tile.scala + Mesh.scala 精读笔记

> **文件路径**：`src/main/scala/gemmini/Tile.scala` 和 `src/main/scala/gemmini/Mesh.scala`
> **阅读前提**：已完成 `PE.scala` 精读
> **RSNCPU 关注点**：移位寄存器打拍方式（流水线寄存器参考）、阵列互连与数据流实现

---

## 1. 架构层次总览

Gemmini 脉动阵列采用**三级层次结构**：

```
PE（处理元素）→ Tile（瓦片）→ Mesh（网格）
```

在默认 INT8 配置（`Configs.scala`）下：

| 参数 | 默认值 | 含义 |
|------|-------|------|
| `tileRows` | 1 | 每个 Tile 内的 PE 行数 |
| `tileColumns` | 1 | 每个 Tile 内的 PE 列数 |
| `meshRows` | 16 | Mesh 中的 Tile 行数 |
| `meshColumns` | 16 | Mesh 中的 Tile 列数 |

**实际效果**：默认配置下每个 Tile 只含 **1 个 PE**，整个 Mesh = 16×16 = **256 个 PE**。

为什么还要有 Tile 这一层？设计解耦——Tile 内部是**纯组合逻辑**（PE 间直连），Tile 之间由 Mesh 插入**流水线寄存器**。如果想增加 Tile 内 PE 数（如 `tileRows=2, tileColumns=2`），可以减少寄存器级数以换取面积，只需改配置参数。

```
                    Mesh（16×16 Tiles）
    ┌─────────────────────────────────────────────┐
    │  ┌──────┐  reg  ┌──────┐  reg  ┌──────┐    │
    │  │Tile  │──────→│Tile  │──────→│Tile  │... │  ← 行 0
    │  │(1×1) │       │(1×1) │       │(1×1) │    │
    │  └──┬───┘       └──┬───┘       └──┬───┘    │
    │     │reg            │reg            │reg     │
    │  ┌──▼───┐       ┌──▼───┐       ┌──▼───┐    │
    │  │Tile  │──────→│Tile  │──────→│Tile  │... │  ← 行 1
    │  │(1×1) │       │(1×1) │       │(1×1) │    │
    │  └──┬───┘       └──┬───┘       └──┬───┘    │
    │     │reg            │reg            │reg     │
    │    ...             ...             ...      │
    └─────────────────────────────────────────────┘
```

---

## 2. Tile.scala —— 纯组合逻辑的 PE 阵列

### 2.1 类签名

```scala
class Tile[T <: Data](inputType: T, weightType: T, outputType: T, accType: T,
                      df: Dataflow.Value, tree_reduction: Boolean,
                      max_simultaneous_matmuls: Int,
                      val rows: Int, val columns: Int)
                     (implicit ev: Arithmetic[T]) extends Module
```

关键参数：
- `rows` / `columns`：Tile 内 PE 的行数/列数（默认都是 1）
- `tree_reduction`：是否用加法树替代链式累加（优化关键路径）
- `df`：数据流模式（WS 或 OS），直接透传给 PE

### 2.2 端口结构

```
             in_b(col)   in_d(col)   in_control(col)   in_valid(col)   in_id(col)   in_last(col)
               ↓            ↓            ↓                ↓               ↓             ↓
in_a(row) → ┌──────────────────────────────────────────────────────────────────────────────────┐
            │                           Tile (rows × columns PEs)                              │
            └──────────────────────────────────────────────────────────────────────────────────┘
               ↓            ↓            ↓                ↓               ↓             ↓      → out_a(row)
             out_b(col)   out_c(col)   out_control(col)  out_valid(col)  out_id(col)  out_last(col)
```

**输入/输出维度**：
- 水平方向（`in_a` / `out_a`）：`Vec(rows, inputType)` —— 每行一个激活值入口
- 垂直方向（`in_b` / `out_b` 等）：`Vec(columns, outputType)` —— 每列一个入口

### 2.3 PE 实例化与 transpose 技巧

```scala
val tile = Seq.fill(rows, columns)(Module(new PE(...)))
val tileT = tile.transpose
```

`tile` 是一个 `Seq[Seq[PE]]`，按 `tile(row)(col)` 访问。`tileT = tile.transpose` 把行列翻转，变成 `tileT(col)(row)`，方便按列遍历。

| 变量 | 索引方式 | 遍历方向 |
|------|---------|---------|
| `tile(r)` | 第 r 行所有 PE | 水平遍历（左→右） |
| `tileT(c)` | 第 c 列所有 PE | 垂直遍历（上→下） |

### 2.4 核心模式：`foldLeft` 链式连接

这是 Tile.scala 和 Mesh.scala 中反复出现的核心 Scala 模式，**必须彻底理解**。

#### foldLeft 基础

`foldLeft` 对序列从左到右依次"折叠"，把每一步的输出作为下一步的输入：

```scala
List(PE0, PE1, PE2).foldLeft(初始值) { case (上一步输出, 当前PE) => 
  // 把"上一步输出"连到"当前PE"的输入
  // 返回"当前PE"的输出，作为下一步的"上一步输出"
  当前PE的输出
}
```

#### 水平方向 a 信号链

```scala
for (r <- 0 until rows) {
  tile(r).foldLeft(io.in_a(r)) {
    case (in_a, pe) =>
      pe.io.in_a := in_a      // 前一个输出 → 当前输入
      pe.io.out_a              // 当前输出 → 传给下一个
  }
}
```

展开后等价于：

```
io.in_a(r) ──→ PE(r,0).in_a    PE(r,0).out_a ──→ PE(r,1).in_a    PE(r,1).out_a ──→ PE(r,2).in_a ...
                   └── 计算 ──→ out_a               └── 计算 ──→ out_a
```

即**激活值 `a` 从左到右逐个 PE 透传**（PE 内部是 `out_a := in_a`，直接转发）。

#### 垂直方向 b 信号链

```scala
for (c <- 0 until columns) {
  tileT(c).foldLeft(io.in_b(c)) {
    case (in_b, pe) =>
      pe.io.in_b := (if (tree_reduction) in_b.zero else in_b)
      pe.io.out_b
  }
}
```

**注意 `tree_reduction` 分支**：
- **普通模式**（`tree_reduction=false`）：`in_b` 从上到下逐 PE 传递，形成经典的脉动累加链。每个 PE 把部分和加上自己的 `a*weight` 后传给下方
- **树形归约模式**（`tree_reduction=true`）：每个 PE 的 `in_b` 被强制设为 0（`in_b.zero`），各 PE 独立计算 `a*weight`，最后用加法树（`accumulateTree`）一次性求和。优点是**关键路径更短**（O(log N) vs O(N)），但面积更大

#### 其他垂直信号链（d, control, valid, id, last）

结构完全相同，都是用 `tileT(c).foldLeft(...)` 从上到下链式传递，不再重复。

### 2.5 输出驱动

```scala
// 底部输出（垂直信号从最后一行 PE 取出）
for (c <- 0 until columns) {
  io.out_c(c) := tile(rows-1)(c).io.out_c
  io.out_b(c) := tile(rows-1)(c).io.out_b  // 非 tree_reduction 时
  // ... control, id, last, valid 同理
}

// 右侧输出（水平信号从最后一列 PE 取出）
for (r <- 0 until rows) {
  io.out_a(r) := tile(r)(columns-1).io.out_a
}
```

**`tree_reduction` 的 `out_b` 特殊处理**：

```scala
io.out_b(c) := {
  if (tree_reduction) {
    val prods = tileT(c).map(_.io.out_b)     // 收集本列所有 PE 的 out_b
    accumulateTree(prods :+ io.in_b(c))       // 加法树求和（含顶部输入）
  } else {
    tile(rows - 1)(c).io.out_b                // 直接取最底部 PE 的输出
  }
}
```

`accumulateTree` 实现（来自 `Util.scala`）：

```scala
def accumulateTree[T <: Data](xs: Seq[T]): T = {
  if (xs.length == 1) xs.head
  else {
    val pairs = xs.padTo(nextPow2, zero).grouped(2)
    val lowerRow = pairs.map { case Seq(a, b) => a + b }
    accumulateTree(lowerRow.toSeq)           // 递归二分求和
  }
}
```

等价于：

```
PE0.out_b ──┐
            ├─ + ──┐
PE1.out_b ──┘      │
                   ├─ + ──→ total_sum
PE2.out_b ──┐      │
            ├─ + ──┘
io.in_b   ──┘
```

### 2.6 关键认知：Tile 内没有寄存器

注意源码注释：**"A Tile is a purely combinational 2D array of passThrough PEs."**

这意味着 Tile 内部 PE 之间的信号传递是**纯组合路径**（一个时钟周期内完成）。寄存器（用于打拍/流水线划分）在 **Mesh** 层级的 Tile 之间插入。

---

## 3. Mesh.scala —— 带流水线寄存器的 Tile 阵列

### 3.1 核心设计思想

Mesh 在 Tile 之间插入 `ShiftRegister`（延迟寄存器），将大规模脉动阵列分割为多级流水线。

```
Tile(0,0) ──[ShiftRegister]──→ Tile(0,1) ──[ShiftRegister]──→ Tile(0,2) ...
    │                              │                              │
 [ShiftRegister]                [ShiftRegister]                [ShiftRegister]
    │                              │                              │
    ▼                              ▼                              ▼
Tile(1,0) ──[ShiftRegister]──→ Tile(1,1) ──[ShiftRegister]──→ Tile(1,2) ...
```

### 3.2 pipe 函数

```scala
def pipe[T <: Data](valid: Bool, t: T, latency: Int): T = {
  chisel3.withReset(false.B) { Pipe(valid, t, latency).bits }
}
```

`Pipe(valid, data, latency)` 是 Chisel 内置的流水线原语，插入 `latency` 级寄存器。`withReset(false.B)` 禁用复位信号（避免全局复位在 Mesh 中的扇出问题）。

与 `ShiftRegister` 的区别：

| 函数 | 行为 | 使用场景 |
|------|------|---------|
| `ShiftRegister(data, n)` | 无条件延迟 n 拍 | `a`、`valid`、`id`、`last` 等无需门控的信号 |
| `pipe(valid, data, n)` | 延迟 n 拍，且受 `valid` 门控 | `b`、`d`、`control` 等数据信号（无效时不传播） |

### 3.3 水平方向 a 链（Mesh 层）

```scala
for (r <- 0 until meshRows) {
  mesh(r).foldLeft(io.in_a(r)) {
    case (in_a, tile) =>
      tile.io.in_a := ShiftRegister(in_a, tile_latency+1)   // ← 关键：插入寄存器！
      tile.io.out_a
  }
}
```

对比 Tile 内部的同结构代码（无寄存器）：
```scala
// Tile 内部
pe.io.in_a := in_a    // 直连，纯组合

// Mesh 层级
tile.io.in_a := ShiftRegister(in_a, tile_latency+1)  // 打拍，插入流水线寄存器
```

`tile_latency+1` 的含义：
- `tile_latency`：Tile 内部的组合逻辑延迟（用 `tileRows` 个 PE 链式相连的延迟来近似）
- `+1`：额外 1 拍的布线/建立时间裕量
- 默认 `tileRows=1` → `tile_latency=0` → 实际插入 **1 级寄存器**

### 3.4 垂直方向 b 链（Mesh 层）

```scala
for (c <- 0 until meshColumns) {
  meshT(c).foldLeft((io.in_b(c), io.in_valid(c))) {
    case ((in_b, valid), tile) =>
      tile.io.in_b := pipe(valid.head, in_b, tile_latency+1)
      (tile.io.out_b, tile.io.out_valid)
  }
}
```

**注意 foldLeft 的初始值是元组 `(in_b, valid)`**，因为 `pipe` 需要 `valid` 信号做门控。每一步都传递 `(数据, 有效位)` 的元组。

### 3.5 输出级额外延迟

```scala
for (...) {
  b := ShiftRegister(tile.io.out_b, output_delay)
  c := ShiftRegister(tile.io.out_c, output_delay)
  // ...
}
```

`output_delay` 是 Mesh 底部/右侧输出的额外流水线级数，用于**物理设计**——输出信号要跨越整个阵列到达边界，长布线需要额外打拍以满足时序。

### 3.6 Mesh 端口维度

Mesh 的端口是**二维 Vec**：

```scala
val in_a = Input(Vec(meshRows, Vec(tileRows, inputType)))
//                   └ 16 行      └ 每行 1 个值（tileRows=1）
```

即 `in_a(r)(tr)` 表示第 r 个 Tile 行的第 tr 个 PE 行的激活值输入。

### 3.7 数据在 Mesh 中的流动时序

以默认 16×16 Mesh 为例，`a` 信号（水平方向）的流动：

```
时钟周期:  0     1     2     3     4    ...    15
           │     │     │     │     │           │
Tile(0,0)  ■                                        a 到达 Tile(0,0)
Tile(0,1)        ■                                  a 到达 Tile(0,1)（延迟 1 拍）
Tile(0,2)              ■                            a 到达 Tile(0,2)（延迟 2 拍）
Tile(0,3)                    ■                      a 到达 Tile(0,3)（延迟 3 拍）
  ...                                   ...
Tile(0,15)                                     ■    a 到达 Tile(0,15)（延迟 15 拍）
```

同时 `b` 信号从上到下流动，也是每级延迟 1 拍。这就是**脉动阵列**的核心——数据像波浪一样**对角线推进**：

```
时钟周期 0:           时钟周期 1:           时钟周期 2:
┌───┬───┬───┐        ┌───┬───┬───┐        ┌───┬───┬───┐
│ ■ │   │   │        │   │ ■ │   │        │   │   │ ■ │
├───┼───┼───┤        ├───┼───┼───┤        ├───┼───┼───┤
│   │   │   │        │ ■ │   │   │        │   │ ■ │   │
├───┼───┼───┤        ├───┼───┼───┤        ├───┼───┼───┤
│   │   │   │        │   │   │   │        │ ■ │   │   │
└───┴───┴───┘        └───┴───┴───┘        └───┴───┴───┘

■ = 数据前沿（a 和 b 在此处相遇进行计算）
```

一次 N×N 矩阵乘法的总延迟：**N + N - 1 = 2N - 1** 拍（数据填充 + 数据排空）。默认 16×16 = **31 拍**。

---

## 4. foldLeft 模式的完整理解

### 4.1 基本形式

```scala
collection.foldLeft(initialValue) { case (accumulator, element) =>
  // 用 accumulator 和 element 做一些操作
  // 返回新的 accumulator
  newAccumulator
}
```

### 4.2 在硬件连接中的含义

`foldLeft` 在 Gemmini 中的使用模式是**链式信号传递**：

```scala
// 效果：io.input → elem(0).input, elem(0).output → elem(1).input, elem(1).output → elem(2).input ...
elements.foldLeft(io.input) { case (signal, elem) =>
  elem.io.input := signal
  elem.io.output        // 返回值成为下一轮的 signal
}
```

等价的命令式写法：

```scala
var signal = io.input
for (elem <- elements) {
  elem.io.input := signal
  signal = elem.io.output
}
```

### 4.3 元组形式（多信号同时链式传递）

Mesh.scala 中常见多信号一起传递：

```scala
meshT(c).foldLeft((io.in_b(c), io.in_valid(c))) {
  case ((in_b, valid), tile) =>
    tile.io.in_b := pipe(valid.head, in_b, tile_latency+1)
    (tile.io.out_b, tile.io.out_valid)  // 返回元组
}
```

这里 `(数据, 有效位)` 作为一个整体在 Tile 间传递，因为 `pipe` 函数需要 `valid` 来门控数据。

---

## 5. 完整数据流图

以 WS（权重固定）模式、3×3 Mesh 为例：

```
         b0[t]     b1[t]     b2[t]     ← 从上方输入（部分和/权重预载入）
           │         │         │
           ▼         ▼         ▼
a0[t] → [PE00] → [PE01] → [PE02] → out_a0
           │         │         │
           ▼         ▼         ▼        每个 → 和 ↓ 都经过 ShiftRegister(1)
a1[t] → [PE10] → [PE11] → [PE12] → out_a1
           │         │         │
           ▼         ▼         ▼
a2[t] → [PE20] → [PE21] → [PE22] → out_a2
           │         │         │
           ▼         ▼         ▼
         out_b0   out_b1   out_b2      ← 底部输出

水平方向：a（激活值）从左到右流动，每经过一个 Tile 延迟 1 拍
垂直方向：b（部分和）从上到下流动，每经过一个 Tile 延迟 1 拍
PE 内部：out_b = in_b + in_a × weight（权重预载入在 c1/c2 中）
```

---

## 6. Tile + Mesh vs RSNCPU 设计启示

### 6.1 流水线寄存器的启示

Mesh 用 `ShiftRegister` 在 Tile 之间插入流水线寄存器。对 RSNCPU 的启示：

| Gemmini 设计 | RSNCPU 对应 |
|-------------|------------|
| Tile 间 `ShiftRegister(data, 1)` | RSNCPU **流水线阶段之间的寄存器**（IF/ID 寄存器、ID/EX 寄存器等） |
| 数据在 Tile 间逐拍传递 | 指令在流水线阶段间逐拍推进 |
| `pipe(valid, data, latency)` 的 valid 门控 | 流水线气泡/暂停时冻结寄存器（类似 stall 信号） |

**关键复用点**：Mesh 的 `pipe` 函数机制可以直接作为 RSNCPU CPU 模式下流水线寄存器的实现模板。当 `valid=false` 时数据不更新，等价于流水线暂停（stall）。

### 6.2 二维互连拓扑的启示

| 特性 | Gemmini Mesh | RSNCPU 阵列 |
|------|-------------|------------|
| 水平方向 | a（激活值）左→右 | **行 CPU 模式**：指令从左→右流水 |
| 垂直方向 | b（部分和）上→下 | **列 CPU 模式**：指令从上→下流水 |
| 两个方向同时活跃 | 是（a 和 b 同时流动） | 是（混合模式下行和列可同时工作） |
| 信号链式传递 | `foldLeft` 模式 | 可直接复用同一模式 |

### 6.3 tree_reduction 与 RSNCPU

`tree_reduction` 在 Gemmini 中用于优化部分和累加的关键路径。RSNCPU 在 DNN 模式下如果保留这个选项，可以在面积和频率之间权衡：
- **无 tree_reduction**：面积小，但关键路径 = N 个加法器串联（频率低）
- **有 tree_reduction**：面积大（多加法器），但关键路径 = log₂N 个加法器（频率高）

对 FPGA（ZCU102 目标 200MHz）来说，tree_reduction 可能是达到目标频率的关键手段。

### 6.4 Tile 的粒度选择

默认 `tileRows=1, tileColumns=1` 意味着每个 Tile 就是一个 PE。对 RSNCPU 来说：
- 如果保持这个配置，那么 10×10 阵列的每对相邻 PE 之间都有寄存器 → **时序好，但寄存器多**
- 如果设 `tileRows=2, tileColumns=2`，则每 4 个 PE 内部直连，组间才有寄存器 → **寄存器少，但组合路径长**

建议 RSNCPU 初期保持 `1×1` 配置，先保证时序收敛，后期再优化面积。

---

## 7. 关键语法总结

| 语法 | 位置 | 含义 |
|------|------|------|
| `Seq.fill(rows, cols)(Module(...))` | Tile, Mesh | 创建二维模块数组 |
| `.transpose` | Tile, Mesh | 行列转置，方便按列遍历 |
| `.foldLeft(init) { case (acc, elem) => ... }` | 全局 | 链式信号连接模式 |
| `ShiftRegister(data, n)` | Mesh | 插入 n 级延迟寄存器（无门控） |
| `Pipe(valid, data, n)` | Mesh | 插入 n 级延迟寄存器（有 valid 门控） |
| `Vec(n, type)` | 端口定义 | n 个同类型信号组成的向量 |
| `withReset(false.B) { ... }` | Mesh.pipe | 局部禁用复位信号 |
| `.padTo(n, zero)` | accumulateTree | 序列补齐到长度 n |
| `.grouped(2)` | accumulateTree | 序列两两分组 |

---

## 8. 待后续精读时解答的问题

### Q3（延续 PE.scala）: Scratchpad 的 bank 寻址机制与 AOMEM 三角色切换有何异同？

→ 留待 `Scratchpad.scala` 精读时解答。

### Q4: Mesh 的 `tile_latency` 参数如何影响整体吞吐？

`tile_latency` 决定了 Tile 之间的流水线深度。增大 `tile_latency`（更深流水线）可以提高频率，但增加了矩阵乘法的启动延迟（填充时间）。对 RSNCPU 的 CPU 模式尤其重要——5 级流水线的分支惩罚 = 流水线深度。

### Q5: RSNCPU 的行/列 CPU 切换如何利用 Mesh 的二维互连？

Mesh 的 `in_a` 走水平、`in_b` 走垂直的物理布线可以直接复用：
- 行 CPU 模式：指令通过 `a` 端口从左→右流水（对应已有的水平布线）
- 列 CPU 模式：指令通过 `b` 端口从上→下流水（对应已有的垂直布线）
- 切换模式时只需 MUX 选择数据源，物理布线不变

---

*笔记版本：v1.0*
*前置笔记：`shen_PE_scala_explain.md`*
*后续笔记：`shen_Scratchpad_scala_explain.md`（待撰写）*
