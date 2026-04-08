# LocalAddr.scala 精读笔记

> **文件路径**：`src/main/scala/gemmini/LocalAddr.scala`
> **阅读定位**：这是 Gemmini 片上存储系统的语义地址定义文件，不只是"地址格式"，而是把**存储空间选择 + 访问模式 + bank/row 寻址**统一编码到一个 `Bundle` 中。
> **RSNCPU 关注点**：学习 Gemmini 如何用一个统一的本地地址描述符，携带 Scratchpad / Accumulator 选择、累加模式、整行读取模式以及 banked memory 的寻址语义，为后续 AOMEM 设计提供参考。

---

## 1. 先给结论：`LocalAddr` 不是普通地址

`LocalAddr` 表面上是在定义 Gemmini 的"本地地址"，但它本质上并不是一个纯 `UInt` 地址，而是一个**带控制语义的地址描述符**。

它至少同时回答了以下几个问题：

1. 这次访问的是 `scratchpad` 还是 `accumulator`
2. 如果访问 accumulator，是**覆盖写**还是**累加写**
3. 读 accumulator 时，是读缩窄后的结果，还是读全精度整行
4. 这次访问是否携带归一化/后处理命令

所以可以先把它记成：

```text
LocalAddr = 存储空间选择 + 访问模式位 + bank/row 地址
```

这对后面阅读 `Scratchpad.scala` 非常关键，因为 Gemmini 的 bank 路由、DMA 读写、冒险检测，都会围绕 `LocalAddr` 展开。

---

## 2. 文件主体结构

`LocalAddr.scala` 由两部分组成：

| 部分 | 作用 |
|------|------|
| `class LocalAddr(...) extends Bundle` | 定义地址的字段、解析函数和比较/加减操作 |
| `object LocalAddr` | 提供几个常用的构造/转换辅助函数 |

对应源码：

```scala
class LocalAddr(sp_banks: Int, sp_bank_entries: Int, acc_banks: Int, acc_bank_entries: Int) extends Bundle { ... }

object LocalAddr { ... }
```

---

## 3. 类头：为什么需要参数化

源码开头：

```scala
class LocalAddr(sp_banks: Int, sp_bank_entries: Int, acc_banks: Int, acc_bank_entries: Int) extends Bundle {
  private val localAddrBits = 32
```

这里的 4 个参数都与片上存储组织有关：

- `sp_banks`：scratchpad bank 数量
- `sp_bank_entries`：每个 scratchpad bank 有多少行
- `acc_banks`：accumulator bank 数量
- `acc_bank_entries`：每个 accumulator bank 有多少行

为什么 `LocalAddr` 需要知道这些？

因为它必须根据 bank 数和每个 bank 的行数，来计算：

- 总地址位宽需要多少位
- bank 编号占多少位
- bank 内 row 编号占多少位

也就是说，`LocalAddr` 并不是写死某种地址格式，而是**随 Gemmini 配置自动推导**。

---

## 4. 地址位宽的推导

先看这一段：

```scala
private val spAddrBits = log2Ceil(sp_banks * sp_bank_entries)
private val accAddrBits = log2Ceil(acc_banks * acc_bank_entries)
private val maxAddrBits = spAddrBits max accAddrBits

private val spBankBits = log2Up(sp_banks)
private val spBankRowBits = log2Up(sp_bank_entries)

private val accBankBits = log2Up(acc_banks)
val accBankRowBits = log2Up(acc_bank_entries)
```

这里分成两层理解。

### 4.1 第一层：整个本地空间需要多少地址位

- `spAddrBits`：编码整个 scratchpad 空间所需位数
- `accAddrBits`：编码整个 accumulator 空间所需位数

例如，如果：

- `sp_banks = 4`
- `sp_bank_entries = 256`

那么 scratchpad 总行数就是 `4 x 256 = 1024` 行，需要：

```text
log2Ceil(1024) = 10 bit
```

### 4.2 第二层：bank 和 row 各占多少位

