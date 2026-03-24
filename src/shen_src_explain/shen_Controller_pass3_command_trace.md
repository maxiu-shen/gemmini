# Controller.scala 第三遍阅读：追踪 tiled_conv_auto 从 C API 到硬件执行

> **阅读目标**：以 `tiled_conv_auto()` 为起点，追踪一次完整的卷积操作如何从 C 层面的函数调用，变成 7 条 LOOP_CONV 配置指令，再被 LoopConv 硬件展开成底层 mvin/preload/compute/mvout，最终进入三大 Controller 执行。
>
> **涉及源文件**：`gemmini.h`、`Controller.scala`、`LoopConv.scala`、`ReservationStation.scala`、`GemminiISA.scala`

---

## 全景：一次卷积操作的完整生命周期

```text
软件层（C API）                硬件前端（LoopConv）           硬件后端（RS + Controllers）
─────────────                 ─────────────────            ──────────────────────────

tiled_conv_auto()             LoopConv 模块内部
       │                            │
  计算 tiling 参数             接收 7 条配置指令
       │                            │
  发出 7 条 RoCC 指令:         解析参数 → configured=true
  ┌─ CONFIG_1 (funct=16)            │
  ├─ CONFIG_2 (funct=17)      并行启动 5 个子状态机:
  ├─ CONFIG_3 (funct=18)      ┌─ ld_bias ──→ CONFIG_LOAD + LOAD3_CMD ──┐
  ├─ CONFIG_4 (funct=19)      ├─ ld_input ─→ CONFIG_LOAD + LOAD_CMD ───┤
  ├─ CONFIG_5 (funct=20)      ├─ ld_weight → CONFIG_LOAD + LOAD2_CMD ──┤→ Arbiter
  ├─ CONFIG_6 (funct=21)      ├─ ex ───────→ CONFIG_EX + PRELOAD      ┤   │
  └─ LOOP_CONV_WS (funct=15)  │              + COMPUTE ────────────────┤   │
                               └─ st ──────→ CONFIG_STORE + STORE_CMD ─┘   │
                                                                           ▼
                                                            conv_cmd (展开后的底层 RISC 命令)
                                                                           │
                                                                  LoopMatmul（透传）
                                                                           │
                                                                     unrolled_cmd
                                                                           │
                                                                   ReservationStation
                                                                     │     │     │
                                                                    ld    ex    st
                                                                     │     │     │
                                                                 LoadCtrl ExCtrl StoreCtrl
```

---

## 第 1 站：C API — tiled_conv_auto() 发出 7 条 RoCC 指令

```c
// gemmini.h 行 2861
// tiled_conv_auto() 是软件层入口，负责：
//   1. 根据输入/输出尺寸和硬件参数（DIM, ACC_ROWS, BANK_NUM 等）计算 tiling 切块方案
//   2. 将切块后的参数编码进 7 条 LOOP_CONV 指令，通过 RoCC 接口发给 Gemmini
_STATIC void tiled_conv_auto(
    int batch_size, int in_row_dim, int in_col_dim, int in_channels,
    int out_channels, int out_row_dim, int out_col_dim,
    int stride, int input_dilation, int kernel_dilation, int padding, int kernel_dim,
    ...
)
```

它最终调用 `gemmini_loop_conv_ws` 宏，发出 7 条 RoCC 指令：

```c
// gemmini.h 行 385-401
// gemmini_loop_conv_ws 宏：将卷积参数编码成 7 条 RoCC 指令
// 每条指令的 funct 字段决定了它的类型
gemmini_loop_conv_ws(...) {
  ROCC_INSTRUCTION_RS1_RS2(XCUSTOM_ACC, ..., k_LOOP_CONV_WS_CONFIG_1)  // funct=16: batch/通道/维度
  ROCC_INSTRUCTION_RS1_RS2(XCUSTOM_ACC, ..., k_LOOP_CONV_WS_CONFIG_2)  // funct=17: kernel/pool/内层边界
  ROCC_INSTRUCTION_RS1_RS2(XCUSTOM_ACC, ..., k_LOOP_CONV_WS_CONFIG_3)  // funct=18: kernel 切块/pad
  ROCC_INSTRUCTION_RS1_RS2(XCUSTOM_ACC, ..., k_LOOP_CONV_WS_CONFIG_4)  // funct=19: stride/输出切块
  ROCC_INSTRUCTION_RS1_RS2(XCUSTOM_ACC, weights, output, k_LOOP_CONV_WS_CONFIG_5) // funct=20: 地址
  ROCC_INSTRUCTION_RS1_RS2(XCUSTOM_ACC, bias, input, k_LOOP_CONV_WS_CONFIG_6)     // funct=21: 地址
  ROCC_INSTRUCTION_RS1_RS2(XCUSTOM_ACC, flags, ctrl, k_LOOP_CONV_WS)              // funct=15: 启动！
}
```

