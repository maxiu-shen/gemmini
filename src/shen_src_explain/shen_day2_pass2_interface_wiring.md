# Controller.scala 第二遍阅读：模块间接口接线全图

> **阅读目标**：不看具体逻辑，只回答"谁和谁连在一起、通过什么接口、数据往哪个方向流"。
>
> **源文件**：`src/main/scala/gemmini/Controller.scala`

---

## 0. 两个类的关系

Controller.scala 包含两个类，它们是 Rocket Chip LazyModule 范式的标准拆分：

| 类 | 层次 | 职责 |
|----|------|------|
| `Gemmini`（外壳，`LazyRoCC`） | Diplomacy / 参数协商层 | 声明 TileLink 节点、实例化 `Scratchpad` LazyModule、生成 header 文件 |
| `GemminiModule`（内核，`LazyRoCCModuleImp`） | 硬件实现层 | 实例化所有子模块、完成全部接口接线 |

`Gemmini` 持有 `val spad = LazyModule(new Scratchpad(config))`，供 `GemminiModule` 通过 `outer.spad.module.io` 访问。

---

## 1. 命令入口：CPU/RoCC → Controller

```text
CPU ──RoCC──→ io.cmd ──→ raw_cmd_q (Queue, depth=2) ──→ raw_cmd
```

| 信号 | 方向 | 说明 |
|------|------|------|
| `io.cmd` | 输入（来自 CPU） | RoCC 标准命令接口，类型 `RoCCCommand` |
| `io.cmd.ready` | 输出（给 CPU） | 反压，等于 `raw_cmd_q.io.enq.ready` |
| `raw_cmd_q.io.enq.bits` | — | 把 `RoCCCommand` 包装成 `GemminiCmd`（加 `rob_id`、`from_conv_fsm`、`from_matmul_fsm` 字段） |
| `raw_cmd` | — | 即 `raw_cmd_q.io.deq`，是后续所有前端展开器的命令源 |

**对应代码（行 237–245）：**

```scala
val raw_cmd_q = Module(new Queue(new GemminiCmd(...), entries = 2))
raw_cmd_q.io.enq.valid := io.cmd.valid
io.cmd.ready := raw_cmd_q.io.enq.ready
raw_cmd_q.io.enq.bits.cmd := io.cmd.bits
// ...
val raw_cmd = raw_cmd_q.io.deq
```

---

## 2. 前端展开链：raw_cmd → LoopConv → LoopMatmul → unrolled_cmd

这是 Gemmini 的**CISC → RISC 展开流水线**，把高级循环命令拆成底层操作。

```text
raw_cmd ──→ [LoopConv] ──→ conv_cmd ──→ [LoopMatmul] ──→ loop_cmd ──→ Queue ──→ unrolled_cmd
                ↑                              ↑
         RS.conv_*_completed            RS.matmul_*_completed
```

### 2.1 LoopConv（条件实例化）

仅当 `has_loop_conv == true` 时存在，否则 `conv_cmd = raw_cmd`（直通）。

| 接口 | 方向 | 连接 |
|------|------|------|
| 命令输入 | `raw_cmd →` | 接收原始命令流 |
| 命令输出 | `→ conv_cmd` | 展开后的命令送给 LoopMatmul |
| 反馈输入 | `← reservation_station.io.conv_ld_completed` | ld 完成反馈 |
| 反馈输入 | `← reservation_station.io.conv_st_completed` | st 完成反馈 |
| 反馈输入 | `← reservation_station.io.conv_ex_completed` | ex 完成反馈 |

### 2.2 LoopMatmul（始终存在）

| 接口 | 方向 | 连接 |
|------|------|------|
| 命令输入 | `conv_cmd →`（或 `raw_cmd →`） | 接收上一级输出 |
| 命令输出 | `→ loop_cmd` | 展开后的命令 |
| 完成信号 | `→ loop_completed` | 矩阵乘完成标志（导出到 `completion_io`） |
| 反馈输入 | `← reservation_station.io.matmul_ld_completed` | ld 完成反馈 |
| 反馈输入 | `← reservation_station.io.matmul_st_completed` | st 完成反馈 |
| 反馈输入 | `← reservation_station.io.matmul_ex_completed` | ex 完成反馈 |

### 2.3 Unrolled Queue

```scala
val unrolled_cmd = Queue(loop_cmd)   // 行 269
```

`unrolled_cmd` 是展开后命令的最终出口，后续送入 ReservationStation 或直接处理特殊命令。

---

## 3. 命令分类与分发：unrolled_cmd → ReservationStation / 特殊处理

```text
unrolled_cmd ─┬── is_flush ──────→ tlb.flush
              ├── is_counter_op ──→ counters.io.in
              ├── is_clock_gate ──→ (直接消费)
              └── otherwise ──────→ reservation_station.io.alloc
```

