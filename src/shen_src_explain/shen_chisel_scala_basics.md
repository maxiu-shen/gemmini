# Chisel/Scala 最小知识集（面向 Gemmini RTL 阅读）

> 本文档不讲完整语言，只讲**读懂 Gemmini 所需的最小语法集合**。

---

## 1. Scala 基础

```scala
// Scala 运行在 JVM 上，Chisel 是它的一个库
// 可以把 Chisel 理解为：用 Scala 程序来"生成"Verilog RTL

// val = 不可变绑定（类似 C 的 const），Scala/Chisel 中几乎全用 val
val x = 5

// class = 类定义，Scala 构造参数写在类名后面
class MyModule(width: Int) extends Module { ... }
// 等价于 Verilog: module MyModule #(parameter WIDTH=...) (...)

// Seq = 序列（类似数组），Seq.fill(n)(expr) = 创建 n 个 expr
val arr = Seq.fill(4)(Module(new PE(...)))  // 创建 4 个 PE 实例

// for (i <- 0 until n) = for 循环，0 到 n-1
// .map / .foldLeft / .reduce = 函数式迭代（Tile.scala 和 Mesh.scala 中大量使用）
```

### 1.1 `class` / `object` / `val` / `def` 的最小直觉

这几个概念在阅读 `LocalAddr.scala` 时非常容易混在一起，先用一个最小例子说明：

```scala
class A {
  val x = 3
  def y(dummy: Int = 0) = x + 1
}

object A {
  def make() = new A
}
```

可以这样理解：

- `class A`：定义一个类，必须先创建**实例对象**才能访问其中的字段和方法
- `object A`：定义一个**单例对象**，可直接通过 `A.xxx` 访问
- `val x = 3`：定义一个字段/值，通常表示对象的属性
- `def y(...) = ...`：定义一个方法/函数，表示一次计算或解析操作

因此：

- `new A().x` 可以
- `new A().y()` 可以
- `A.make()` 可以
- `A.x` 不可以，因为 `x` 不在 `object A` 里，而在 `class A` 的实例里

### 1.2 用 `LocalAddr.scala` 里的例子理解

`LocalAddr.scala` 里有两部分：

```scala
class LocalAddr(...) extends Bundle {
  val spRows = sp_banks * sp_bank_entries
  def sp_bank(dummy: Int = 0) = ...
}

object LocalAddr {
  def cast_to_sp_addr(...) = ...
  def cast_to_acc_addr(...) = ...
}
```

这里要区分两类成员。

#### 属于 `class LocalAddr` 实例的成员

这些成员必须先有一个 `LocalAddr` 对象实例，才能访问：

```scala
val laddr = Wire(new LocalAddr(...))

laddr.spRows
laddr.sp_bank()
```

其中：

- `spRows` 用 `val`，因为它更像一个固定属性
- `sp_bank()` 用 `def`，因为它更像"从当前地址里解析出 bank 编号"的函数

#### 属于 `object LocalAddr` 的成员

这些成员定义在伴生对象里，可以直接通过 `LocalAddr.xxx` 调用：

```scala
LocalAddr.cast_to_sp_addr(...)
LocalAddr.cast_to_acc_addr(...)
LocalAddr.garbage_addr(...)
```

### 1.3 为什么 `LocalAddr.spRows` 不可以

因为 `spRows` 定义在：

```scala
class LocalAddr(...) extends Bundle {
  val spRows = ...
}
```

它是**实例字段**，不是 `object LocalAddr` 里的静态成员。

所以：

- `laddr.spRows` 可以
- `LocalAddr.spRows` 不可以

你可以先记住这个简单规则：

- `对象实例.xxx`：访问类实例里的字段/方法
- `类名.xxx`：访问同名 `object` 里的成员

### 1.4 为什么 `sp_bank` 写成 `def sp_bank(dummy: Int = 0)`

这是一个常见的小技巧。作者希望调用时写成：

```scala
laddr.sp_bank()
```

而不是：

```scala
laddr.sp_bank
```

但这个函数实际上并不需要真实输入，所以加了一个占位参数：

```scala
dummy: Int = 0
```

这样就既能保留函数调用的括号形式，又不用真的传参。

### 1.5 Scala 常用集合模式

这几种写法在 Gemmini 里出现非常频繁，尤其是 `Tile.scala` 和 `Mesh.scala`。

#### `map`

```scala
val outs = pes.map(_.io.out_b)
```

意思是：对序列里的每个元素做同一种操作，生成一个新序列。上面这句就是"把所有 PE 的 `out_b` 收集出来"。

#### `zip`

```scala
Seq(1, 2, 3) zip Seq("a", "b", "c")
// 结果：Seq((1, "a"), (2, "b"), (3, "c"))
```