**关键理解**：前 6 条是纯配置（写参数寄存器），第 7 条 `LOOP_CONV_WS` 才是"启动"信号。

---

## 第 2 站：Controller 入口 — 7 条指令逐个进入

7 条指令通过 RoCC 接口逐个到达 `io.cmd`，经 `raw_cmd_q` 缓冲后送给 LoopConv：

```scala
// Controller.scala 行 237-245
// RoCC 命令包装成 GemminiCmd 入队
// Queue 接口：enq = 入队（写入端），deq = 出队（读出端），均为 DecoupledIO（valid/ready/bits 握手）
val raw_cmd_q = Module(new Queue(new GemminiCmd(...), entries = 2)) // 2 深缓冲队列
raw_cmd_q.io.enq.valid := io.cmd.valid          // CPU 命令有效时入队
io.cmd.ready := raw_cmd_q.io.enq.ready          // 队列满时反压 CPU
raw_cmd_q.io.enq.bits.cmd := io.cmd.bits        // 原始 RoCCCommand

// val 别名：raw_cmd 和 raw_cmd_q.io.deq 指向同一个硬件对象，不产生新连线
// 这里不用 <> 是因为 raw_cmd 的消费者不是单一模块，而是后续由 LoopConv 内部复杂条件分别驱动 .valid/.ready/.bits
val raw_cmd = raw_cmd_q.io.deq
```

```scala
// Controller.scala 行 251-260
// raw_cmd 直接连入 LoopConv 模块的 io.in
val (conv_cmd, loop_conv_unroller_busy) =
  if (has_loop_conv)
    withClock(gated_clock) {
      LoopConv(raw_cmd, ...)  // ← 7 条指令从这里进入 LoopConv
    }
  else (raw_cmd, false.B)
```

---

## 第 3 站：LoopConv 顶层 — 识别 LOOP 指令 vs 非 LOOP 指令

LoopConv 模块内部首先判断命令类型：

```scala
// LoopConv.scala 行 1242-1253
// ─── 判断当前命令是否属于 LOOP_CONV 系列 ───
val is_loop_run_cmd = cmd.bits.cmd.inst.funct === LOOP_CONV_WS      // funct=15：启动命令
val is_loop_config_cmd =
  cmd.bits.cmd.inst.funct >= LOOP_CONV_WS_CONFIG_1 &&               // funct=16
  cmd.bits.cmd.inst.funct <= LOOP_CONV_WS_CONFIG_6                  // funct=21
val is_loop_cmd = is_loop_run_cmd || is_loop_config_cmd              // 两类合称 "loop 命令"

// 输出选择：如果有配置好的 loop 正在展开，输出展开后的命令；否则透传非 loop 命令
io.out.valid := Mux(
  loop_configured,
  unrolled_cmd.valid,                                // 正在展开 → 输出展开后的底层命令
  cmd.valid && !is_loop_config_cmd && !is_loop_run_cmd // 非 loop 命令 → 直接透传
)

// 非 loop 命令可以直接通过，loop 命令被 LoopConv 消费（不往下传）
cmd.ready := Mux(
  is_loop_cmd,
  !loop_being_configured.configured, // loop 命令：只在有空闲 loop 槽时才接收
  !loop_configured && io.out.ready   // 非 loop 命令：只在没有 loop 展开时才透传
)
```

**关键理解**：
- LOOP_CONV_WS_CONFIG_1~6 和 LOOP_CONV_WS 这 7 条指令**不会**出现在 LoopConv 的输出端，它们被 LoopConv 内部消费。
- 展开后产生的底层 RISC 命令（LOAD_CMD、PRELOAD_CMD 等）才从输出端送出。
- 非 loop 命令（比如裸的 mvin/compute）在没有 loop 运行时直接透传。

---

## 第 4 站：LoopConv 配置阶段 — 6 条 CONFIG 写入参数寄存器

