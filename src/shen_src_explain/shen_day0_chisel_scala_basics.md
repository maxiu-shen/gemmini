# Chisel/Scala 最小知识集（面向 Gemmini RTL 阅读）

> 本文档不讲完整语言，只讲**读懂 Gemmini 所需的最小语法集合**。

---

## 整体认知框架

Chisel 代码中混合了两个层次的语义，阅读时必须区分：

| 层次 | 角色 | 对应本文章节 | elaboration 后 |
|------|------|-------------|---------------|
| **Chisel 硬件描述** | 描述**生成什么硬件**——Wire、Reg、when、Bundle、Decoupled 等 | 第一节、第二节 | 变成 Verilog RTL |
| **Scala 元编程** | 控制**怎么生成硬件**——for 循环展开 N 个 PE、if 按配置裁剪电路、map/zip 批量连线 | 第三节 | 全部消失，只留下它们生成的硬件 |

一个直观的例子：

```scala
// Scala 层：for 循环在 elaboration 时展开，生成 4 份硬件
for (i <- 0 until 4) {
  // Chisel 层：每份硬件中包含一个运行时 MUX
  when(bankSel === i.U) {
    out := banks(i)
  }
}
```

elaboration 结束后，`for` 消失，留下 4 个并行的 `when` MUX 电路。

---

## 一、Chisel ↔ Verilog 直接对照

### 1.1 信号声明

| Chisel | Verilog | 说明 |
|--------|---------|------|
| `val x = Wire(UInt(8.W))` | `wire [7:0] x` | 组合逻辑线网 |
| `val r = Reg(UInt(8.W))` | `reg [7:0] r` | 时序寄存器 |
| `val r = RegEnable(data, en)` | `always @(posedge clk) if(en) r <= data` | 带使能寄存器 |
| `x := y` | `assign x = y` 或 `x <= y` | 取决于左侧类型 |
| `Vec(4, UInt(8.W))` | `wire [7:0] arr [0:3]` | 向量（数组） |
| `Reg(Vec(n, t))` | `reg [W-1:0] arr [0:n-1]` | 寄存器数组 |

### 1.2 位宽与字面量

| Chisel | 含义 |
|--------|------|
| `8.W` | 位宽 8 |
| `UInt(8.W)` | 8-bit 无符号整数类型 |
| `SInt(32.W)` | 32-bit 有符号整数类型 |
| `3.U` | 无符号常量 3（位宽自动推断） |
| `3.U(8.W)` | 8-bit 无符号常量 3 |
| `(-1).S` | 有符号常量 -1 |

注意区分：`UInt(8.W)` 定义**类型/位宽**，`3.U` 定义**具体常量值**。

`i.U` 用于将 Scala `Int` 转为硬件 `UInt`，常见于循环中与硬件信号比较：

```scala
for (i <- 0 until 4) {
  when(bankSel === i.U) { ... }  // i 是 Scala Int，i.U 转为硬件常量
}
```

### 1.3 端口与模块

| Chisel | Verilog |
|--------|---------|
| `Input(UInt(8.W))` | `input [7:0]` |
| `Output(UInt(8.W))` | `output [7:0]` |
| `Module(new X(...))` | 实例化模块 X |

构造参数类比 Verilog 的 `parameter`：

```scala
class MyModule(width: Int) extends Module { ... }
// 等价于 Verilog: module MyModule #(parameter WIDTH=...) (...)
```

端口通过 `IO(new Bundle { ... })` 声明：

```scala
val io = IO(new Bundle {
  val in_a  = Input(UInt(8.W))
  val out_d = Output(UInt(32.W))
})
// 等价于 Verilog:
// module MyModule(input [7:0] in_a, output [31:0] out_d);
```

`extends Bundle` vs `extends Module`：

| 基类 | 作用 | 类比 |
|------|------|------|
| `extends Bundle` | 信号束（多个信号打包） | SystemVerilog `struct` |
| `extends Module` | 硬件模块（生成 Verilog module） | Verilog `module` |

### 1.4 条件与选择

