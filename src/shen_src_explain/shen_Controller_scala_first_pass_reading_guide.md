# `Controller.scala` 第一轮阅读导读

> 适用阶段：`Day 2`  
> 阅读目标：第一次阅读 `Controller.scala` 时，不追所有实现细节，只抓住它作为 Gemmini 顶层“总装图 + 总调度入口”的角色。

---

## 1. 先给这个文件一个总定位

`Controller.scala` 是 Gemmini 当前主线里最核心的文件之一。

第一次读它时，不要把注意力放在每一个状态机、每一条位宽公式、每一个 TileLink 接口细节上。  
第一轮你只做一件事：

**把 `Controller.scala` 看成 Gemmini 的“总装图 + 总调度入口”。**

也就是说，这一轮阅读你要回答的是：

1. CPU 发来的命令从哪里进入 Gemmini？
2. 命令在顶层经过了哪些前端处理？
3. 三个 controller 是怎么被接起来的？
4. 完成信号怎么回传给调度器？

---

## 2. 第一次阅读时最重要的一句话

`Controller.scala` 最核心的价值，不是单独实现某个算法，而是：

**把 Gemmini 的主要子模块都实例化出来，并把命令流、数据流、完成信号流接起来。**

所以第一次读它，你要像看系统框图，而不是像看一个普通软件函数。

---

## 3. 开头先抓住 3 个最重要的类

文件一开始，先只抓住下面 3 个名字：

### 3.1 `GemminiCmd`

`GemminiCmd` 是 Gemmini 内部流转时使用的命令包。

它比原始 `RoCCCommand` 多了这些字段：

- `rob_id`
- `from_matmul_fsm`
- `from_conv_fsm`

这说明：

**命令进入 Gemmini 后，不再只是“原始 RoCC 指令”，而会被包装成带内部调度信息的格式。**

也就是说，Gemmini 内部看到的命令，已经是“增强版命令”了。

---

### 3.2 `Gemmini`

`Gemmini` 是 Gemmini 作为 RoCC 协处理器接入系统时的外层包装。

它继承的是：

```scala
extends LazyRoCC(...)
```

所以你可以把它理解成：

- 和 Chipyard/Rocket 对接的那一层
- 负责 RoCC 接口、TL 端口等系统集成事务

---

### 3.3 `GemminiModule`

`GemminiModule` 才是 Gemmini 内部真正的硬件主体。

可以先粗略理解成：

- `Gemmini`：系统外层壳
- `GemminiModule`：内部接线和控制主体

所以这次阅读真正的主战场，其实是 `GemminiModule`。

---

## 4. 外层 `Gemmini` 先干了什么

第一次读外层 `Gemmini` 时，只需要抓住 4 件事：

1. 它是一个 `LazyRoCC`
2. 它使用 `config.opcodes`
3. 它会生成软件头文件
4. 它先实例化了 `Scratchpad`

特别值得注意的是：

```scala
Files.write(Paths.get(config.headerFilePath), config.generateHeader().getBytes(StandardCharsets.UTF_8))
```

这说明：

- 你在 `GemminiConfigs.scala` 里看到的 `generateHeader()`，会在这里真正被调用
- Gemmini 顶层 elaboration 时，会顺手生成软件侧头文件

这正好把 `Day 1` 和 `Day 2` 串起来了。

---

## 5. 第一次进入 `GemminiModule`，先看公共基础模块

进入 `GemminiModule` 后，先不要急着追命令流，先看到这些公共模块：

### 5.1 CounterController

它负责 Gemmini 的计数器与事件统计。

你现在只需要知道：

- Gemmini 内部有性能计数器
- 顶层会把多个模块的 counter 事件收集起来
- 某些命令可以直接访问 counter

---

### 5.2 FrontendTLB

它负责 Gemmini 的地址翻译相关前端逻辑。

第一次阅读时只要知道：

- 顶层需要把 TLB / PTW 接起来
- 这属于系统支撑路径，不是当前主线的调度核心

---

### 5.3 时钟门控

`Controller.scala` 里还负责管理 gated clock。

第一次阅读只要知道：

- 顶层不仅管命令和数据流
- 也管一些系统级控制事务，比如 clock gate