LoopConv 内部维护 2 个 loop 槽（双缓冲），每个槽是一个 `LoopConvState`：

```scala
// LoopConv.scala 行 1194-1204
// ─── 双缓冲：2 个 loop 槽交替工作 ───
val concurrent_loops = 2                          // Scala 编译期常量，不是硬件信号
val loops = Reg(Vec(concurrent_loops,             // Reg(Vec(N, T))：N 组寄存器，每组包含 T 的所有字段
  new LoopConvState(...)))
val head_loop_id = RegInit(0.U)                   // RegInit：带复位初始值的寄存器，复位后=0
val tail_loop_id = (~head_loop_id).asUInt         // 1-bit 取反：0↔1，组合逻辑（非寄存器）

val loop_being_configured = loops(                // loops(idx)：用硬件信号动态索引，综合成 MUX
  Mux(head_loop.configured, tail_loop_id, head_loop_id)
  // head 已配置完（正在执行）→ 新配置写入 tail 槽（提前配置下一次卷积）
  // head 未配置         → 先配置 head 槽
)
```

6 条 CONFIG 指令逐个到达，每条往 `loop_being_configured` 中写入一部分参数。每条指令的 `rs1`（64bit）和 `rs2`（64bit）共 128bit 载荷，6 条合计 768bit，足以编码一次卷积的全部参数（维度、地址、pad、stride 等）：

```scala
// LoopConv.scala 行 1279-1377
// ─── 按 funct 解析并存储卷积参数 ───
when(cmd.valid && is_loop_cmd && !loop_being_configured.configured) {
  switch (cmd.bits.cmd.inst.funct) {

    is (LOOP_CONV_WS_CONFIG_1) {                 // funct=16
      // rs1 编码：out_channels | in_channels | in_row_dim | batch_size
      loop_being_configured.outer_bounds.out_channels := cmd.bits.cmd.rs1(63, 48)
      loop_being_configured.outer_bounds.in_channels  := cmd.bits.cmd.rs1(47, 32)
      loop_being_configured.outer_bounds.in_row_dim   := cmd.bits.cmd.rs1(31, 16)
      loop_being_configured.outer_bounds.batch_size   := cmd.bits.cmd.rs1(15, 0)
      // rs2 编码：padding | stride | out_col_dim | pool_out_row_dim | out_row_dim
      // ...
    }

    is (LOOP_CONV_WS_CONFIG_5) {                 // funct=20
      // 权重和输出的 DRAM 地址
      loop_being_configured.weights_dram_addr := cmd.bits.cmd.rs1
      loop_being_configured.output_dram_addr  := cmd.bits.cmd.rs2
    }

    is (LOOP_CONV_WS_CONFIG_6) {                 // funct=21
      // bias 和输入的 DRAM 地址
      loop_being_configured.bias_dram_addr  := cmd.bits.cmd.rs1
      loop_being_configured.input_dram_addr := cmd.bits.cmd.rs2
    }

    is (LOOP_CONV_WS) {                          // funct=15 ← 最后一条：启动！
      loop_being_configured.no_bias := cmd.bits.cmd.rs1(0)
      loop_being_configured.wrot180 := cmd.bits.cmd.rs1(1)
      loop_being_configured.no_pool := cmd.bits.cmd.rs2(0)
      loop_being_configured.activation := cmd.bits.cmd.rs2(4,3)
      // ...
      loop_being_configured.configured := true.B  // ★ 标记配置完成，触发展开 ★
    }
  }
}
```

**关键理解**：
- 6 条 CONFIG 只是往寄存器写数据，每条 1 周期消费，**没有任何实际硬件操作发生**。
- 第 7 条 `LOOP_CONV_WS` 设置 `configured := true.B`，这才真正启动展开。
- 双缓冲意味着：head loop 正在执行时，tail loop 可以提前接收下一次卷积的配置，消除两次卷积之间的配置延迟。

---

## 第 5 站：LoopConv 展开阶段 — 5 个子状态机并行产生底层命令

`configured` 置位后的下一个周期，5 个子模块的启动条件逐级满足：