`zip` 会把两个**等长序列按相同下标配对**。在硬件连接里，常用来把"左边端口"和"右边模块"一一对齐。

#### 连续 `zip` 会形成嵌套元组

```scala
val xs = Seq(1, 2)
val ys = Seq("a", "b")
val zs = Seq(true, false)

xs zip ys zip zs
// 结果：Seq(((1, "a"), true), ((2, "b"), false))
```

注意不是 `Seq((1, "a", true), ...)`，而是**嵌套元组**：

```scala
((x, y), z)
```

如果继续 `zip`，就会继续向外套：

```scala
((((a, b), c), d), e)
```

#### `for` 中的解构

```scala
for (((x, y), z) <- xs zip ys zip zs) {
  // 每一轮循环里，x/y/z 都被直接拆出来了
}
```

这里左边不是单个变量，而是**模式匹配解构**。意思是：把右边序列里的每个嵌套元组，按形状直接拆成 `x`、`y`、`z`。

Gemmini 里的典型写法：

```scala
for (((((((b, c), v), ctrl), id), last), tile) <- io.out_b zip io.out_c zip io.out_valid zip io.out_control zip io.out_id zip io.out_last zip mesh.last) {
  ...
}
```

它的含义就是：把多个输出端口和 `mesh.last` 这一排 Tile **按列一一对齐**，然后在循环体里直接使用 `b`、`c`、`v`、`ctrl`、`id`、`last`、`tile` 这些名字。

#### `foldLeft`

```scala
elements.foldLeft(io.input) { case (signal, elem) =>
  elem.io.input := signal
  elem.io.output
}
```

`foldLeft` 的意思是：把"上一个阶段的输出"作为"下一个阶段的输入"，从左到右一路串起来。

在 Gemmini 里：
- `zip` 负责把多个序列**按位置对齐**
- `for` / `case` 负责把嵌套元组**拆开**
- `foldLeft` 负责把一串模块**链式连接**

---

## 2. Chisel 硬件原语（对照 Verilog）

| Chisel | Verilog 等价 | 说明 |
|--------|-------------|------|
| `val x = Wire(UInt(8.W))` | `wire [7:0] x` | 组合逻辑线网 |
| `val r = Reg(UInt(8.W))` | `reg [7:0] r` | 时序寄存器 |
| `val r = RegEnable(data, enable)` | `always @(posedge clk) if(en) r <= data` | 带使能的寄存器 |
| `x := y` | `assign x = y` 或 `x <= y` | 连接（组合或时序取决于左侧类型） |
| `Input(UInt(8.W))` | `input [7:0]` | 输入端口 |
| `Output(UInt(8.W))` | `output [7:0]` | 输出端口 |
| `Vec(4, UInt(8.W))` | `wire [7:0] arr [0:3]` | 向量（数组） |
| `Mux(cond, a, b)` | `cond ? a : b` | 多路选择器 |
| `when(cond) { } .otherwise { }` | `if ... else ...` | 条件赋值（生成 MUX 逻辑） |
| `ShiftRegister(x, n)` | n 级流水线寄存器 | 把信号延迟 n 拍 |
| `Module(new X(...))` | 实例化模块 X | |
| `io.xxx` | 模块端口访问 | |

---

## 3. 类型参数 `[T <: Data]`

```scala
class PE[T <: Data](inputType: T, ...)
```

表示 PE 是一个**泛型模块**——`T` 可以是 `SInt`（定点）、`UInt`、`Float` 等任意 Chisel 数据类型。这样同一套 PE 代码可以同时支持 INT8 和浮点运算，编译时根据配置选择具体类型。

---

## 4. `implicit ev: Arithmetic[T]`

这是 Scala 的**类型类（Type Class）**模式。`Arithmetic[T]` 在 `Arithmetic.scala` 中定义了 `+`、`*`、`mac`（乘累加）等运算。意思是：只要你的类型 `T` 实现了 `Arithmetic` 接口，PE 就能对它做加法和乘法。

暂时只需知道 `.mac(a, b)` = `a * b + self` 即可。

---

## 5. `extends Bundle` vs `extends Module`

| 基类 | 作用 | 类比 |
|------|------|------|
| `extends Bundle` | 信号束（多个信号打包） | SystemVerilog 的 `struct` |
| `extends Module` | 硬件模块（会生成 Verilog module） | Verilog 的 `module` |

---

## 6. `import ev._`

```scala
class MacUnit[T <: Data](...)(implicit ev: Arithmetic[T]) extends Module {
  import ev._
  // 现在可以直接在 T 类型上调用 .mac()、.+() 等方法
}
```