- `spBankBits = log2Up(sp_banks)`
- `spBankRowBits = log2Up(sp_bank_entries)`

如果还是上面的例子：

- 4 个 bank -> bank 号需要 2 bit
- 每个 bank 256 行 -> row 号需要 8 bit

所以一个 scratchpad 地址主体就可以理解为：

```text
[bank(2 bit) | row(8 bit)]
```

这就是典型的 **banked memory** 寻址方式。

---

## 5. 语义字段：Gemmini 为什么不用纯地址

继续看最关键的字段定义：

```scala
val is_acc_addr = Bool()
val accumulate = Bool()
val read_full_acc_row = Bool()
val norm_cmd = NormCmd()
```

这 4 个字段说明，Gemmini 的本地地址不只是"定位到哪一行"，还要告诉控制器：

### 5.1 `is_acc_addr`

表示目标存储空间是否是 accumulator：

- `false` -> 访问 scratchpad
- `true` -> 访问 accumulator

这是最基础的一位，因为 Gemmini 的片上存储并不是单一空间，而是分成了：

- `scratchpad`
- `accumulator`

### 5.2 `accumulate`

只有 accumulator 才真正关心这一位。

它表示写 accumulator 时，是：

- 直接写入
- 还是把新结果与已有值累加

这很符合 Gemmini 的定位，因为 accumulator 本来就承担部分和存储的角色。

### 5.3 `read_full_acc_row`

这一位表示读 accumulator 时，是否读取整行全精度数据。

这和 accumulator 的特殊性有关：它里边存的是高精度累加结果，而不是普通的输入激活。

### 5.4 `norm_cmd`

这表示这次访问还可能关联归一化/后处理命令。

所以 `LocalAddr` 不是单纯地址，而是：

**地址 + 访问语义 + 后处理语义**

---

## 6. `metadata_w`：为什么要算元数据总宽度

```scala
private val metadata_w = is_acc_addr.getWidth + accumulate.getWidth + read_full_acc_row.getWidth + norm_cmd.getWidth
assert(maxAddrBits + metadata_w < 32)
```

这段的意思是：

- 地址主体 `data`
- 加上这些模式字段
- 总共必须塞进 32 位本地地址格式中

也就是说，Gemmini 在架构上把 `LocalAddr` 约束为一个 32-bit 语义地址对象，方便：

- 指令打包
- 控制器队列传递
- 地址比较
- 冒险检测

这里要注意，`32 bit` 不是说片上 SRAM 物理地址一定有 32 位，而是说**控制路径里统一使用 32 位格式来表达本地地址语义**。

---

## 7. `data`：真正的地址主体

```scala
val data = UInt(maxAddrBits.W)
```

`data` 是真正承载 bank/row 信息的地址主体。

你可以这样理解：

- `is_acc_addr / accumulate / read_full_acc_row / norm_cmd` 决定"怎么访问"
- `data` 决定"访问哪一个 bank 的哪一行"

所以 `LocalAddr` 的结构是：

```text
LocalAddr
├─ 语义位：访问哪类存储、按什么方式访问
└─ data：bank + row 地址主体
```

---

## 8. banked memory 的解析函数

源码：

```scala
def sp_bank(dummy: Int = 0) = if (spAddrBits == spBankRowBits) 0.U else data(spAddrBits - 1, spBankRowBits)
def sp_row(dummy: Int = 0) = data(spBankRowBits - 1, 0)
def acc_bank(dummy: Int = 0) = if (accAddrBits == accBankRowBits) 0.U else data(accAddrBits - 1, accBankRowBits)
def acc_row(dummy: Int = 0) = data(accBankRowBits - 1, 0)

def full_sp_addr(dummy: Int = 0) = data(spAddrBits - 1, 0)
def full_acc_addr(dummy: Int = 0) = data(accAddrBits - 1, 0)
```

这是 `LocalAddr.scala` 的核心之一。

### 8.1 `sp_bank()` / `sp_row()`

表示把 `data` 拆成：

```text
[sp_bank | sp_row]
```

也就是：