```scala
// LoopConv.scala 行 1206-1212
// ─── LoopConv 内部的 5 个子命令生成器 ───
val ld_bias    = Module(new LoopConvLdBias(...))    // 加载 bias → 产生 CONFIG_LOAD + LOAD3_CMD
val ld_input   = Module(new LoopConvLdInput(...))   // 加载输入 → 产生 CONFIG_LOAD + LOAD_CMD
val ld_weights = Module(new LoopConvLdWeight(...))  // 加载权重 → 产生 CONFIG_LOAD + LOAD2_CMD
val ex         = Module(new LoopConvExecute(...))   // 计算     → 产生 CONFIG_EX + PRELOAD + COMPUTE
val st         = Module(new LoopConvSt(...))        // 存储输出 → 产生 CONFIG_STORE + STORE_CMD
```

### 5 个子模块的启动顺序和依赖关系

```scala
// LoopConv.scala 行 1394, 1419, 1438, 1461-1462, 1485
// ─── 子模块启动条件 ───

// 三个 load 子模块：configured 后立即可以启动，互相不依赖
ld_bias.io.req.valid    := !loop.ld_bias_started    && loop.configured
ld_input.io.req.valid   := !loop.ld_input_started   && loop.configured
ld_weights.io.req.valid := !loop.ld_weights_started && loop.configured

// execute：必须等三个 load 都已启动（注意是 started，不是 completed）
ex.io.req.valid := !loop.ex_started &&
  loop.ld_bias_started &&           // bias 加载已启动
  loop.ld_input_started &&          // 输入加载已启动
  loop.ld_weights_started &&        // 权重加载已启动
  loop.configured

// store：必须等 execute 已启动
st.io.req.valid := !loop.st_started && loop.ex_started && loop.configured
```

```text
启动依赖关系图（粗粒度，基于 started 标志）：

  configured ──→ ld_bias ────┐
              ──→ ld_input ──┤──→ ex ──→ st
              ──→ ld_weight ─┘
```

完整的启动时序：

```text
configured=true（第 7 条指令到达的下一个周期）
   ↓
ld_bias / ld_input / ld_weights 三路并行启动（各自 req.fire）
   ↓ 三路都 started 后
ex 启动（但逐 tile 等待 load 数据就绪，见下文）
   ↓ ex started 后
st 启动（但逐 tile 等待 ex 数据就绪）
   ↓ 5 路全部 idle
all_completed → reset loop 槽 → 切换 head/tail 双缓冲
```

### 子模块之间的细粒度同步

粗粒度的 `started` 只控制子模块何时从 idle 进入工作状态。执行过程中还有更精细的 **tile 级同步**——execute 每产生一条 preload/compute 之前，都要确认对应的 load 数据已搬完：

```scala
// LoopConv.scala 行 1273-1276
// ─── execute 在每个 tile 级别等待 load 完成 ───
ex.io.lda_completed := (ld_input.io.loop_id =/= ex.io.loop_id) || ld_input.io.idle
ex.io.ldb_completed := (ld_weights.io.loop_id =/= ex.io.loop_id) || ld_weights.io.idle
ex.io.ldd_completed := (ld_bias.io.loop_id =/= ex.io.loop_id) || ld_bias.io.idle

// store 等待 execute 完成
st.io.ex_completed  := (ex.io.loop_id =/= st.io.loop_id) || ex.io.idle
```

```scala
// LoopConvExecute 内部 行 723, 730
// ─── execute 只有在对应的 load 全部完成后才发命令 ───
val ld_ahead = io.lda_completed && io.ldb_completed && io.ldd_completed
command_p.io.in.valid := state =/= idle && !skip_iteration && ld_ahead  // 必须 ld_ahead
```

---

## 第 6 站：子模块如何产生底层 RISC 命令（以 ld_input 和 ex 为例）

### 6a. LoopConvLdInput — 产生 mvin 命令

每个 load 子模块内部是一个简单状态机：`idle → config → ld → ld → ... → idle`

