# `GemminiConfigs.scala` 阅读导读

> 适用阶段：`Day 1`  
> 阅读目标：先理解 `GemminiConfigs.scala` 在整个 Gemmini 中的系统角色，再抓住最关键的参数分类与设计约束。

---

## 1. 先说这个文件是干嘛的

你可以把 `GemminiConfigs.scala` 理解成：

**Gemmini 的总参数定义中心。**

它回答的是一句话：

**“这个 Gemmini 到底长什么样？”**

比如：

- 阵列多大
- 数据位宽是多少
- scratchpad / accumulator 多大
- load/store/execute 队列多深
- DMA 宽度多大
- 是否支持 `loop_conv`、`normalization` 等功能

这个文件里最核心的定义就是 `GemminiArrayConfig`。  
后面很多硬件模块都会依赖这个 config。

---

## 2. 按顺序怎么读

今天读这个文件时，不要从上到下硬啃全部参数。  
正确方式是按下面 5 步来读。

---

## 3. 第 1 步：先看开头的几个参数容器

文件开头先定义了几类“参数描述方式”：

```scala
sealed abstract trait GemminiMemCapacity
case class CapacityInKilobytes(kilobytes: Int) extends GemminiMemCapacity
case class CapacityInMatrices(matrices: Int) extends GemminiMemCapacity

case class ScaleArguments[T <: Data, U <: Data](...)
```

这里你只需要理解两件事：

### 3.1 `GemminiMemCapacity`

表示 Gemmini 的片上存储容量可以用两种方式描述：

- 按 KB 描述
- 按能放多少个矩阵块描述

### 3.2 `ScaleArguments`

表示如果启用了缩放功能，那么要把下面这些东西一起打包：

- 缩放函数
- 延迟
- 缩放因子类型
- 缩放单元数量

这一部分不用深究实现，知道它在先搭“参数语言”就够了。

---

## 4. 第 2 步：抓住 `GemminiArrayConfig`

文件最关键的主体是：

```scala
case class GemminiArrayConfig[T <: Data : Arithmetic, U <: Data, V <: Data](...)
```

你现在先记一句话：

**`GemminiArrayConfig` 是整个 Gemmini 硬件形态的参数根。**

也就是说，后面顶层控制、片上存储、阵列组织、DMA、软件头文件生成，都会依赖它。

---

## 5. 第 3 步：参数不要硬背，要分类看

这个参数表很长，但你应该按 6 类来读。

### 5.1 数据类型参数

```scala
inputType: T,
weightType: T,
accType: T,
spatialArrayInputType: T,
spatialArrayWeightType: T,
spatialArrayOutputType: T,
```

先抓住最重要的 3 个：

- `inputType`
- `weightType`
- `accType`

它们决定：

- scratchpad 里每个元素多宽
- PE 里的乘加位宽
- accumulator 为什么通常比 input 更宽

你可以先建立这个直觉：

- 输入和权重通常位宽较小
- 累加结果通常位宽更大

---

### 5.2 阵列形状参数

```scala
dataflow: Dataflow.Value = Dataflow.BOTH,

tileRows: Int = 1,
tileColumns: Int = 1,
meshRows: Int = 16,
meshColumns: Int = 16,
```

这是 Day 1 最重要的一组参数。

先记：

- `meshRows`, `meshColumns`：大阵列尺寸
- `tileRows`, `tileColumns`：tile 内部尺寸
- 最终阵列维度由它们共同决定

文件后面会直接推导：

```scala
val BLOCK_ROWS = tileRows * meshRows
val BLOCK_COLS = tileColumns * meshColumns
val DIM = BLOCK_ROWS
```

所以你要得到第一个结论：

**Gemmini 的 `DIM` 不是手写常数，而是由 `tile * mesh` 一起决定的。**

---

### 5.3 队列和调度容量参数

```scala
ld_queue_length: Int = 8,
st_queue_length: Int = 2,
ex_queue_length: Int = 8,

reservation_station_entries_ld: Int = 8,
reservation_station_entries_st: Int = 4,
reservation_station_entries_ex: Int = 16,
```

这组参数决定的是：

**前端能屯多少命令，三条路径各自能承受多少调度压力。**

你现在先不要想实现细节，只需要知道：

- 它们不是算力参数
- 它们是调度深度和并发容量参数

后面读 `ReservationStation.scala` 时，你会再次遇到它们。

---

### 5.4 片上存储参数

```scala
sp_banks: Int = 4,
sp_singleported: Boolean = false,
sp_capacity: GemminiMemCapacity = CapacityInKilobytes(256),
spad_read_delay: Int = 4,

acc_banks: Int = 2,
acc_singleported: Boolean = false,
acc_sub_banks: Int = -1,
acc_capacity: GemminiMemCapacity = CapacityInKilobytes(64),
acc_latency: Int = 2,
```

这是第二重要的一组参数。

