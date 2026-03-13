# Gemmini `src/main/scala/gemmini` 模块层次与调用关系

> **范围**：`src/main/scala/gemmini` 下当前全部 55 个 Scala 文件  
> **目标**：用“先主链、后分层、最后速查”的方式，把 Gemmini 这一层讲清楚  
> **说明**：这里的“调用关系”按硬件语义理解为 `参数依赖 + 模块实例化 + 接口接线 + 指令生产/消费`，不是软件里的函数调用栈

---

## 1. 先回答一个问题：`gemmini/` 这一层到底在做什么？

如果只看系统职责，`src/main/scala/gemmini` 做的是这件事：

**把 CPU 发来的 Gemmini 指令，变成一串可执行的 load / execute / store 微操作，再驱动 scratchpad、DMA 和脉动阵列去真正完成矩阵乘、卷积、归一化、写回等工作。**

可以把它理解成 5 层：

1. **配置层**：决定阵列大小、bank 数、数据类型、DMA/TLB 参数、是否支持卷积/归一化等。
2. **前端展开与调度层**：把宏命令拆细，并判断哪些 load/execute/store 现在可以发射。
3. **访存与片上存储层**：负责主存 <-> scratchpad/accumulator 的搬运和管理。
4. **阵列执行层**：把数据送进 `Mesh -> Tile -> PE` 完成计算。
5. **基础支撑层**：提供 ISA 编码、地址编码、FIFO、仲裁器、流水线、TLB、计数器等公共能力。

所以，第一次读这层代码时，最重要的不是背 55 个文件，而是先抓住**当前真实主链**。

---

## 2. 当前真实主链是什么？

从 `Controller.scala` 当前接线看，Gemmini 的主路径是：

```text
Config*.scala / GemminiConfigs.scala
    -> GemminiArrayConfig
        -> Controller.scala
            -> raw_cmd_q
            -> LoopConv.scala        (可选)
            -> LoopMatmul.scala
            -> ReservationStation.scala
                -> LoadController.scala
                -> StoreController.scala
                -> ExecuteController.scala
                    -> Scratchpad.scala
                    -> Im2Col.scala   (卷积相关)
                    -> MeshWithDelays.scala
                        -> Transposer.scala
                        -> Mesh.scala
                            -> Tile.scala
                                -> PE.scala
                                    -> MacUnit
```

同时，访存相关的另一条并行主线是：

```text
LoadController / StoreController
    -> Scratchpad.scala
        -> DMA.scala
            -> DMACommandTracker.scala
            -> XactTracker.scala
            -> BeatMerger.scala
        -> AccumulatorMem.scala
        -> VectorScalarMultiplier.scala
        -> PixelRepeater.scala
        -> ZeroWriter.scala
        -> Normalizer.scala
        -> AccumulatorScale.scala
        -> SyncMem.scala / SharedExtMem.scala
```

最后，完成信号会回到调度器：

```text
Load completed
Store completed
Execute completed
    -> completion arbiter
    -> ReservationStation.completed
    -> 回馈 LoopConv / LoopMatmul
```

一句话概括这条主链：

**配置先决定硬件长什么样，前端再把命令拆细，`ReservationStation` 决定谁先发，三个 controller 分别管 load / store / execute，`Scratchpad` 和 `Mesh` 负责真正搬数据和算数据。**

---

## 3. 主链按层展开

## 3.1 第 1 层：配置层

这一层回答的是：**Gemmini 这个加速器到底长成什么样？**

- `GemminiConfigs.scala`
  - 定义 `GemminiArrayConfig`
  - 它是整个目录的参数根
  - 阵列维度、数据类型、bank 数、DMA 位宽、TLB 大小、是否支持 `loop_conv`、是否支持 `normalization`，都从这里派生

- `Configs.scala`
  - 给出整数版 Gemmini 的常用配置入口
  - 最终把配置挂到 `BuildRoCC -> LazyModule(new Gemmini(...))`

- `ConfigsFP.scala`
  - 给出浮点版 Gemmini 的配置入口

- `DSEConfigs.scala`
  - 提供设计空间探索用的多组参数变体

- `CustomConfigs.scala`
  - 提供用户自定义 Gemmini 参数入口