---

## 6. 现在开始看真正的命令主线

这是第一次阅读时最重要的部分。

---

### 6.1 命令先进入 `raw_cmd_q`

你首先会看到：

```scala
val raw_cmd_q = Module(new Queue(new GemminiCmd(reservation_station_entries), entries = 2))
...
val raw_cmd = raw_cmd_q.io.deq
```

这一步说明：

**CPU / RoCC 发来的命令，会先被包装成 `GemminiCmd`，然后进入顶层原始命令队列 `raw_cmd_q`。**

所以主线的第一步是：

```text
io.cmd -> raw_cmd_q -> raw_cmd
```

---

### 6.2 前端宏命令展开：`LoopConv` 和 `LoopMatmul`

接着你会看到：

```scala
val (conv_cmd, loop_conv_unroller_busy) = if (has_loop_conv) { LoopConv(raw_cmd, ...) } else ...
val (loop_cmd, loop_matmul_unroller_busy, loop_completed) = LoopMatmul(...)
```

这里的核心理解是：

- `raw_cmd` 先可能经过 `LoopConv`
- 再经过 `LoopMatmul`
- 大命令在这里被展开成更细的微命令流

所以前端主线可以写成：

```text
raw_cmd
  -> LoopConv（可选）
  -> LoopMatmul
  -> loop_cmd
```

这一步是当前 Gemmini 前端主线最关键的部分。

---

### 6.3 再进入 `unrolled_cmd`

```scala
val unrolled_cmd = Queue(loop_cmd)
```

这说明：

- 前端展开后的命令还会再进一层队列

所以到目前为止，你脑子里应该有这条线：

```text
io.cmd
 -> raw_cmd_q
 -> LoopConv
 -> LoopMatmul
 -> unrolled_cmd
```

---

## 7. `ReservationStation` 是调度核心

第一次读 `Controller.scala`，最关键的模块之一就是：

```scala
val reservation_station = Module(new ReservationStation(...))
```

后面你会看到：

```scala
reservation_station.io.alloc.valid := true.B
...
when(reservation_station.io.alloc.fire) {
  unrolled_cmd.ready := true.B
}
```

这说明：

**普通执行命令不会直接送给 controller，而是先分配给 `ReservationStation`。**

所以这一步你要建立的图是：

```text
unrolled_cmd -> reservation_station.alloc
```

这也是为什么说：

**`ReservationStation` 是这个文件里的调度核心。**

因为：

- `LoopConv / LoopMatmul` 负责拆命令
- `ReservationStation` 负责判断哪些命令现在可以发

---

## 8. 三大 controller 在哪里被接起来

第一次阅读时，你一定要抓住这三行：

```scala
val load_controller = Module(new LoadController(...))
val store_controller = Module(new StoreController(...))
val ex_controller = Module(new ExecuteController(...))
```

这就是三大 controller 的顶层实例化位置。

更关键的是下面这些接线：

```scala
load_controller.io.cmd.valid := reservation_station.io.issue.ld.valid
store_controller.io.cmd.valid := reservation_station.io.issue.st.valid
ex_controller.io.cmd.valid := reservation_station.io.issue.ex.valid
```

这说明：

**`ReservationStation` 会把命令分成 `ld / st / ex` 三路，再分别发给三个 controller。**

所以你现在应该把主线补成：

```text
unrolled_cmd
 -> ReservationStation
    -> LoadController
    -> StoreController
    -> ExecuteController
```

这就是 Gemmini 顶层调度关系的主干。

---

## 9. 为什么说 `Scratchpad` 是数据总枢纽

这是第一次阅读 `Controller.scala` 时最应该抓住的另一个结论。

你会看到顶层有这样一组接线：

```scala
spad.module.io.dma.read <> load_controller.io.dma
spad.module.io.dma.write <> store_controller.io.dma
ex_controller.io.srams.read <> spad.module.io.srams.read
ex_controller.io.srams.write <> spad.module.io.srams.write
spad.module.io.acc.read_req <> ex_controller.io.acc.read_req
ex_controller.io.acc.read_resp <> spad.module.io.acc.read_resp
ex_controller.io.acc.write <> spad.module.io.acc.write
```