- 高位 -> bank 号
- 低位 -> bank 内 row 号

### 8.2 `acc_bank()` / `acc_row()`

与 scratchpad 同理，只不过这里对应 accumulator。

### 8.3 为什么 `sp_bank()` 可能直接返回 `0.U`

```scala
if (spAddrBits == spBankRowBits) 0.U
```

这意味着：

- 如果 scratchpad 只有 1 个 bank
- 那么 bank 号根本不需要单独编码
- 整个地址主体都只是 row 地址

所以直接返回 bank 0。

这是为了兼容单 bank 配置。

### 8.4 这和后面的 `Scratchpad.scala` 有什么关系

后续你会频繁看到：

- `laddr.sp_bank() === i.U`
- `laddr.sp_row()`
- `laddr.acc_bank()`
- `laddr.acc_row()`

这说明 Gemmini 的 banked memory 路由流程就是：

1. 先用 `bank()` 选中对应 bank
2. 再把 `row()` 作为该 bank 的地址输入

---

## 9. 默认配置下一行到底多宽

这里顺手把一个常见疑问说明白。

在 Gemmini 默认整数配置中：

- `inputType = SInt(8.W)`
- `accType = SInt(32.W)`
- `meshColumns = 16`
- `tileColumns = 1`

因此：

- scratchpad 一行宽度 = `16 x 8 = 128 bit = 16 Byte`
- accumulator 一行宽度 = `16 x 32 = 512 bit = 64 Byte`

也就是说：

- scratchpad 一行能放 16 个 INT8 元素
- accumulator 一行能放 16 个 INT32 元素

所以这里的 `row` 不是"单个元素"，而是 **bank 内一整行宽数据**。

---

## 10. `is_same_address()`：为什么不比较全部字段

源码：

```scala
def is_same_address(other: LocalAddr): Bool = is_acc_addr === other.is_acc_addr && data === other.data
def is_same_address(other: UInt): Bool = is_same_address(other.asTypeOf(this))
```

这段很值得精读。

它比较的是：

- 两者是否都在同一类存储空间
- 两者的地址主体 `data` 是否相同

但它**不比较**：

- `accumulate`
- `read_full_acc_row`
- `norm_cmd`

### 为什么？

因为从存储冲突和冒险检测角度看，最核心的问题是：

> 这两个请求是不是命中了同一个片上存储位置？

而这件事只由以下两项决定：

1. 是不是都访问 scratchpad，或都访问 accumulator
2. 地址主体 `data` 是否相同

至于访问模式不同，例如一个是累加写、一个是普通写，那属于访问方式差异，而不是"是不是同一个存储位置"的第一层判断条件。

### 实际用途

`is_same_address()` 会被 `ExecuteController.scala` 用来做 hazard 检测。

所以你要把它理解为：

**地址冲突判定函数**

而不是"两个 `LocalAddr` 全字段完全相等"的函数。

### 不要和 `bank conflict` 混淆

这里要特别区分两类不同冲突：

- `bank conflict`：两个请求同一拍打到**同一个 bank**，属于端口/带宽冲突
- `same-address hazard`：两个请求命中**同一个精确存储位置**，属于数据相关冲突

因此：

- 只看同一个 bank，只能说明"可能争同一个 bank 端口"
- 比较 `is_acc_addr + data`，才能说明"是否真的是同一个地址"

`is_same_address()` 处理的是第二类问题，也就是**同地址冒险判断**，不是 bank 仲裁判断。

---

## 11. 比较运算符：为什么先判断是否同类地址空间

源码：

```scala
def <=(other: LocalAddr) =
  is_acc_addr === other.is_acc_addr &&
    Mux(is_acc_addr, full_acc_addr() <= other.full_acc_addr(), full_sp_addr() <= other.full_sp_addr())

def <(other: LocalAddr) =
  is_acc_addr === other.is_acc_addr &&
    Mux(is_acc_addr, full_acc_addr() < other.full_acc_addr(), full_sp_addr() < other.full_sp_addr())

def >(other: LocalAddr) =
  is_acc_addr === other.is_acc_addr &&
    Mux(is_acc_addr, full_acc_addr() > other.full_acc_addr(), full_sp_addr() > other.full_sp_addr())
```