- `CustomCPUConfigs.scala`
  - 自定义 CPU 组合模板
  - 当前更像模板，不是主线活跃代码

- `CustomSoCConfigs.scala`
  - 自定义 SoC 组合模板
  - 当前更像模板，不是主线活跃代码

这里最重要的认识是：

**`GemminiConfigs.scala` 决定“硬件长什么样”，`Configs*.scala` 决定“这套硬件被哪个 SoC 配置实例化出来”。**

---

## 3.2 第 2 层：顶层装配与前端展开层

这一层回答的是：**CPU 发来的命令，怎么变成 Gemmini 能真正执行的微操作？**

### 顶层入口

- `Controller.scala`
  - Gemmini 顶层装配文件
  - 它负责把前端展开器、调度器、三个 controller、TLB、Scratchpad、Im2Col、计数器全部接起来
  - 如果只读一个文件来建立全局图景，优先读它

### 宏命令展开

- `LoopConv.scala`
  - 把卷积宏命令拆成 bias/input/weight 的 load、execute、store 微命令
  - 只有配置打开 `has_loop_conv` 时才位于主链上

- `LoopMatmul.scala`
  - 把 `LOOP_WS`、`resadd` 等矩阵乘宏命令拆成普通 Gemmini 微命令
  - 它是当前前端主链上的关键展开器

### 执行前的小型重写器

- `TransposePreloadUnroller.scala`
  - 处理转置 + WS 这种特殊执行情形
  - 会把部分 preload/compute 序列改写成更安全的微序列

### 当前主链的调度核心

- `ReservationStation.scala`
  - 当前 Gemmini 真正活跃的统一调度器
  - 它的核心职责不是“算”，而是“决定现在谁能发”
  - 它把命令分成 `ld / ex / st` 三类，检查地址依赖，再向三个 controller 分别发射

所以这一层的逻辑可以记成：

```text
Controller
    -> 先收命令
    -> LoopConv / LoopMatmul 把大命令拆小
    -> ReservationStation 判断依赖并发射
```

---

## 3.3 第 3 层：三个 controller 分别干什么？

这一层回答的是：**发射出去之后，谁真正干活？**

- `LoadController.scala`
  - 负责 `mvin` 一类加载命令
  - 主要任务是从主存读数据，放进 scratchpad 或 accumulator

- `StoreController.scala`
  - 负责 `mvout`、`store_spad`、`config_store`、`config_norm` 等写回相关命令
  - 主要任务是把 scratchpad 或 accumulator 里的结果写回主存

- `ExecuteController.scala`
  - 负责 preload、compute、flush 等执行类命令
  - 主要任务是从 `Scratchpad`/`ACC` 读操作数，送进阵列前端，再驱动计算进行

这三个模块的关系非常清楚：

```text
ReservationStation.issue.ld -> LoadController
ReservationStation.issue.st -> StoreController
ReservationStation.issue.ex -> ExecuteController
```

也就是说：

- **LoadController** 管“搬进来”
- **StoreController** 管“搬出去”
- **ExecuteController** 管“在里面怎么算”

---

## 3.4 第 4 层：`Scratchpad` 为什么是核心枢纽？

这一层回答的是：**数据在 Gemmini 内部到底怎么流？**

Gemmini 的真正数据中心不是 `Mesh`，而是 `Scratchpad.scala`。

原因是：

1. load 路径要把数据搬进来，先进入它
2. execute 路径要读操作数，也先从它拿
3. store 路径要把结果写出去，也要经过它
4. accumulator、归一化、激活、缩放等后处理，也都挂在它周围

所以你可以把 `Scratchpad.scala` 看成：

**Gemmini 的片上存储总枢纽 + 访存总枢纽 + 部分后处理总枢纽**

它下面最关键的几个模块是：

- `DMA.scala`
  - 负责和主存/TileLink 打交道
  - 读路径会结合 `XactTracker.scala` 和 `BeatMerger.scala`

- `DMACommandTracker.scala`
  - 负责判断一整条 DMA 命令是否已经完成

- `XactTracker.scala`
  - 负责跟踪每个 in-flight 读事务的地址和元数据