你只要先记住：

- `sp_*` 对应 scratchpad
- `acc_*` 对应 accumulator
- 两者不仅容量不同，组织和延迟也不同

更关键的是，这些抽象参数会被推导成真正的硬件组织：

```scala
val sp_width = meshColumns * tileColumns * inputType.getWidth
val sp_bank_entries = ...
val acc_bank_entries = ...
val local_addr_t = new LocalAddr(...)
```

这说明两件事：

1. 参数不是摆设，它们会被转换成实际 bank 行数和地址组织。
2. `LocalAddr` 依赖 scratchpad / accumulator 的真实规模。

---

### 5.5 DMA / TLB / 访存参数

```scala
dma_maxbytes: Int = 64,
dma_buswidth: Int = 128,

tlb_size: Int = 4,
use_tlb_register_filter: Boolean = true,
max_in_flight_mem_reqs: Int = 16,
```

这一类参数控制的是：

**Gemmini 怎么和片外内存系统交互。**

今天先知道它们的存在和大致职责即可，不需要深挖细节。

---

### 5.6 功能开关参数

```scala
has_training_convs: Boolean = true,
has_max_pool: Boolean = true,
has_nonlinear_activations: Boolean = true,
has_dw_convs: Boolean = true,
has_normalizations: Boolean = false,
has_first_layer_optimizations: Boolean = true,
has_loop_conv: Boolean = true,
```

这组参数像“编译期开关”。

它说明：

- Gemmini 不是一成不变的固定硬件
- 某些功能是可以通过配置裁剪或打开的

---

## 6. 第 4 步：看派生参数，而不是死背原始参数

这个文件不只是存参数，还会根据参数推导出很多真正供硬件使用的常量，比如：

- 行数位宽
- 列数位宽
- stride 位宽
- `DIM`
- ROB 大小
- tile 字节数

例如：

```scala
val mvin_cols_bits = ...
val mvin_rows_bits = ...
val mvout_cols_bits = ...
val mvout_rows_bits = ...
```

还有：

```scala
val BLOCK_ROWS = tileRows * meshRows
val BLOCK_COLS = tileColumns * meshColumns
val DIM = BLOCK_ROWS
```

所以这一步你要建立的认识是：

**`GemminiArrayConfig` 不只是一个“配置表”，而是很多局部实现常量的源头。**

---

## 7. 第 5 步：重点看 `require(...)`

很多初学者读配置文件时会跳过 `require`，但这里恰恰藏着重要设计约束。

比如：

- bank 行数要求是 2 的幂
- systolic array 默认要求是方阵
- 阵列维度要求是 2 的幂
- accumulator / scratchpad 的组织必须和阵列尺寸兼容
- 某些缩放功能组合不是任意合法的

这意味着：

**Gemmini 的参数空间不是“想怎么配就怎么配”，而是受到实现约束的。**

你要把 `require(...)` 当作“硬件设计边界条件”来看。

---

## 8. 第 6 步：最后理解 `generateHeader()`

今天不用逐行读 `generateHeader()`，只需要抓住它的系统作用：

**根据当前硬件配置，自动生成软件侧要用的 `gemmini_params.h`。**

它会把像下面这些信息导出给软件：

- `DIM`
- bank 数
- bank 行数
- accumulator 行数
- 数据类型
- 缩放相关宏

也就是说：

**这个文件不只服务 RTL，也服务软件库和 baremetal 测试。**

这正说明 Gemmini 的硬件参数和软件参数是联动的。

---

## 9. 读完这个文件后，你必须记住的 4 句话

1. `GemminiConfigs.scala` 是 Gemmini 的参数根文件。
2. 最核心的对象是 `GemminiArrayConfig`。
3. 这个文件不仅保存参数，还会推导出很多硬件真正使用的常量。
4. 这个文件还负责把硬件配置同步生成到软件头文件里。

---

## 10. 你在 Day 1 读完后应该能回答的问题

1. `GemminiArrayConfig` 在整个 Gemmini 里最核心的作用是什么？
2. `meshRows / tileRows / meshColumns / tileColumns` 决定的是哪一类东西？
3. 为什么这个文件里会有 `generateHeader()`？
4. 为什么说 `require(...)` 反而是这个文件里最值得读的一部分之一？

---

## 11. 当前阶段不要深陷的内容

今天先不要在这些地方停留太久：

- 每一个位宽公式的细节推导
- `generateHeader()` 里的所有字符串拼接
- 所有 scale 相关细节
- 每一个 C 宏具体如何给软件使用

今天的目标不是“彻底吃透全部实现”，而是：

**先建立参数系统的全局作用。**

---

## 12. 一句话收束

如果只用一句话概括 `GemminiConfigs.scala`，那就是：

**它定义了 Gemmini 长什么样，并把这种“长相”进一步翻译成硬件内部常量和软件可见参数。**