| Chisel/Scala | Verilog 等价 | 性质 |
|---|---|---|
| `when / .elsewhen / .otherwise` | `always` 块内 `if / else if / else` | **运行时**硬件条件，综合出 MUX |
| Scala `if` | `` `generate if` `` | **编译期**裁剪，只有一条路径进入硬件 |
| `switch / is` | `case` | 运行时多路选择 |
| `Mux(cond, a, b)` | `cond ? a : b` | 三元选择器 |
| `ShiftRegister(x, n)` | n 级流水线寄存器 | 延迟 n 拍 |

**判断用哪个**：看条件的类型。Chisel `Bool`（硬件信号）→ `when`；Scala `Boolean`/`Int`（编译期常量）→ `if`。

> 注意：同名变量可能在不同作用域有不同类型。例如 Entry 类内 `val q = UInt(2.W)` 是硬件字段，但外层 `for (q <- Seq(ldq, exq, stq))` 的循环变量 `q` 是 Scala `Int`，会**遮蔽**（shadow）硬件字段。

---

## 二、Chisel 特有机制（Verilog 无直接对应）

### 2.1 `Bundle` — 信号束

类比 SystemVerilog 的 `struct`，打包多个信号：

```scala
class MyBundle extends Bundle {
  val data = UInt(8.W)    // 声明字段类型，不是创建 Wire
}
```

注意：`Bundle` 内 `val data = UInt(8.W)` 只定义**类型形状**，不创建硬件。只有在 `Module` 中 `Wire(new MyBundle)` 才创建实际线网。

### 2.2 `Wire` / `WireInit` / `DontCare`

| 写法 | 含义 |
|------|------|
| `Wire(UInt(8.W))` | 创建线网，无默认驱动 |
| `WireInit(0.U(8.W))` | 创建线网，默认连到 0 |
| `Wire(chiselTypeOf(x))` | 按 x 的类型新建线网 |
| `WireInit(x)` | 按 x 的类型新建线网，并复制 x 的值 |
| `result := DontCare` | 标记为"值无所谓"，综合器可自由优化（不是赋 0） |

区分：`Wire(arg)` 吃**类型模板**，`WireInit(init)` 吃**已有值/原型**（同时提供类型和默认值）。

新建 Bundle 后通常先给默认驱动，再覆盖关键字段（否则未驱动字段会报错）：

```scala
val result = WireInit(this)          // 整体默认复制
// 或
val result = Wire(chiselTypeOf(this))
result := DontCare                   // 整体标记不关心
```

### 2.3 `Decoupled` 与 `.fire`

Chisel 标准握手接口，`.fire` = `valid && ready`，表示该拍握手成功、数据有效传输。

### 2.4 `asTypeOf` / `chiselTypeOf`

| 写法 | 含义 |
|------|------|
| `t.asTypeOf(proto)` | 把一串 bit 按 proto 的字段布局**重新解释**（类似 C 的 union cast） |
| `chiselTypeOf(proto)` | 拿到 proto 的类型形状，用于创建同类型新对象 |

---

## 三、Scala 语言基础（硬件生成的元编程框架）

> Scala 代码在 elaboration 阶段执行完毕后全部消失，只留下它生成的硬件电路。

### 3.1 `val` / `def`

```scala
val x = 5              // 不可变绑定（类似 const），几乎全用 val
def f(y: Int) = y + 1  // 方法定义，默认返回最后一个表达式（无需 return）
for (i <- 0 until n)   // for 循环，0 到 n-1
Seq.fill(4)(Module(new PE(...)))  // 创建 4 个 PE 实例
```

- `val` 不一定是编译期常量，只是名字不能再指向别的东西
- 省略类型时由编译器自动推断
- 构造参数前加 `val` 自动成为成员字段：`class Gemmini(val config: ...)` 里的 `config` 可在类内直接使用

### 3.2 `class` / `object`

```scala
class A {
  val x = 3
  def y(dummy: Int = 0) = x + 1
}

object A {
  def make() = new A
  def apply(x: Int) = x + 1   // A(3) 等价于 A.apply(3)
}
```