**对应代码（行 471–515）：**

| 命令类型 | 判断条件 | 去向 | 是否进 RS |
|----------|----------|------|-----------|
| FLUSH | `risc_funct === FLUSH_CMD` | 直接刷新 TLB | 否 |
| COUNTER_OP | `risc_funct === COUNTER_OP` | 直接送 `counters.io.in` | 否 |
| CLKGATE_EN | `risc_funct === CLKGATE_EN` | 直接消费 | 否 |
| 其他（load/store/exec 等） | `otherwise` | `reservation_station.io.alloc` | **是** |

---

## 4. 调度发射：ReservationStation → 三大 Controller

ReservationStation 完成依赖检查后，通过三个独立的 issue 端口分发命令：

```text
reservation_station.io.issue.ld ──→ load_controller.io.cmd
reservation_station.io.issue.st ──→ store_controller.io.cmd
reservation_station.io.issue.ex ──→ ex_controller.io.cmd
```

### 三组连线的结构完全对称（以 ld 为例，行 346–349）

```scala
load_controller.io.cmd.valid := reservation_station.io.issue.ld.valid
reservation_station.io.issue.ld.ready := load_controller.io.cmd.ready
load_controller.io.cmd.bits := reservation_station.io.issue.ld.cmd
load_controller.io.cmd.bits.rob_id.push(reservation_station.io.issue.ld.rob_id)
```

**关键观察**：
- 三条连线**结构完全相同**，只是目标 controller 和 issue 端口不同。
- 每条命令携带 `rob_id`，用于完成后回报给 RS。
- RS 的 `ready` 由对应 controller 的 `cmd.ready` 驱动——**controller 繁忙时 RS 不会强行发射该类型命令**。

---

## 5. 数据通路：Controller ↔ Scratchpad（核心连线）

这是 Gemmini 数据搬运的核心连线，分成三组：

### 5.1 DMA 路径（LoadController / StoreController ↔ Scratchpad）

```text
load_controller.io.dma  ←→  spad.module.io.dma.read    (搬数据进来)
store_controller.io.dma ←→  spad.module.io.dma.write   (搬结果出去)
```

**对应代码（行 362–363）：**

```scala
spad.module.io.dma.read  <> load_controller.io.dma
spad.module.io.dma.write <> store_controller.io.dma
```

### 5.2 SRAM 路径（ExecuteController ↔ Scratchpad）

```text
ex_controller.io.srams.read  ←→  spad.module.io.srams.read   (从 SP 读操作数)
ex_controller.io.srams.write ←→  spad.module.io.srams.write  (向 SP 写回结果)
```

**对应代码（行 364–365）：**

```scala
ex_controller.io.srams.read  <> spad.module.io.srams.read
ex_controller.io.srams.write <> spad.module.io.srams.write
```

### 5.3 Accumulator 路径（ExecuteController ↔ Scratchpad）

```text
ex_controller.io.acc.read_req  ──→  spad.module.io.acc.read_req
spad.module.io.acc.read_resp   ──→  ex_controller.io.acc.read_resp
ex_controller.io.acc.write     ──→  spad.module.io.acc.write
```

**对应代码（行 366–368）：**

```scala
spad.module.io.acc.read_req  <> ex_controller.io.acc.read_req
ex_controller.io.acc.read_resp <> spad.module.io.acc.read_resp
ex_controller.io.acc.write   <> spad.module.io.acc.write
```

### 5.4 数据通路总图

```text
                    ┌──────────────────────────────────┐
                    │          Scratchpad               │
                    │  ┌──────────┐  ┌──────────────┐  │
   LoadController ──┤  │ DMA read │  │ SRAM banks   │  ├──── ExecuteController
  (io.dma)          │  └──────────┘  │  .read/.write │  │    (io.srams + io.acc)
                    │  ┌──────────┐  └──────────────┘  │
  StoreController ──┤  │DMA write │  ┌──────────────┐  │
  (io.dma)          │  └──────────┘  │ Accumulator   │  │
                    │                │ .read/.write  │  │
                    │                └──────────────┘  │
                    └──────────────────────────────────┘
```

---

## 6. Im2Col 单元与仲裁

Im2Col 也需要读取 Scratchpad SRAM，因此和 ExecuteController 共享读端口，需要仲裁：

```text
ex_controller.io.srams.read ──┐
                               ├──→ Arbiter ──→ spad.module.io.srams.read
im2col.io.sram_reads ─────────┘

spad.module.io.srams.read.resp ──→ ex_controller.io.srams.read.resp（广播）
                                ──→ im2col.io.sram_reads.resp（广播）
```