这组接线几乎直接证明：

**Gemmini 的数据中心不是 `Mesh`，而是 `Scratchpad`。**

因为你可以看出来：

- load 路径通过 DMA 把数据搬进 `spad`
- store 路径通过 DMA 从 `spad` 写回
- execute 路径从 `spad / acc` 读写数据

所以 `Scratchpad` 在顶层看来，是：

- 访存总枢纽
- 片上存储总枢纽
- 执行路径的数据接口中心

---

## 10. `Im2Col` 的位置

第一次阅读时，你还会看到：

```scala
val im2col = Module(new Im2Col(...))
```

以及：

```scala
im2col.io.req <> ex_controller.io.im2col.req
ex_controller.io.im2col.resp <> im2col.io.resp
```

这里你第一轮只要知道：

- `Im2Col` 不是三大 controller 之一
- 它是执行路径的辅助模块

后面那段仲裁代码还说明：

- `ExecuteController` 和 `Im2Col` 都可能访问 scratchpad 读口
- 顶层在这里做读口仲裁

这部分今天知道“有这个共享读口仲裁”就够了。

---

## 11. 完成信号怎么回传

这一步非常重要，因为它让整个命令闭环成立。

你会看到：

```scala
val reservation_station_completed_arb = Module(new Arbiter(..., 3))
...
reservation_station_completed_arb.io.in(0) := ex_controller.io.completed
reservation_station_completed_arb.io.in(1) := load_controller.io.completed
reservation_station_completed_arb.io.in(2) := store_controller.io.completed
...
reservation_station.io.completed := reservation_station_completed_arb.io.out
```

这说明：

**三个 controller 完成后，不是各自散掉，而是统一通过一个完成仲裁器回传给 `ReservationStation`。**

所以顶层闭环可以补成：

```text
Load/Store/Execute 完成
 -> completion arbiter
 -> ReservationStation.completed
```

这一步很关键，因为它对应了调度器的依赖解除和后续发射。

---

## 12. 第一次阅读后，你脑子里至少要有这条主线

第一次读完 `Controller.scala` 后，脑子里应该能形成下面这条线：

```text
CPU/RoCC io.cmd
 -> raw_cmd_q
 -> LoopConv（可选）
 -> LoopMatmul
 -> unrolled_cmd
 -> ReservationStation.alloc
 -> ReservationStation.issue.{ld,st,ex}
 -> LoadController / StoreController / ExecuteController
 -> Scratchpad / Accumulator / 阵列执行路径
 -> completed arbiter
 -> ReservationStation.completed
```

如果你能把这条线顺畅讲出来，说明你这一轮就已经读对了。

---

## 13. 第一次阅读时，不要深陷的部分

这次第一轮导读里，下面这些地方先不要卡太久：

- `use_ext_tl_mem` 那大段外部 scratchpad TileLink 接线
- `TLClientNode` / `TLIdentityNode` 细节
- `FrontendTLB` 的完整接口
- `Im2Col` 的内部实现
- 所有注释掉的旧 CISC / tiler 路径
- 每个 `funct` 编码为什么这样分类

尤其是被注释掉的旧路径，第一次阅读可以直接略过：

- `CmdFSM`
- `TilerController`
- `LoopUnroller` 的旧路径

因为当前真实主线已经明显变成：

- `LoopConv`
- `LoopMatmul`
- `ReservationStation`

---

## 14. 这次第一轮阅读，你应该能回答的 4 个问题

1. `GemminiCmd` 相比原始 `RoCCCommand`，为什么要多包一层？
2. `Controller.scala` 里，命令从 `io.cmd` 进入后，第一轮前端主线经过了哪些阶段？
3. 为什么说 `ReservationStation` 是这个文件里的调度核心？
4. 从顶层接线看，为什么说 `Scratchpad` 是数据总枢纽？

---

## 15. 一句话收束

如果只用一句话概括 `Controller.scala` 的第一轮阅读重点，那就是：

**它把 Gemmini 的前端命令展开、统一调度、三大 controller、scratchpad 数据通路和完成回传闭环，全部在顶层接了起来。**
