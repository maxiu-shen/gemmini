# `Configs.scala` 阅读导读

> 适用阶段：`Day 1`  
> 阅读目标：理解 `Configs.scala` 如何基于 `GemminiArrayConfig` 构造具体配置实例，并把 Gemmini 作为 RoCC 协处理器挂进系统。

---

## 1. 先说这个文件是干嘛的

如果说：

- `GemminiConfigs.scala` 定义了“参数模板”和“参数推导规则”

那么：

- `Configs.scala` 定义的就是“具体要用哪一套参数”，以及“怎么把 Gemmini 接进 Chipyard/Rocket 系统”

所以这两个文件的关系可以先记成：

```text
GemminiConfigs.scala
    = 定义参数模板与推导规则

Configs.scala
    = 选一套具体参数，并把 Gemmini 实例化到系统里
```

---

## 2. 今天读这个文件，目标只抓 3 件事

1. `GemminiConfigs.defaultConfig` 这种东西到底是什么。
2. `DefaultGemminiConfig` 这种 `Config` 类到底做了什么。
3. Gemmini 最终是怎么通过 `BuildRoCC` 挂进系统的。

今天先不要把注意力放在所有配置变种的细节差别上。

---

## 3. 第 1 步：先看最上面的 `object GemminiConfigs`

文件开头最重要的一段是：

```scala
object GemminiConfigs {
  val defaultConfig = GemminiArrayConfig[SInt, Float, Float](...)
```

这里最关键的理解是：

**这里不是在定义参数类型，而是在创建一份具体配置实例。**

也就是说：

- `GemminiArrayConfig` 是参数模板
- `defaultConfig` 是基于这个模板创建出来的一套默认参数

你可以先建立这个映射：

```text
GemminiArrayConfig = 参数模板
defaultConfig      = 一套具体默认参数
```

---

## 4. 第 2 步：第一遍只看 `defaultConfig`

这个文件里有很多配置，但第一遍你只需要抓住 `defaultConfig`。

---

### 4.1 数据类型参数

```scala
inputType = SInt(8.W),
weightType = SInt(8.W),
accType = SInt(32.W),

spatialArrayInputType = SInt(8.W),
spatialArrayWeightType = SInt(8.W),
spatialArrayOutputType = SInt(20.W),
```

这里你先记住：

- 输入和权重默认是 8-bit
- 累加默认是 32-bit
- 阵列内部输出位宽是 20-bit

这说明：

**阵列内部输出位宽和最终 accumulator 位宽不是同一层东西。**

也就是说，Gemmini 内部不同位置看到的数据位宽可能并不一样。

---

### 4.2 阵列尺寸参数

```scala
tileRows = 1,
tileColumns = 1,
meshRows = 16,
meshColumns = 16,
```

这里必须马上和 `GemminiConfigs.scala` 对起来：

因为之前你已经知道：

```text
DIM = tileRows * meshRows = tileColumns * meshColumns
```

所以默认配置对应的是：

- `1 * 16 = 16`

也就是说：

**默认 Gemmini 是一个 16x16 阵列。**

这是 Day 1 必须记住的一个结论。

---

### 4.3 片上存储参数

```scala
sp_capacity = CapacityInKilobytes(256),
acc_capacity = CapacityInKilobytes(64),

sp_banks = 4,
acc_banks = 2,

sp_singleported = true,
acc_singleported = false,
```

这组参数先不要算公式，只要读出系统直觉：

- scratchpad 比 accumulator 更大
- scratchpad bank 更多
- scratchpad 和 accumulator 的端口组织不同

这说明默认配置下：

- scratchpad 更像主输入工作区
- accumulator 更像结果和部分和工作区

---

### 4.4 调度与队列参数

```scala
reservation_station_entries_ld = 8,
reservation_station_entries_st = 4,
reservation_station_entries_ex = 16,

ld_queue_length = 8,
st_queue_length = 2,
ex_queue_length = 8,
```

这部分说明：

**默认配置已经把 load/store/execute 三条路径的调度深度具体化了。**

你现在只要知道：

- 这些不是算力参数
- 它们是调度容量和并发容量参数

后面读 `ReservationStation.scala` 时，你会真正看到这些值怎么被消费。

---

### 4.5 DMA / TLB 参数

```scala
max_in_flight_mem_reqs = 16,

dma_maxbytes = 64,
dma_buswidth = 128,

tlb_size = 4,
```

这说明：

**`defaultConfig` 不只配置阵列，也配置访存系统能力。**

换句话说，这是一颗完整 Gemmini 的系统配置，而不只是“计算阵列参数表”。

---

## 5. 第 3 步：看到 `mvin_scale_args` / `acc_scale_args` 不要慌

这一大段代码最容易让初学者读崩。

比如：

```scala
mvin_scale_args = Some(ScaleArguments(...))
acc_scale_args = Some(ScaleArguments(...))
```

第一遍你只需要读出 3 件事：

1. 默认配置启用了 `mvin` 缩放和 `acc` 缩放。
2. 这些缩放实现依赖 `hardfloat`。
3. `ScaleArguments` 里不只放“类型”，还放了：
   - 硬件实现函数
   - 延迟
   - 软件侧字符串表示

所以这里不是普通参数表，而是在配置一部分**功能实现策略**。

但今天先不要深入浮点 recoding 和 `MulAddRecFN` 的实现细节。

---

## 6. 第 4 步：看 `dummyConfig`，理解“配置可以派生”

第二个值得看的配置是：

```scala
val dummyConfig = GemminiArrayConfig[DummySInt, Float, Float](...)
```

这里第一遍你不需要搞懂 `DummySInt` 的全部背景，只要知道：

**这里又构造了一套新的 Gemmini 配置。**