把 `ev`（即 `Arithmetic[T]` 实例）的所有方法导入当前作用域。如果不写这行，`io.in_c.mac(io.in_a, io.in_b)` 会编译报错，因为 Chisel 的原生 `Data` 类型没有 `mac` 方法。

---

## 7. `UInt(1.W)` 中的 `.W`

`.W` 是 Chisel 的位宽标记方法：
- `8.W` → 8 位宽
- `1.W` → 1 位宽
- `UInt(8.W)` → 8-bit 无符号整数
- `SInt(32.W)` → 32-bit 有符号整数

### 7.1 `i.U` 中的 `.U`

`.U` 是 Chisel 的**无符号字面量转换**语法。它的作用是把一个 Scala 里的整数常量，转成硬件里的 `UInt` 常量。

- `0.U` → 硬件中的无符号常量 0
- `1.U` → 硬件中的无符号常量 1
- `i.U` → 如果 `i` 是 Scala `Int`，就把它转成 `UInt`

最常见的场景是 `for` 循环里的下标：

```scala
for (i <- 0 until 4) {
  when(bankSel === i.U) {
    ...
  }
}
```

这里的 `i` 本来只是 Scala 软件层面的循环变量，不是硬件信号；写成 `i.U` 之后，才变成可以和 `bankSel` 这种硬件 `UInt` 信号比较的常量。

你可以把它粗略理解为：

- `i` = Scala/软件世界里的整数
- `i.U` = Chisel/硬件世界里的无符号常量

还要注意 `.U` 和 `.W` 的区别：

- `.W` 表示"位宽"
- `.U` 表示"把数值变成 `UInt` 常量"

例如：

```scala
UInt(8.W)   // 类型：8 位宽的 UInt
3.U         // 值：无符号常量 3，位宽由 Chisel 推断
3.U(8.W)    // 值：8 位宽的无符号常量 3
(-1).S      // 有符号常量 -1
```

Gemmini 里你会经常见到：

```scala
laddr.sp_bank() === i.U
0.U
1.U
```

本质上都是在写**硬件常量**，而不是普通 Scala 整数。

---

## 8. 上下文界定语法糖 `[T <: Data : Arithmetic]`

```scala
class PEControl[T <: Data : Arithmetic](accType: T)
```

其中 `: Arithmetic` 是以下写法的**语法糖**：

```scala
class PEControl[T <: Data](accType: T)(implicit ev: Arithmetic[T])
```

意思是"T 必须是 Data 的子类，且必须有一个 `Arithmetic[T]` 的隐式实例可用"。

---

## 9. 柯里化（多参数列表）

```scala
class MacUnit[T <: Data](inputType: T, weightType: T, cType: T, dType: T)
                        (implicit ev: Arithmetic[T])
```

Scala 允许一个类/函数有**多个参数列表**（用多组括号）。第二个括号里的 `implicit` 参数由编译器**自动查找并传入**，调用时不需要手动写。

---

## 10. Chisel 的 `IO(new Bundle { ... })`

每个 `Module` 必须有一个 `io`，通过 `IO(new Bundle { ... })` 声明端口列表：

```scala
val io = IO(new Bundle {
  val in_a  = Input(UInt(8.W))    // 输入端口
  val out_d = Output(UInt(32.W))  // 输出端口
})
```

等价于 Verilog：
```verilog
module MyModule(
  input  [7:0]  in_a,
  output [31:0] out_d
);
```

---

## 11. 自己问过的问题

### 11.1 类内部访问自身成员时，通常省略的是 `this.`

在 Scala 类内部，访问自己的字段和方法时，通常可以省略 `this.`：

```scala
class A {
  val x = 3
  def f(y: Int) = x + g(y)
  def g(z: Int) = z + 1
}
```

这里的：

- `x` 等价于 `this.x`
- `g(y)` 等价于 `this.g(y)`

Scala 默认关键字是 `this`，不是 Python/C++ 风格的 `self`。只有你显式写：

```scala
class A { self =>
  ...
}
```

时，`self` 才是你手动给 `this` 起的别名。

### 11.2 Scala 里的运算符本质上也是方法

Scala 中：

```scala
a < b
```

本质上等价于：

```scala
a.<(b)
```

因此：

```scala
def <(other: LocalAddr) = ...
```

可以直接理解成 Scala 版本的运算符重载，效果上类似 C++ 的：

```cpp
bool operator<(const LocalAddr& other) const;
```

同理：

- `a + b` 等价于 `a.+(b)`
- `a > b` 等价于 `a.>(b)`
- `a <= b` 等价于 `a.<=(b)`

### 11.3 Scala 方法默认返回最后一个表达式