这里很容易忽略一个细节：

在做 `<`、`<=`、`>` 比较之前，代码先检查：

```scala
is_acc_addr === other.is_acc_addr
```

为什么？

因为：

- scratchpad 地址空间
- accumulator 地址空间

本来就是两套不同的语义空间。它们之间直接比较大小没有实际意义。

所以这里的设计原则是：

**只有属于同一类片上存储空间的地址，才允许做大小比较。**

---

## 12. `+` 和 `add_with_overflow()`：为什么不是普通加法

先看 `+`：

```scala
def +(other: UInt) = {
  require(isPow2(sp_bank_entries))
  require(isPow2(acc_bank_entries))

  val result = WireInit(this)
  result.data := data + other
  result
}
```

这个函数只对 `data` 做加法，保留其他语义位不变。

也就是说，地址在向后移动时：

- 访问空间类别不变
- 累加模式不变
- `norm_cmd` 不变

只改变地址主体。

这很符合硬件控制流逻辑。

### `add_with_overflow()`

```scala
def add_with_overflow(other: UInt): Tuple2[LocalAddr, Bool] = {
  require(isPow2(sp_bank_entries))
  require(isPow2(acc_bank_entries))

  val sum = data +& other

  val overflow = Mux(is_acc_addr, sum(accAddrBits), sum(spAddrBits))

  val result = WireInit(this)
  result.data := sum(maxAddrBits - 1, 0)

  (result, overflow)
}
```

这里就不是简单加法了，而是：

1. 用 `+&` 做扩展位加法
2. 根据当前地址属于 scratchpad 还是 accumulator
3. 选取对应有效位上的溢出位

也就是说，它的语义是：

> 当前本地地址向后推进若干行时，会不会超出该本地空间的可编码范围？

这在：

- block 遍历
- 循环搬运
- reservation station 中的回绕判断

都很有用。

---

## 13. `floorSub()`：带下边界保护的减法

源码：

```scala
// This function can only be used with non-accumulator addresses. Returns both new address and underflow
def floorSub(other: UInt, floor: UInt): (LocalAddr, Bool) = {
  require(isPow2(sp_bank_entries))
  require(isPow2(acc_bank_entries))

  val underflow = data < (floor +& other)

  val result = WireInit(this)
  result.data := Mux(underflow, floor, data - other)

  (result, underflow)
}
```

这个函数不是普通减法，而是：

- 地址减去 `other`
- 如果结果会小于 `floor`
- 就直接钳在 `floor`

所以它的真实语义是：

**带下边界保护的地址回退**

### 为什么注释说只能用于 non-accumulator 地址

因为这类操作更适合 scratchpad 侧的数据布局和边界处理，例如：

- 输入特征图回退
- pixel repeat
- 滑窗边界限制

而 accumulator 更偏向部分和结果缓存，不太符合这种 floor 语义。

---

## 14. `garbage address`：为什么硬件系统要有"垃圾地址"

源码：

```scala
def is_garbage(dummy: Int = 0) = is_acc_addr && accumulate && read_full_acc_row && data.andR &&
  (if (garbage_bit.getWidth > 0) garbage_bit.asBool else true.B)
```

以及：

```scala
def make_this_garbage(dummy: Int = 0): Unit = {
  is_acc_addr := true.B
  accumulate := true.B
  read_full_acc_row := true.B
  garbage_bit := 1.U
  data := ~(0.U(maxAddrBits.W))
}
```

这里 Gemmini 专门保留了一个特殊编码，表示：

> 这不是一个真实有效的本地地址，而是一个占位符

### 它有什么用

在很多命令格式里，某个 operand 位置必须存在，但该拍实际上不需要访问真实存储。

这时如果专门为"无效地址"再设计一套独立控制分支，会让控制器更复杂。

Gemmini 的做法更巧妙：