- `BeatMerger.scala`
  - 负责把总线上的 beat 拼成 scratchpad/accumulator 需要的行宽数据

- `AccumulatorMem.scala`
  - 真正承载 accumulator bank 的存储体

- `VectorScalarMultiplier.scala`
  - 负责向量逐元素缩放

- `PixelRepeater.scala`
  - 服务首层卷积优化，把像素重复展开

- `ZeroWriter.scala`
  - 服务全零写入或零初始化场景

- `Normalizer.scala`
  - 负责 layernorm / softmax 等统计与归一化相关处理

- `AccumulatorScale.scala`
  - 负责把 ACC 读出的结果进一步做激活、缩放、位宽裁剪

- `SyncMem.scala`
  - 提供底层同步存储封装

- `SharedExtMem.scala`
  - 面向共享外部存储的特殊配置路径

这一层最实用的记忆方式是：

```text
主存 <-> DMA <-> Scratchpad / Accumulator <-> ExecuteController
```

---

## 3.5 第 5 层：阵列执行层怎么理解？

这一层回答的是：**数据进入执行路径后，怎么一步步流到 PE？**

主线是：

```text
ExecuteController
    -> MeshWithDelays
        -> Transposer
        -> Mesh
            -> Tile
                -> PE
                    -> MacUnit
```

各模块的职责可以这样理解：

- `MeshWithDelays.scala`
  - 真正进入阵列前的“阵列前端包装层”
  - 它负责转置、时序对齐、tag 跟踪、输入延迟匹配

- `Transposer.scala`
  - 提供输入矩阵转置能力

- `Mesh.scala`
  - 把多个 `Tile` 组织成二维脉动阵列

- `Tile.scala`
  - 把多个 `PE` 组成一个更小的局部阵列单元

- `PE.scala`
  - 单个处理元素
  - 真正执行 MAC 与数据传播

- `Im2Col.scala`
  - 不直接属于 `Mesh` 内部，但常与执行路径一起理解
  - 主要服务卷积，把输入重排成适合 GEMM 的列块

- `Shifter.scala`
  - 提供移位/延迟对齐能力
  - 更像执行前端和存储之间的配套模块

所以阵列层最重要的认识是：

**真正做乘加的是 `PE`，真正把很多 `PE` 组织成可用脉动阵列的是 `Mesh`，真正让阵列“吃对数据、吃对时序”的是 `MeshWithDelays`。**

---

## 4. 哪些模块不是当前主链，要单独看？

这部分代码容易和主链混淆，所以单独列出来。

### 4.1 旧的 CISC / Tiler 路径

```text
CmdFSM.scala
    -> TilerController.scala
        -> TilerFSM.scala
        -> TilerScheduler.scala
            -> Load / Store / ExecuteController
```

- `CmdFSM.scala`
  - 旧 CISC 前端命令收集器
  - 先把一组命令攒起来，再形成更高层的 tiler 命令

- `TilerController.scala`
  - 旧 tiler 路径的顶层

- `TilerFSM.scala`
  - 把 tiler 命令展开成一串基础 RoCC 指令

- `TilerScheduler.scala`
  - 对这些展开后的指令做依赖和发射调度

这条链**有参考价值，但不是当前 `Controller.scala` 里主用的前端路径**。

### 4.2 其他保留/辅助模块

- `LoopUnroller.scala`
  - 早期的简化版 `loop_ws` 展开器
  - 适合当历史实现参考

- `InstructionCompression.scala`
  - preload / compute 压缩与解压模块
  - 更像辅助优化模块，不是主链骨架

---

## 5. 基础支撑层：哪些文件是“大家都会用到的公共件”？

这一层不直接构成主链，但很多主链文件都依赖它们。

- `Dataflow.scala`
  - 定义 `OS / WS / BOTH`

- `GemminiISA.scala`
  - 定义 Gemmini 指令编码和 `rs1/rs2` 打包格式

- `LocalAddr.scala`
  - 定义 Gemmini 本地地址编码
  - 是 `ReservationStation`、`ExecuteController`、`Scratchpad` 都会解码的关键公共格式

- `NormCmd.scala`
  - 定义归一化/统计相关命令编码