```scala
// LoopConv.scala 行 252-256, 314-361
// ─── LoopConvLdInput 的状态机 ───
object State extends ChiselEnum {
  val idle, config, ld = Value   // 三个状态
}

// config 状态：产生一条 CONFIG_CMD（配置 load 通道的 stride/scale 等）
val config_cmd = Wire(new RoCCCommand)
config_cmd.inst.funct := CONFIG_CMD              // funct=0
config_cmd_rs1.stride := input_spad_stride       // 告诉 LoadController SP 内部 stride
config_cmd_rs1.state_id := 0.U                   // state_id=0 对应 input 通道
config_cmd_rs1._unused := 1.U                    // rs1[1:0]=1 → CONFIG_LOAD 类型

// ld 状态：产生 LOAD_CMD（搬一块 input tile 从 DRAM 到 Scratchpad）
val mvin_cmd = Wire(new RoCCCommand)
mvin_cmd.inst.funct := LOAD_CMD                  // funct=2
// rs1 = DRAM 源地址, rs2 = {行数, 列数, SP 目标地址}

// 状态机：config 发完切换到 ld，ld 中遍历 (batch, irow, icol, ich)，全部遍历完回到 idle
when(command_p.io.in.fire) {
  when (state === config) {
    state := ld                                   // config → ld
  }.otherwise {
    // 四层嵌套循环：ich → icol → irow → batch
    val next_ich  = sFloorAdd(ich, ich_it, ichs.zext, 0.S)
    val next_icol = sFloorAdd(icol, I.asUInt, ...)
    val next_irow = sFloorAdd(irow, 1.U << downsample, ...)
    val next_b    = sFloorAdd(b, b_it, batches.zext, 0.S, ...)
    // 全部归零时 → idle，否则继续 ld
    state := Mux(all_done, idle, ld)
  }
}
```

### 6b. LoopConvExecute — 产生 preload + compute 命令

Execute 子模块的状态机更复杂：`idle → config → pre → comp → pre → comp → ... → idle`

```scala
// LoopConv.scala 行 614-618, 711-721
// ─── LoopConvExecute 的状态机 ───
object State extends ChiselEnum {
  val idle, config, pre, comp = Value     // 四个状态
}

// pre 状态：产生 PRELOAD_CMD（预加载权重到阵列）
val pre_cmd = Wire(new RoCCCommand)
pre_cmd.inst.funct := PRELOAD_CMD                 // funct=6
// rs1 = {K, J, b_addr(权重 SP 地址)}, rs2 = {I, J, c_addr(累加器地址)}

// comp 状态：产生 COMPUTE_AND_FLIP/STAY_CMD（启动阵列计算）
val comp_cmd = Wire(new RoCCCommand)
comp_cmd.inst.funct := Mux(
  new_weights,
  COMPUTE_AND_FLIP_CMD,                            // funct=4：新权重，需要翻转
  COMPUTE_AND_STAY_CMD                             // funct=5：同一权重，保持
)
// rs1 = {I, K, a_addr(输入 SP 地址)}, rs2 = {I, J, GARBAGE_ADDR}

// 状态转换：pre 和 comp 交替进行，每次遍历一个 (ocol, orow, b, kch, kcol, krow, och) tile
when(command_p.io.in.fire || skip_iteration) {
  when (state === config) { state := pre }
  .elsewhen (state === pre) { state := comp }     // preload → compute
  .otherwise {
    // 七层嵌套循环：ocol → orow → b → kch → kcol → krow → och
    state := Mux(all_done, idle, pre)              // compute → 下一个 preload（或 idle）
  }
}
```

---

## 第 7 站：5 路仲裁 → conv_cmd 输出

5 个子模块产生的底层命令通过一个 5 输入 Arbiter 汇聚成单一输出：

```scala
// LoopConv.scala 行 1220-1226
// ─── 5 路仲裁器：决定每周期输出哪个子模块的命令 ───
val arb = Module(new Arbiter(new RoCCCommand, 5))
arb.io.in(0) <> st.io.cmd          // 优先级 0（最高）：store
arb.io.in(1) <> ex.io.cmd          // 优先级 1：execute
arb.io.in(2) <> ld_bias.io.cmd     // 优先级 2：load bias
arb.io.in(3) <> ld_weights.io.cmd  // 优先级 3：load weights
arb.io.in(4) <> ld_input.io.cmd    // 优先级 4（最低）：load input
val unrolled_cmd = arb.io.out       // 仲裁后的单一输出
```

**优先级设计意图**：store > execute > load，确保已计算完的数据尽快搬出，避免占用 Accumulator 空间。

仲裁输出连到 LoopConv 的 `io.out`：

```scala
// LoopConv.scala 行 1246-1250
// ─── 输出打标记：from_conv_fsm = true ───
io.out.bits.cmd := Mux(loop_configured, unrolled_cmd.bits, cmd.bits.cmd)
io.out.bits.from_conv_fsm := Mux(loop_configured, true.B, cmd.bits.from_conv_fsm)
// from_conv_fsm 标记让 RS 完成时能回报给 LoopConv，告知进度
```