- 仍然给它一个 `LocalAddr`
- 但这个 `LocalAddr` 被编码成 `garbage`
- 下游逻辑识别后忽略即可

这样做的工程好处是：

1. 统一命令格式
2. 少写特判逻辑
3. 更容易流水化和排队

---

## 15. `object LocalAddr`：4 个辅助构造函数

### 15.1 `cast_to_local_addr()`

源码：

```scala
def cast_to_local_addr[T <: Data](local_addr_t: LocalAddr, t: T): LocalAddr = {
  val result = WireInit(t.asTypeOf(local_addr_t))
  if (result.garbage_bit.getWidth > 0) result.garbage := 0.U
  result
}
```

作用是：

- 把某个原始 bit 向量解释成 `LocalAddr`
- 并且把没必要的垃圾位清零

这里体现了很强的工程意识：

不只是要逻辑正确，还要尽量避免无关位传播到后续组合逻辑中，减少额外位宽负担。

### 15.2 `cast_to_sp_addr()`

```scala
def cast_to_sp_addr[T <: Data](local_addr_t: LocalAddr, t: T): LocalAddr = {
  val result = WireInit(cast_to_local_addr(local_addr_t, t))
  result.is_acc_addr := false.B
  result.accumulate := false.B
  result.read_full_acc_row := false.B
  result
}
```

作用很直接：

- 把某个原始地址包装成 scratchpad 地址
- 明确指出这不是 accumulator 地址

### 15.3 `cast_to_acc_addr()`

```scala
def cast_to_acc_addr[T <: Data](local_addr_t: LocalAddr, t: T, accumulate: Bool, read_full: Bool): LocalAddr = {
  val result = WireInit(cast_to_local_addr(local_addr_t, t))
  result.is_acc_addr := true.B
  result.accumulate := accumulate
  result.read_full_acc_row := read_full
  result
}
```

它比 `cast_to_sp_addr()` 多了两层语义：

- 是否累加
- 是否读全宽 accumulator 行

这恰好说明 accumulator 的角色比 scratchpad 更复杂。

### 15.4 `garbage_addr()`

```scala
def garbage_addr(local_addr_t: LocalAddr): LocalAddr = {
  val result = Wire(chiselTypeOf(local_addr_t))
  result := DontCare
  result.make_this_garbage()
  result
}
```

作用是快速构造一个垃圾地址，占位用。

---

## 16. 实际用法怎么理解

### 16.1 同一个裸地址值，可以被包装成不同语义

例如在 `LoopMatmul.scala` 中，会看到：

```scala
mvin_cmd_rs2.local_addr := cast_to_sp_addr(mvin_cmd_rs2.local_addr, sp_addr)
```

也会看到：

```scala
mvin_cmd_rs2.local_addr := cast_to_acc_addr(mvin_cmd_rs2.local_addr, sp_addr, accumulate = false.B, read_full = false.B)
```

这说明：

- 裸地址值只是 bit 模式
- 真正决定其硬件语义的是外面的 `LocalAddr` 包装

### 16.2 `garbage_addr` 在控制器里很常见

某些命令拍不需要真实访问时，控制器会直接塞一个 `garbage_addr`，让流水线继续跑，而不是拆分成多个命令格式。

这对大规模流水控制非常实用。

---

## 17. 对 RSNCPU / AOMEM 的启发

这是你读这个文件时最该带走的部分。

### 17.1 启发一：地址和访问模式不必完全分离

Gemmini 的做法说明：

**片上存储访问语义，可以直接绑定在地址描述符中。**

这对 RSNCPU 很有参考意义，因为你未来的 AOMEM 也需要表达：

- 当前访问的是哪一类逻辑角色
- 是否是输入端口语义
- 是否是输出端口语义
- 是否是 CPU 数据缓存语义

### 17.2 启发二：banked memory 的寻址最好标准化

Gemmini 统一提供：

- `sp_bank / sp_row`
- `acc_bank / acc_row`

这让后续所有控制器都围绕同一套地址解析接口工作。