更重要的是，它大量复用了默认配置：

```scala
tileRows     = defaultConfig.tileRows,
tileColumns  = defaultConfig.tileColumns,
meshRows     = defaultConfig.meshRows,
meshColumns  = defaultConfig.meshColumns,
...
dma_maxbytes = defaultConfig.dma_maxbytes,
dma_buswidth = defaultConfig.dma_buswidth,
tlb_size     = defaultConfig.tlb_size,
```

这说明一个非常重要的工程习惯：

**Gemmini 的配置通常不是从零重写，而是从已有配置继承和修改。**

---

## 7. 第 5 步：重点理解 `.copy(...)`

这个文件里最值得学的写法之一，就是：

```scala
val chipConfig = defaultConfig.copy(...)
val largeChipConfig = chipConfig.copy(...)
val leanConfig = defaultConfig.copy(...)
val leanPrintfConfig = defaultConfig.copy(...)
```

你要从这里看出一个核心思想：

- `defaultConfig` 是基础版
- `chipConfig` 是更偏芯片落地场景的版本
- `largeChipConfig` 是更大阵列版本
- `leanConfig` 是更精简版本

也就是说：

**这个文件维护的是一个配置家族，而不是只有一套唯一配置。**

这点很重要，因为后面你看到不同论文、不同实验、不同 SoC 组合时，往往只是配置变了，不是整个 Gemmini 重写了。

---

## 8. 第 6 步：真正关键的地方，Gemmini 怎么挂进系统

Day 1 里最关键的代码是这里：

```scala
class DefaultGemminiConfig[T <: Data : Arithmetic, U <: Data, V <: Data](
  gemminiConfig: GemminiArrayConfig[T,U,V] = GemminiConfigs.defaultConfig
) extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq(
    (p: Parameters) => {
      implicit val q = p
      val gemmini = LazyModule(new Gemmini(gemminiConfig))
      gemmini
    }
  )
})
```

这段代码必须看懂，因为它回答了 Day 1 的核心问题：

**Gemmini 是怎么进入 Chipyard/Rocket 系统的？**

你可以把它翻译成中文：

- 定义一个系统配置类：`DefaultGemminiConfig` 
- 默认使用 GemminiConfigs.defaultConfig 这套参数
- 当 Chipyard 构建 RoCC 协处理器列表时，在原有列表基础上再追加一个新的 Gemmini
- 这颗 Gemmini 用 gemminiConfig 参数实例化，并以 LazyModule 形式接入系统

所以一句话：

**Gemmini 是通过 `BuildRoCC` 作为一个 RoCC 协处理器挂进系统的。**

---

## 9. 第 7 步：`LeanGemminiConfig` / `DummyDefaultGemminiConfig` 的本质

你再看下面这些类，会发现结构几乎一样：

```scala
class LeanGemminiConfig[...](
  gemminiConfig: GemminiArrayConfig[T,U,V] = GemminiConfigs.leanConfig
) extends Config(...)
```

这时候你应该立刻总结出：

**这些类的主要区别，不在“挂接方式”，而在“传进去的是哪套配置”。**

也就是说：

- 系统挂接框架是一样的
- 变化的是具体使用哪份 `gemminiConfig`

---

## 10. 第 8 步：`DualGemminiConfig` 第一遍只看思想

文件最后还有：

```scala
class DualGemminiConfig extends Config((site, here, up) => { ... })
```

注释已经说得很清楚：

```scala
// This Gemmini config has both an Int and an FP Gemmini side-by-side, sharing
// the same scratchpad.
```

第一遍你只需要理解：

**这里在尝试同时挂两个 Gemmini：一个整数版，一个浮点版。**

并且它们还涉及共享存储。

这部分很重要，但不适合 Day 1 深挖。  
今天你只要知道：这展示了更复杂的系统集成方式。

---

## 11. 用一句话概括整个 `Configs.scala`

**`Configs.scala` 做了两件事：先定义若干套具体 Gemmini 参数实例，再把这些实例通过 `BuildRoCC` 作为 RoCC 加速器挂进系统。**

---

## 12. 读完这个文件后，你必须记住的 5 句话

1. `GemminiConfigs.scala` 定义参数模板，`Configs.scala` 定义具体参数实例。
2. `defaultConfig` 是一套具体默认参数，而不是参数类型本身。
3. `chipConfig`、`leanConfig`、`largeChipConfig` 等，都是从已有配置派生出来的变体。
4. `DefaultGemminiConfig` 这类 `Config` 的核心工作，是把 `Gemmini(gemminiConfig)` 挂到 `BuildRoCC`。
5. Gemmini 在 Chipyard 里本质上是一个 `RoCC` 协处理器。

---

## 13. 你在 Day 1 读完后应该能回答的问题

1. `defaultConfig` 和 `GemminiArrayConfig` 的关系是什么？
2. `DefaultGemminiConfig` 真正做的最核心的一件事是什么？
3. 为什么 `chipConfig = defaultConfig.copy(...)` 这种写法很重要？
4. 为什么说这个文件比前一个文件更接近“系统集成层”？

---

## 14. 当前阶段不要深陷的内容

今天先不要在这些地方停太久：

- `hardfloat` 相关实现细节
- 每一个 `copy(...)` 改了哪些性能含义
- `DualGemminiConfig` 里的共享存储接线细节
- 所有变体之间的完整性能取舍

今天的目标不是“吃透所有配置版本”，而是：

**先理解配置实例是怎么形成的，以及 Gemmini 怎么挂进系统。**

---

## 15. 一句话收束

如果只用一句话概括 `Configs.scala`，那就是：

**它把抽象参数模板变成具体 Gemmini 实例，并把这个实例作为 RoCC 协处理器接到系统里。**