---

## 第 8 站：LoopConv 输出 → LoopMatmul（透传） → unrolled_cmd

```scala
// Controller.scala 行 262-269
// ─── LoopConv 输出的底层 RISC 命令进入 LoopMatmul ───
val (loop_cmd, loop_matmul_unroller_busy, loop_completed) =
  withClock(gated_clock) {
    LoopMatmul(
      if (has_loop_conv) conv_cmd else raw_cmd,  // conv_cmd = LoopConv 的输出
      ...
    )
  }
// 底层 RISC 命令（LOAD_CMD/PRELOAD_CMD/COMPUTE/STORE_CMD）不是 LOOP_WS 系列
// 因此 LoopMatmul 不拦截，直接透传
val unrolled_cmd = Queue(loop_cmd)
```

---

## 第 9 站：命令分类 → RS → 三大 Controller

（与简单命令追踪相同，不再重复代码）

展开后的底层命令在 Controller 的 `when (unrolled_cmd.valid)` 块中被分发：

| LoopConv 子模块 | 产生的 funct | RS 分类 | 目标 Controller |
|-----------------|-------------|---------|----------------|
| ld_bias | CONFIG_CMD(CONFIG_LOAD) | `is_load` → ld 队列 | LoadController |
| ld_bias | LOAD3_CMD (funct=14) | `is_load` → ld 队列 | LoadController |
| ld_input | CONFIG_CMD(CONFIG_LOAD) | `is_load` → ld 队列 | LoadController |
| ld_input | LOAD_CMD (funct=2) | `is_load` → ld 队列 | LoadController |
| ld_weights | CONFIG_CMD(CONFIG_LOAD) | `is_load` → ld 队列 | LoadController |
| ld_weights | LOAD2_CMD (funct=1) | `is_load` → ld 队列 | LoadController |
| ex | CONFIG_CMD(CONFIG_EX) | `is_ex` → ex 队列 | ExecuteController |
| ex | PRELOAD_CMD (funct=6) | `is_ex` → ex 队列 | ExecuteController |
| ex | COMPUTE_AND_FLIP/STAY | `is_ex` → ex 队列 | ExecuteController |
| st | CONFIG_CMD(CONFIG_STORE) | `is_store` → st 队列 | StoreController |
| st | STORE_CMD (funct=3) | `is_store` → st 队列 | StoreController |

---

## 两级调度协同：LoopConv 管生成，RS 管并行

展开后的底层命令经过**两级调度**，各管不同粒度：

| 层次 | 负责者 | 职责 | 粒度 |
|------|--------|------|------|
| 生成顺序 | LoopConv | 5 个子状态机按 tile 级依赖产生命令，不会产生因果错误的序列 | tile 级 |
| 发射并行 | RS | 基于 SP/Acc 地址重叠做依赖检查，允许无冲突的 ld/ex/st 并行发射 | 指令级 |

**具体例子**：假设 LoopConv 按顺序产出 `load_A(SP[0,15]) → load_B(SP[100,115]) → preload → compute → store_A → load_C(SP[0,15])`：

```text
LoopConv 逐条产出:  load_A → load_B → preload → compute → store_A → load_C

如果没有 RS（纯串行）:
  |--load_A--|--load_B--|--preload--|--compute--|--store_A--|--load_C--|
  总时间 = 各命令时间之和

有 RS 后（三队列并行发射）:
  LoadCtrl:  |==load_A==|==load_B==|                        |==load_C==|
  ExecCtrl:              |.wait.|==preload==|===compute===|
  StoreCtrl:                                    |..wait..|==store_A==|
             ─────────────────────────────────────────────────────────→ 时间
                         ↑                      ↑
                    load_A 和 load_B        store_A 和 load_C
                    背靠背并行发射           同时并行执行
```

并行发生在两个地方：

1. **load_A 与 load_B 背靠背**：两者写入 SP 不同地址区域（SP[0,15] vs SP[100,115]），RS 检测到无地址重叠，不阻塞
2. **store_A 与 load_C 重叠**：compute 完成后，store_A（DMA 写，从 Acc[200,215] 搬出结果）和 load_C（DMA 读，往 SP[0,15] 搬入下一轮输入）走不同数据通路，RS 检测到无依赖冲突，同时发射给不同 Controller