RSNCPU 如果未来做 AOMEM，也建议尽早固定：

- bank 编号怎么切
- row 编号怎么切
- 模式切换后哪些语义位保留，哪些改变

### 17.3 启发三：保留"无效占位地址"很有用

`garbage_addr` 这种做法对流水化控制器特别友好。

如果未来 RSNCPU 的模式切换、任务切换、AOMEM 多角色切换很复杂，保留统一格式的"空操作地址"很可能会大幅减少控制逻辑分叉。

---

## 18. 读完本文件后，你应该能回答的问题

### Q1：Gemmini 为什么不用一个纯 `UInt` 表示本地地址？

因为它不仅要表示"访问哪一行"，还要表示：

- 访问哪类存储空间
- 是否累加
- 是否读全宽 accumulator
- 是否带归一化命令

除此之外，还有一个很重要的工程原因：

- Gemmini 是可配置的，不同配置下 `sp_banks`、`sp_bank_entries`、`acc_banks`、`acc_bank_entries` 都可能变化
- 因此地址主体 `data` 的位宽、`bank`/`row` 的切分边界也会随配置变化

如果只用纯 `UInt`，那么代码库里很多地方都要手动写基于配置的位切片逻辑；而用 `LocalAddr` 这种 `Bundle`，就可以把这些随配置变化的字段布局封装起来，外部只通过：

- `sp_bank()`
- `sp_row()`
- `acc_bank()`
- `acc_row()`

这类统一接口访问，减少手写位切片和配置联动错误。

### Q2：Gemmini 的 banked memory 地址是怎么拆的？

通过 `data` 拆成：

- `bank`
- `row`

分别由：

- `sp_bank()` / `sp_row()`
- `acc_bank()` / `acc_row()`

来解析。

### Q3：`is_same_address()` 为什么不比较 `accumulate` 等字段？

因为它的目标是判断：

> 是否命中同一个存储位置

而不是判断两个访问命令是否全语义一致。

### Q4：`garbage_addr` 的工程意义是什么？

统一命令格式，减少空操作情况下的控制分支。

---

## 19. 一句话总结

`LocalAddr.scala` 的真正价值不在于它定义了一个地址类型，而在于它定义了 **Gemmini 片上存储访问的统一语义接口**：用一个 `Bundle` 同时携带存储空间选择、访问模式和 bank/row 地址，使后续的 DMA、Scratchpad、Accumulator、ReservationStation 和 ExecuteController 都能围绕同一套抽象工作。

---

## 20. 自己问过的问题

### 20.1 `is_same_address(other: UInt)` 不是“防止传错类型”，而是提供重载便利

源码：

```scala
def is_same_address(other: UInt): Bool = is_same_address(other.asTypeOf(this))
```

它的作用不是防止调用者传入 `UInt`，而是：

- 允许调用者直接传一个与 `LocalAddr` 位布局兼容的 `UInt`
- 先用 `asTypeOf(this)` 按 `LocalAddr` 布局解释
- 再复用真正的 `is_same_address(other: LocalAddr)` 比较逻辑

也就是说，它是一个**重载便利函数**。

### 20.2 `sum(accAddrBits)` 的作用

在：

```scala
val sum = data +& other
val overflow = Mux(is_acc_addr, sum(accAddrBits), sum(spAddrBits))
```

中，`+&` 会保留额外的进位位。

因此：

- `sum(accAddrBits)` 表示 accumulator 地址位宽之外的那一位
- 如果它为 `1`，说明 accumulator 地址加法发生了 overflow

所以它本质上是在取：

**“地址位宽之外的进位位”**

### 20.3 `garbage_bit` 和 `garbage` 的区别

它们不是同一种语义。

- `garbage_bit`：专门参与 `is_garbage()` 判定的标志位之一
- `garbage`：补齐 32 位布局时留下的保留/padding 字段

`garbage` 不是“记录哪些位是垃圾”的位图，它本身通常没有独立业务语义。

### 20.4 为什么 `cast_to_local_addr` 要传 `local_addr_t`