- `class A`：必须 `new A()` 创建实例后才能访问字段/方法
- `object A`：**单例对象**，直接 `A.xxx` 访问
- 同名的 `class A` 和 `object A` 叫**伴生类/伴生对象**
- `object A` 中定义 `apply(...)` 后可直接写 `A(...)`，所以 `LoopConv(raw_cmd, ...)` 实际调用的是 `object LoopConv` 的 `apply`

访问规则：
- `new A().x` ✓（实例字段）
- `A.make()` ✓（单例方法）
- `A.x` ✗（`x` 在 `class A`，不在 `object A`）

Gemmini 实例（`LocalAddr.scala`）：

```scala
class LocalAddr(...) extends Bundle {
  val spRows = sp_banks * sp_bank_entries   // 实例字段：laddr.spRows ✓
  def sp_bank(dummy: Int = 0) = ...         // 实例方法：laddr.sp_bank() ✓
}
object LocalAddr {
  def cast_to_sp_addr(...) = ...            // 静态方法：LocalAddr.cast_to_sp_addr(...) ✓
}
// LocalAddr.spRows ✗ — spRows 在 class 里，不在 object 里
```

### 3.3 运算符即方法

```scala
a < b    // 等价于 a.<(b)
a + b    // 等价于 a.+(b)
```

`def <(other: LocalAddr) = ...` 就是运算符重载，类似 C++ 的 `bool operator<(...) const`。

### 3.4 `this.` 省略

类内部访问自身成员时省略 `this.`：

```scala
class A {
  val x = 3
  def f(y: Int) = x + g(y)   // x = this.x, g = this.g
  def g(z: Int) = z + 1
}
```

Scala 默认关键字是 `this`。`class A { self => ... }` 中的 `self` 是手动给 `this` 起的别名。

### 3.5 集合操作

Gemmini 中高频出现的模式：

#### `map` — 逐元素变换

```scala
val outs = pes.map(_.io.out_b)   // 收集所有 PE 的 out_b
```

#### `zip` — 按位置配对

```scala
Seq(1, 2) zip Seq("a", "b")   // → Seq((1, "a"), (2, "b"))
```

连续 `zip` 形成嵌套元组：`xs zip ys zip zs` → `((x, y), z)`

#### `zipWithIndex` — 带索引遍历

```scala
entries.zipWithIndex.foreach { case (e, i) =>
  when(issue_sel(i)) { ... }
}
// 等价于 for (i <- 0 until entries.length) { val e = entries(i); ... }
```

#### `for` 中的解构

```scala
for (((x, y), z) <- xs zip ys zip zs) { ... }
```

Gemmini 典型写法：

```scala
for (((((((b, c), v), ctrl), id), last), tile) <- io.out_b zip io.out_c zip ...) { ... }
```

#### `foldLeft` — 链式连接

```scala
elements.foldLeft(io.input) { case (signal, elem) =>
  elem.io.input := signal
  elem.io.output           // 上一阶段的输出 → 下一阶段的输入
}
```

#### 元组 + `foreach` 统一处理

```scala
Seq((ldq, entries_ld), (exq, entries_ex), (stq, entries_st))
  .foreach { case (q_, entries_type_) =>
    // 每轮拿到队列类型和对应数组，一段逻辑统一处理三个队列
  }
```

### 3.6 类型参数与隐式

```scala
class PE[T <: Data](inputType: T, ...)(implicit ev: Arithmetic[T])
```

- `[T <: Data]`：泛型，T 可以是 `SInt`/`UInt`/`Float` 等，同一套代码支持多种数据类型
- `implicit ev: Arithmetic[T]`：类型类模式，要求 T 实现 `+`/`*`/`mac` 等运算
- `import ev._`：将运算方法导入作用域，之后可直接写 `.mac(a, b)`（`mac(a, b)` = `a * b + self`）
- `[T <: Data : Arithmetic]` 是上面的语法糖

柯里化（多参数列表）：第二个 `()` 里的 `implicit` 参数由编译器自动查找传入。

### 3.7 dummy 参数技巧

```scala
def sp_bank(dummy: Int = 0) = ...
```

函数不需要输入，但作者希望调用时写 `laddr.sp_bank()` 而非 `laddr.sp_bank`，加占位参数保留括号形式。