如果没有 RS，命令只能严格串行；有了 RS，ld/ex/st 三条流水线可以重叠执行——这就是 **decoupled access/execute** 的核心收益。

---

## 第 10 站：完成回报闭环 — Controller → RS → LoopConv

Controller 完成后 RS 回报，RS 再通知 LoopConv：

```scala
// LoopConv.scala 行 1229-1235
// ─── LoopConv 跟踪 RS 中各队列的占用数 ───
val ld_utilization = RegInit(0.U(...))
val st_utilization = RegInit(0.U(...))
val ex_utilization = RegInit(0.U(...))

// 每发出一条 ld 命令 +1，RS 回报一条 ld 完成 -1
ld_utilization := ld_utilization +& (ld_bias.io.cmd.fire || ld_weights.io.cmd.fire || ld_input.io.cmd.fire) -& io.ld_completed
st_utilization := st_utilization +& st.io.cmd.fire -& io.st_completed
ex_utilization := ex_utilization +& ex.io.cmd.fire -& io.ex_completed
```

```scala
// LoopConv.scala 行 1266-1270
// ─── 当 RS 队列接近满时，暂停子模块的命令输出（反压） ───
ld_bias.io.rob_overloaded    := ld_utilization >= max_lds.U
ld_input.io.rob_overloaded   := ld_utilization >= max_lds.U
ld_weights.io.rob_overloaded := ld_utilization >= max_lds.U
ex.io.rob_overloaded         := ex_utilization >= max_exs.U
st.io.rob_overloaded         := st_utilization >= max_sts.U
```

当一个 loop 的全部 5 个子模块都完成时，loop 槽被释放，下一个 loop 可以开始：

```scala
// LoopConv.scala 行 1517-1519
// ─── loop 全部完成后释放槽位 ───
when (head_loop.running && head_loop.all_completed()) {
  head_loop.reset()               // 清空状态
  head_loop_id := ~head_loop_id   // 切换 head/tail，实现双缓冲交替
}
```

---

## LoopConv 展开一次 3×3 卷积会产生多少条底层命令？

以一个简化例子估算：`3×3 kernel, 16 in_ch, 16 out_ch, 8×8 output, batch=1, DIM=16`

| 子模块 | 产生的命令数 | 说明 |
|--------|------------|------|
| ld_bias | 1 config + ~4 load3 | 16 out_ch / DIM = 1 组 × 8×8 output tiles |
| ld_input | 1 config + ~100 load | 遍历 (irow, icol, ich) tiles |
| ld_weights | 1 config + ~9 load2 | 3×3×(16/16)×(16/16) = 9 tiles |
| ex | 1 config + ~9×(pre+comp) | 每个 kernel tile 一对 preload+compute |
| st | 1 config + ~4 store | 16 out_ch / DIM × output tiles |

总计约 **130+ 条底层 RISC 命令**，全部由 LoopConv 硬件自动展开，软件只需发 7 条指令。

---

## 与简单 mvin/compute/mvout 场景的对比

| 维度 | 手动底层命令 | tiled_conv_auto |
|------|------------|-----------------|
| 软件发出的指令数 | 每个 tile 手动 4 条 | 7 条（配置 + 启动） |
| 硬件展开 | 无（命令透传 LoopConv） | LoopConv 展开成 100+ 条 |
| load/execute 重叠 | 需软件手动编排双缓冲 | LoopConv 5 路并行 + 双缓冲自动调度 |
| RS 反压 | 软件需自行避免溢出 | LoopConv 跟踪 utilization 自动暂停 |

---

## 第三遍阅读自检

1. `tiled_conv_auto` 最终发出几条 RoCC 指令？这些指令在 LoopConv 内部如何被消费？
2. LoopConv 内部 5 个子模块的启动条件分别是什么？为什么 execute 必须等三个 load 都"已启动"？
3. 5 路 Arbiter 的优先级为什么是 store > execute > load？
4. `from_conv_fsm` 标记有什么用？RS 完成后这个标记如何帮助 LoopConv 跟踪进度？
5. LoopConv 的双缓冲（`concurrent_loops = 2`）解决了什么问题？
6. LoopConv 如何防止向 RS 塞入过多命令导致溢出？
7. LoopConv 和 RS 在命令排序上各自负责什么？为什么两级都不可少？