- `Arithmetic.scala`
  - 定义 `UInt / SInt / Float` 等算术抽象

- `Activation.scala`
  - 定义激活/归一化模式编码

- `Util.scala`
  - 通用工具函数

- `Pipeline.scala`
  - 通用流水线模块

- `WeightedArbiter.scala`
  - 带权仲裁器

- `TagQueue.scala`
  - tag 对齐队列

- `MultiHeadedQueue.scala`
  - 多读头 FIFO

- `MultiTailedQueue.scala`
  - 多写尾 FIFO

- `CounterFile.scala`
  - 性能计数器及 `CounterController`

- `FrontendTLB.scala`
  - DMA 前端 TLB

这层可以理解为：

**主链模块负责“做事”，基础支撑层负责“提供统一格式和公共零件”。**

---

## 6. 全目录文件速查表

## 6.1 配置与构建入口

- `GemminiConfigs.scala`
- `Configs.scala`
- `ConfigsFP.scala`
- `DSEConfigs.scala`
- `CustomConfigs.scala`
- `CustomCPUConfigs.scala`
- `CustomSoCConfigs.scala`

## 6.2 顶层控制与前端展开

- `Controller.scala`
- `ReservationStation.scala`
- `LoadController.scala`
- `StoreController.scala`
- `ExecuteController.scala`
- `LoopMatmul.scala`
- `LoopConv.scala`
- `TransposePreloadUnroller.scala`
- `LoopUnroller.scala`
- `InstructionCompression.scala`
- `CmdFSM.scala`
- `TilerController.scala`
- `TilerFSM.scala`
- `TilerScheduler.scala`

## 6.3 访存、片上存储与地址

- `Scratchpad.scala`
- `DMA.scala`
- `DMACommandTracker.scala`
- `XactTracker.scala`
- `BeatMerger.scala`
- `SyncMem.scala`
- `SharedExtMem.scala`
- `AccumulatorMem.scala`
- `LocalAddr.scala`

## 6.4 阵列执行与数据重排

- `MeshWithDelays.scala`
- `Mesh.scala`
- `Tile.scala`
- `PE.scala`
- `Transposer.scala`
- `Shifter.scala`
- `Im2Col.scala`
- `PixelRepeater.scala`
- `VectorScalarMultiplier.scala`
- `ZeroWriter.scala`
- `AccumulatorScale.scala`
- `Normalizer.scala`
- `Activation.scala`
- `Arithmetic.scala`

## 6.5 基础协议、队列、工具、系统服务

- `Dataflow.scala`
- `GemminiISA.scala`
- `NormCmd.scala`
- `Util.scala`
- `Pipeline.scala`
- `WeightedArbiter.scala`
- `TagQueue.scala`
- `MultiHeadedQueue.scala`
- `MultiTailedQueue.scala`
- `CounterFile.scala`
- `FrontendTLB.scala`

---

## 7. 推荐阅读顺序

如果你是第一次系统读 Gemmini，推荐顺序如下：

1. `GemminiConfigs.scala`
2. `Controller.scala`
3. `ReservationStation.scala`
4. `LoadController.scala`
5. `StoreController.scala`
6. `ExecuteController.scala`
7. `Scratchpad.scala`
8. `MeshWithDelays.scala`
9. `Mesh.scala`
10. `Tile.scala`
11. `PE.scala`
12. `LoopMatmul.scala`
13. `LoopConv.scala`

如果你接下来想补历史路径或旁路设计，再读：

1. `CmdFSM.scala`
2. `TilerController.scala`
3. `TilerFSM.scala`
4. `TilerScheduler.scala`
5. `LoopUnroller.scala`
6. `InstructionCompression.scala`

---

## 8. 最后用一句话收束

Gemmini `src/main/scala/gemmini` 这一层，最核心的理解方式不是“55 个独立文件”，而是下面这条主线：

```text
配置决定硬件形态
    -> 前端把宏命令拆成微命令
    -> ReservationStation 判断依赖并发射
    -> Load / Store / ExecuteController 分头执行
    -> Scratchpad 负责搬数据与存数据
    -> Mesh / Tile / PE 负责真正计算
```

先把这条线看清楚，再回来看每个文件，就不会乱。