源码：

```scala
def cast_to_local_addr[T <: Data](local_addr_t: LocalAddr, t: T): LocalAddr = {
  val result = WireInit(t.asTypeOf(local_addr_t))
  ...
}
```

这里传 `local_addr_t` 的目的不是读取它的当前值，而是借它提供：

- `LocalAddr` 的具体字段布局
- 每个字段的具体位宽

因为 `LocalAddr` 是参数化 `Bundle`，不同配置下其内部位宽可能不同，不能只靠类名 `LocalAddr` 推断。

### 20.5 为什么这些辅助函数写在 `object LocalAddr` 里

`cast_to_local_addr`、`cast_to_sp_addr`、`cast_to_acc_addr`、`garbage_addr` 更像：

- 类型级别的构造/转换工具
- 围绕 `LocalAddr` 类型的工厂函数

而不是某个具体 `LocalAddr` 实例依赖“当前值”才能执行的成员行为。

因此写在 `object LocalAddr` 里更自然，也更像 Java/C++ 中的静态辅助函数。

### 20.6 使用 `LocalAddr` 时，通常不是手动逐字段赋值

实际使用里，一般分两步：

1. 先声明/实例化一个 `LocalAddr` 类型对象
2. 在需要生成具体地址语义时，优先调用辅助函数

例如：

```scala
cast_to_sp_addr(...)
cast_to_acc_addr(...)
garbage_addr(...)
```

这样比手动写：

```scala
x.is_acc_addr := ...
x.accumulate := ...
x.read_full_acc_row := ...
x.data := ...
```

更清晰，也更不容易漏字段。

### 20.7 `data` 取 `raw_addr` 的哪几位，是由 `Bundle` 布局自动决定的

当你写：

```scala
t.asTypeOf(local_addr_t)
```

时，Chisel 会按 `LocalAddr` 的 `Bundle` 字段顺序和位宽重新解释这串 bit。

在当前代码中，`data` 是最后声明的字段，因此它会对应 `raw_addr` 的**低位部分**；而 `is_acc_addr`、`accumulate`、`read_full_acc_row` 等语义字段在更高位。

所以并不需要手工再写“`data` 在第几位到第几位”。

### 20.8 `norm_cmd` 不是图片里的那 3 个经典高位之一

图片里描述的 3 个经典高位，对应的是：

- `is_acc_addr`
- `accumulate`
- `read_full_acc_row`

而：

```scala
val norm_cmd = NormCmd()
```

是额外加入的枚举语义字段，用于表达 layernorm / softmax 一类后处理命令。

所以：

- 图片描述的是 Gemmini 私有地址里最经典的 3 个特殊位
- 当前代码在此基础上又扩展了 `norm_cmd` 与 `garbage` 相关语义

### 20.9 为什么代码里是 `< 32` 而不是 `<= 32`

源码：

```scala
assert(maxAddrBits + metadata_w < 32)
```

这不是说 `LocalAddr` 不是 32 位，而是说：

- `data`
- 加上 `metadata_w`

不能把 32 位完全占满，因为代码还想额外预留至少 1 位给：

```scala
garbage_bit
```

并可能再留一些 `garbage` padding 位。

因此：

- 总体格式仍然是 32 位
- 但 `data + metadata` 必须严格小于 32

### 20.10 `data` 的最大位宽不是 28

因为：

```scala
metadata_w = is_acc_addr + accumulate + read_full_acc_row + norm_cmd
```

其中：

- 前 3 个字段共 3 bit
- `norm_cmd` 是枚举字段，不是 1 bit

在当前 `NormCmd` 有 8 个枚举值，因此通常需要 3 bit。

所以：

```text
metadata_w = 1 + 1 + 1 + 3 = 6 bit
```

再结合：

```scala
maxAddrBits + metadata_w < 32
```

得到：

```text
maxAddrBits < 26
```

因此最大只能是：

```text
25 bit
```

---

*笔记版本：v1.1*
*后续推荐阅读：`Scratchpad.scala`*