**对应代码（行 380–397）：**

```scala
val req_arb = Module(new Arbiter(new ScratchpadReadReq(...), 2))
req_arb.io.in(0) <> ex_read.req       // ExController 优先级 0（更高）
req_arb.io.in(1) <> im2col_read.req   // Im2Col 优先级 1
spad_read.req <> req_arb.io.out
// 响应广播给双方
```

Im2Col 与 ExecuteController 之间还有专用请求/响应接口：

```scala
im2col.io.req  <> ex_controller.io.im2col.req     // ExController 发请求给 Im2Col
ex_controller.io.im2col.resp <> im2col.io.resp     // Im2Col 返回结果给 ExController
```

---

## 7. 完成回报：三大 Controller → ReservationStation

三个 controller 完成命令后，通过 Arbiter 汇总回报给 RS：

```text
ex_controller.io.completed ────→ Arbiter.in(0)  (优先级最高)
load_controller.io.completed ──→ Arbiter.in(1)
store_controller.io.completed ─→ Arbiter.in(2)
                                     │
                         Arbiter.out ──→ reservation_station.io.completed
```

**对应代码（行 425–440）：**

```scala
val reservation_station_completed_arb = Module(new Arbiter(UInt(...), 3))
reservation_station_completed_arb.io.in(0).valid := ex_controller.io.completed.valid
reservation_station_completed_arb.io.in(0).bits  := ex_controller.io.completed.bits
reservation_station_completed_arb.io.in(1) <> load_controller.io.completed
reservation_station_completed_arb.io.in(2) <> store_controller.io.completed
// ...
reservation_station.io.completed.valid := reservation_station_completed_arb.io.out.valid
reservation_station.io.completed.bits  := reservation_station_completed_arb.io.out.bits
```

**关键观察**：
- 完成信号里只携带 `rob_id`（即 RS 条目编号），RS 凭此释放条目。
- ExController 仲裁优先级最高（in(0)）。
- `Arbiter.out.ready := true.B`——RS 无条件接收完成信号。

---

## 8. TLB 连线

```text
spad.module.io.tlb ←→ tlb.io.clients     (Scratchpad 的 DMA 引擎需要地址翻译)
tlb.io.ptw         ←→ io.ptw             (Page Table Walker，连到 Rocket 核)
tlb.io.exp.flush   ──→ spad.module.io.flush
```

**对应代码（行 174–185）：**

```scala
(tlb.io.clients zip outer.spad.module.io.tlb).foreach(t => t._1 <> t._2)
io.ptw <> tlb.io.ptw
spad.module.io.flush := tlb.io.exp.map(_.flush()).reduce(_ || _)
```

---

## 9. 性能计数器连线

`CounterController` 收集全系统各模块的事件信号：

```text
counters.io.event_io.collect(spad.module.io.counter)
counters.io.event_io.collect(tlb.io.counter)
counters.io.event_io.collect(reservation_station.io.counter)
counters.io.event_io.collect(load_controller.io.counter)
counters.io.event_io.collect(store_controller.io.counter)
counters.io.event_io.collect(ex_controller.io.counter)
counters.io.event_io.collect(im2col.io.counter)
```

此外还有顶层组合的忙碌周期事件（行 450–466），监控 ld/st/ex 的各种并行组合。

计数器的访问命令通过 `io.resp` 返回给 CPU：

```scala
io.resp <> counters.io.out
```

---

## 10. 全局控制信号

### busy 信号

```scala
io.busy := raw_cmd.valid ||
           loop_conv_unroller_busy ||
           loop_matmul_unroller_busy ||
           reservation_station.io.busy ||
           spad.module.io.busy ||
           unrolled_cmd.valid ||
           loop_cmd.valid ||
           conv_cmd.valid
```

**含义**：流水线中只要有**任何一级**不空闲，就告诉 CPU "我还在忙"。

### interrupt 信号

```scala
io.interrupt := tlb.io.exp.map(_.interrupt).reduce(_ || _)
```

仅由 TLB 异常触发。

### clock gating

```scala
val gated_clock = ClockGate(clock, clock_en_reg, ...)
outer.spad.module.clock := gated_clock
// reservation_station, 三大 controller, im2col 都用 withClock(gated_clock) 实例化
```

---

## 11. Diplomacy 层连线（Gemmini 外壳类）

在 `Gemmini` 类（非 `GemminiModule`）中：

```scala
val spad = LazyModule(new Scratchpad(config))       // Scratchpad 是 LazyModule
override val tlNode  = spad.id_node                 // TileLink 节点挂接
override val atlNode = spad.id_node                 // 二选一，取决于 use_dedicated_tl_port
```