如果不显式写 `return`，Scala 方法的返回值默认就是**方法体最后一个表达式**的值。

例如：

```scala
def f(x: Int) = x + 1
```

等价于：

```scala
def f(x: Int): Int = {
  x + 1
}
```

如果最后一个表达式没有实际返回值语义，则返回类型通常是 `Unit`，类似 C/C++ 的 `void`。

### 11.4 `Bundle` 里写 `val data = UInt(8.W)` 不是在创建 `Wire`

在 `Bundle` 里：

```scala
class MyBundle extends Bundle {
  val data = UInt(8.W)
}
```

这里只是在声明：

- `MyBundle` 里有个字段 `data`
- 它的类型是 8-bit `UInt`

这更像定义结构体字段，而不是在模块内部创建一根独立的线网。

只有在 `Module` 里写：

```scala
val data = Wire(UInt(8.W))
```

才是在真正创建一根组合逻辑线。

### 11.5 `UInt(1.W)` 这种写法只定义形状，不定义数值

例如：

```scala
val garbage_bit = UInt(1.W)
```

它的意思是：

- 定义一个 1-bit 的 `UInt` 字段

它**没有在这里被赋具体数值**。真正的值是在后续用 `:=` 连接时才指定，例如：

```scala
garbage_bit := 1.U
```

所以要区分：

- `UInt(1.W)`：定义类型/位宽
- `1.U`：定义一个具体常量值

### 11.6 `Wire` 和 `WireInit` 的区别

最简洁地说：

- `Wire(...)`：创建一根线
- `WireInit(...)`：创建一根线，并立即给它一个默认连接

例子：

```scala
val x = Wire(UInt(8.W))
val y = WireInit(0.U(8.W))
```

其中：

- `x` 只有类型，还没有默认驱动
- `y` 有类型，并默认先连到 `0`

### 11.7 `Wire` 更像吃“类型模板”，`WireInit` 更像吃“已有值/原型”

可以先粗略记成：

- `Wire(arg)`：`arg` 主要提供类型形状
- `WireInit(init)`：`init` 同时提供类型形状和默认值来源

例如：

```scala
Wire(UInt(8.W))
WireInit(0.U(8.W))
Wire(chiselTypeOf(this))
WireInit(this)
```

其中：

- `Wire(chiselTypeOf(this))`：按 `this` 的类型形状新建线
- `WireInit(this)`：按 `this` 的类型形状新建线，并默认复制 `this` 的当前值

不建议把 `Wire(this)` 当常规写法。若想复制类型，通常写：

```scala
Wire(chiselTypeOf(this))
```

若想复制类型并默认拷贝值，通常写：

```scala
WireInit(this)
```

### 11.8 为什么新建对象后常常先给默认驱动

如果你新建了一个 `Bundle` / `Wire`，后面又只打算改其中一部分字段，那么这个对象通常需要先有一个默认驱动，否则其余字段可能未被驱动。

常见两种做法：

```scala
val result = WireInit(this)   // 先整体默认复制
```

或者：

```scala
val result = Wire(chiselTypeOf(this))
result := DontCare            // 先整体标记为不关心
```

然后再只覆盖关键字段。

### 11.9 `DontCare` 是“我不关心这个值”，不是 0

例如：

```scala
result := DontCare
```

意思是：

- 先把整个对象默认标成“值无所谓”
- 后面再覆盖真正关心的字段

它不是赋 0，也不是赋随机数，而是告诉编译器/综合器：

**这些位如果最终不影响功能，可以自由优化。**

### 11.10 “没有赋值”不等于“会被优化掉”

要区分两种情况：

1. 字段被显式标成 `DontCare`，且最终确实无人使用  
   这类位可能在综合时被优化掉。

2. 字段本该被使用，但你忘了赋值  
   这通常是错误或不完整连接，不应指望综合器帮你“自动处理”。

所以：

- `无人使用 + DontCare` -> 可能被优化掉
- `忘了赋值` -> 更可能是危险信号

### 11.11 `asTypeOf` 和 `chiselTypeOf` 的最小直觉

这两个在 Gemmini 中非常常见。

#### `asTypeOf(proto)`

表示：

**把一串 bit 按 `proto` 的类型布局重新解释。**

例如：

```scala
t.asTypeOf(local_addr_t)
```

不是“做运行时类型检查”，而是“按 `local_addr_t` 这个原型对象的字段布局来拆位”。

#### `chiselTypeOf(proto)`

表示：

**拿到 `proto` 的类型形状，用来创建同类型的新对象。**

例如：

```scala
Wire(chiselTypeOf(local_addr_t))
```

意思是按 `local_addr_t` 的类型形状新建一根线，但不复制它当前的值。