如果启用外部 TL 存储（`use_ext_tl_mem`），还有 `spad_read_nodes` 和 `spad_write_nodes` 两组 TLClientNode。

---

## 12. 完整接线总图

```text
  ┌─────────┐
  │  CPU     │
  │ (Rocket) │
  └────┬─────┘
       │ io.cmd (RoCCCommand)            io.resp (计数器结果)
       │                                     ↑
  ┌────▼──────────────────────────────────────┴──────────────────────┐
  │                        GemminiModule                             │
  │                                                                  │
  │  io.cmd → [raw_cmd_q] → raw_cmd                                 │
  │                            │                                     │
  │                    ┌───────▼────────┐                            │
  │                    │   LoopConv     │ ←── RS.conv_*_completed    │
  │                    └───────┬────────┘                            │
  │                    ┌───────▼────────┐                            │
  │                    │  LoopMatmul    │ ←── RS.matmul_*_completed  │
  │                    └───────┬────────┘                            │
  │                      [Queue] → unrolled_cmd                      │
  │                            │                                     │
  │          ┌─────────────────┼──────────────────┐                  │
  │          │ flush/counter/  │ otherwise         │                  │
  │          │ clkgate 直接处理│                   │                  │
  │          ▼                 ▼                   │                  │
  │  [TLB/Counters]  ┌────────────────┐           │                  │
  │                   │ Reservation    │           │                  │
  │                   │   Station      │           │                  │
  │                   │                │           │                  │
  │                   │ .alloc ←───────┘           │                  │
  │                   │                            │                  │
  │                   │ .issue.ld ──→ LoadController ──┐             │
  │                   │ .issue.st ──→ StoreController ─┤             │
  │                   │ .issue.ex ──→ ExecController ──┤             │
  │                   │                                │             │
  │                   │ .completed ←── [Arbiter] ◄─────┘             │
  │                   └────────────────┘                             │
  │                                                                  │
  │  ┌──────────────────────────────────────────────────────┐       │
  │  │                   Scratchpad                          │       │
  │  │                                                       │       │
  │  │  .dma.read  ←→ LoadController.io.dma                 │       │
  │  │  .dma.write ←→ StoreController.io.dma                │       │
  │  │  .srams.read  ←→ [Arbiter] ← ExecCtrl + Im2Col      │       │
  │  │  .srams.write ←→ ExecController.io.srams.write       │       │
  │  │  .acc.read/write ←→ ExecController.io.acc            │       │
  │  │  .tlb ←→ FrontendTLB ←→ io.ptw (→ Rocket PTW)       │       │
  │  └──────────────────────────────────────────────────────┘       │
  │                                                                  │
  │  [Im2Col] ←→ ExecController.io.im2col                          │
  │  [CounterController] ← collect(所有模块.io.counter)              │
  │                                                                  │
  └──────────────────────────────────────────────────────────────────┘
```

---

## 13. 第二遍阅读自检

读完上面的接线梳理后，用这些问题检验理解：

1. **命令从 CPU 到 controller 经过几级缓冲/展开？**
   → 3 级：`raw_cmd_q` → `LoopConv` → `LoopMatmul`（+ Queue），最终到 RS。

2. **LoopConv/LoopMatmul 为什么需要 RS 的完成反馈？**
   → 它们在展开高级循环时需要知道前序操作（如某个 load tile）是否已完成，以决定何时发出下一条命令（实现双缓冲/重叠调度）。

3. **三大 controller 的数据通路有何本质区别？**
   → Load/Store 走 **DMA 路径**（`spad.io.dma`），涉及虚实地址翻译和 TileLink 总线事务；Execute 走 **SRAM 直连路径**（`spad.io.srams` + `spad.io.acc`），延迟更低。

4. **为什么完成信号要走 Arbiter 而不是直连？**
   → RS 的 `completed` 端口只有一个入口，三个 controller 可能同周期都完成，需要仲裁。每周期只能回报一个完成，其他排队等下一周期。

5. **Im2Col 和 ExController 是什么关系？**
   → Im2Col 是 ExController 的辅助模块：ExController 发请求让 Im2Col 重新组织数据（模拟 im2col 变换），Im2Col 需要独立读取 Scratchpad SRAM，因此和 ExController 的读端口通过 Arbiter 共享。

6. **TLB 为什么挂在 Scratchpad 上而不是 Controller 上？**
   → 因为实际需要地址翻译的是 Scratchpad 内部的 DMA 引擎（它把虚拟地址翻成物理地址发到总线上），不是 Controller 本身。

---

## 14. 下一步

第三遍阅读（Day 2 最后一步）将沿着一条具体命令，从 `io.cmd` 入口追踪到 controller 执行，把这张接线图变成一条活的时序故事。
