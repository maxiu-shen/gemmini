# Day 3 阅读笔记：ReservationStation 与三大 Controller

> **阅读目标**：深入 RS 内部依赖管理机制，理解三大 Controller 的状态机与职责边界。
>
> **涉及源文件**：`ReservationStation.scala`、`LoadController.scala`、`StoreController.scala`、`ExecuteController.scala`

---

## 一、ReservationStation 内部机制

> Day 2 已理解 RS 的宏观角色（缓冲命令、检查依赖、并行发射）。本节深入**怎么做**。

### 1.1 三个队列的物理组织

RS 维护三个独立的条目数组（行 122-126）：

```scala
val entries_ld = Reg(Vec(reservation_station_entries_ld, UDValid(new Entry)))
val entries_ex = Reg(Vec(reservation_station_entries_ex, UDValid(new Entry)))
val entries_st = Reg(Vec(reservation_station_entries_st, UDValid(new Entry)))
```

`Reg(Vec(n, t))` = n 个类型 t 的寄存器数组。**不用 Queue（FIFO）**是因为 RS 需要乱序发射（任意就绪 entry 可被选中）和任意位置释放，本质是**关联表**而非先进先出队列。新 entry 分配到任意空闲槽，不一定在末尾。

每个 Entry 的核心字段（行 86-116）：

| 字段 | 类型 | 作用 |
|------|------|------|
| `q` | UInt(2.W) | 队列类型：ldq=0, exq=1, stq=2 |
| `opa / opb` | UDValid(OpT) | 两个操作数的 SP/Acc 地址范围（start, end, wraps_around） |
| `opa_is_dst` | Bool | opa 是写目标（store 时为 true） |
| `valid`（外层 UDValid） | Bool | 槽位是否被占用（`false` = 空闲可分配） |
| `issued` | Bool | 是否已发射给 Controller（`valid && !issued` = 等待中；`valid && issued` = 执行中） |

> Entry 生命周期：`空闲(!valid) → 分配(valid, !issued) → 发射(valid, issued) → 完成 → 空闲(!valid)`
| `complete_on_issue` | Bool | CONFIG 类指令，发射即完成 |
| `deps_ld` | Vec(N, Bool) | 对 ld 队列中各条目的依赖 |
| `deps_ex` | Vec(N, Bool) | 对 ex 队列中各条目的依赖 |
| `deps_st` | Vec(N, Bool) | 对 st 队列中各条目的依赖 |
| `ready()` | 方法 | 三个依赖向量全零 = 无依赖 = 可发射 |

### 1.2 地址重叠检测

依赖判断的基础是地址范围重叠检查（行 73-77）：

```scala
def overlaps(other: OpT): Bool = {
  ((other.start <= start && (start < other.end || other.wraps_around)) ||
   (start <= other.start && (other.start < end || wraps_around))) &&
  !(start.is_garbage() || other.start.is_garbage())
}
```

- 判断两个地址区间 `[start, end)` 是否有交集
- `wraps_around` 处理 SP 地址环绕——SP 大小固定，搬运跨越末尾时地址回卷到开头（环形缓冲区），控制器据此将请求拆成两段 DMA
- `is_garbage()` 地址不参与冲突检测（garbage 表示不关心该操作数）

### 1.3 依赖建立规则

新指令入队时，与队列中已有条目（只对已有条目这一点很重要，因为也对之后进来的条目的话，有些指令可能永远发射不了）进行依赖检查（行 327-379）：

**同队列内**：严格按序——新指令依赖该队列中所有已存在的未发射条目。

```scala
// load 队列内（行 329）：
new_entry.deps_ld := VecInit(entries_ld.map { e => e.valid && !e.bits.issued })

// ex 队列内（行 344）：同上
// st 队列内（行 378）：依赖所有 valid 条目（更保守，包括已 issued 的）
```

**语法**：`.map` 对每个 entry 生成 Bool，`VecInit` 转为硬件 `Vec[Bool]`。语义：新 entry 入站时快照当前所有"有效且未发射"的 entry 作为依赖，全部清零后才能发射。

**跨队列**：基于 `overlaps` 检测 RAW/WAW/WAR 冲突。

| 新指令类型 | 对 ld 队列的依赖 | 对 ex 队列的依赖 | 对 st 队列的依赖 |
|-----------|----------------|----------------|----------------|
| **load** | 同队列按序 | opa 与 ex 的 opa/opb 重叠（WAW/WAR） | opa 与 st 的 opa/opb 重叠（WAW/WAR） |
| **ex** | opa/opb 与 ld 的 opa 重叠（RAW/WAW） | 同队列按序 | 根据 `opa_is_dst` 判断读写方向 |
| **st** | opa/opb 与 ld 的 opa 重叠（RAW） | 根据 `opa_is_dst` 判断读写方向 | 同队列按序（保守：依赖所有 valid） |

### 1.4 依赖清除

依赖有**两个清除时机**：

**时机 1：发射时（行 463-477）**

```scala
when (io.fire) {
  // 同队列（ld/ex）：发射后立即清除其他条目对它的依赖
  if ((q == q_) && (q_ != stq)) {
    deps_type(issue_id) := false.B
  } else {
    // 跨队列 + complete_on_issue：CONFIG 指令发射即完成，立即清除
    when (issue_entry.bits.complete_on_issue) {
      deps_type(issue_id) := false.B
    }
  }
}
```

- 同队列（ld/ex）：发射后即清除，因为同队列按序发射，先发的已先到 Controller
- 同队列（st）：**不立即清除**，必须等 Controller 完成回报（更保守）
- 跨队列 + `complete_on_issue`：CONFIG 指令发射即完成，立即广播清除

**时机 2：完成时（行 491-524）**

```scala
when (io.completed.fire) {
  val queue_type = io.completed.bits(type_width + 1, type_width) // 高 2 位 = 队列类型
  val issue_id = io.completed.bits(type_width - 1, 0)            // 低位 = 条目索引

  // 广播清除：所有条目的对应依赖位置零
  entries.foreach(_.bits.deps_ld(issue_id) := false.B)  // 若 queue_type == ldq
  entries_ld(issue_id).valid := false.B                  // 释放槽位

  // 通知 LoopConv/LoopMatmul
  conv_ld_completed := entries_ld(issue_id).bits.cmd.from_conv_fsm
}
```

### 1.5 RS 完整生命周期

```text
分配 → 建立依赖 → 等待依赖清零 → ready() = true → 发射 → Controller 执行 → 完成回报 → 释放槽位
                                                      ↓
                                              清除其他条目对自己的依赖
```

---

## 二、LoadController — 把数据搬进来

> 三大 Controller 中最简单的，适合先建立 Controller 的基本模式。

### 2.1 状态机

3 个状态（行 30）：

```text
waiting_for_command ──→ waiting_for_dma_req_ready ──→ sending_rows ──→ 回到 waiting
      ↓ CONFIG                                              ↑
   写寄存器，立即 ready                           逐行发 DMA 请求，row_counter++
```

### 2.2 多通道配置

`strides/scales/shrinks` 等是 `Vec(load_states, ...)`（行 33-37），对应三个 load 通道：

| funct | 通道 | state_id | 用途 |
|-------|------|----------|------|
| LOAD_CMD | input | 0 | 加载输入 |
| LOAD2_CMD | weight | 1 | 加载权重 |
| LOAD3_CMD | bias | 2 | 加载偏置 |

每个通道有独立的 stride / scale / shrink / block_stride / pixel_repeat。CONFIG_LOAD 指令通过 `state_id` 指定配置哪个通道。

### 2.3 LoadController ↔ DMA 交互

LoadController 不直接搬数据，而是向 DMA 引擎发请求，DMA 独立完成 DRAM ↔ SP 的搬运。分离的原因：DRAM 访问延迟极高（几十到几百周期），如果 LoadController 自己等数据返回，整个流水线会阻塞。分离后 LoadController 连续发请求、DMA 后台搬运，两边解耦各自全速运行。

一条 mvin 指令的 `num_rows` 行通过 `row_counter` 逐行发出 DMA 请求（行 102），每个 `row_counter` 搬运 SP 的一行：

```scala
io.dma.req.bits.vaddr := vaddr + row_counter * stride  // 每行 DRAM 地址 = 基地址 + 行号 × stride
io.dma.req.bits.laddr := localaddr + row_counter       // SP 目标地址逐行递增
io.dma.req.bits.cols := cols                            // 每行搬 cols 个元素
```

逐行发请求而非一次发整块，是因为 DRAM 中行间步幅（stride）可能不连续（如从 224×224 feature map 中取 16×16 tile）。

交互时序（以 mvin 搬 4 行为例，假设 DRAM 访问延迟 L 周期）：

```text
cycle   LoadController          DMA 发 TileLink       DRAM 返回 → 写 SP
─────   ──────────────          ───────────────       ─────────────────
  1     row0 req ────→
  2     row1 req ────→          row0 Get ─────→
  3     row2 req ────→          row1 Get ─────→
  4     row3 req ────→          row2 Get ─────→
  5     空闲（可接新 mvin）     row3 Get ─────→
  ...                           （各等 L 周期，流水线效应）
  2+L                                                 row0 返回 → SP[0]
  3+L                                                 row1 返回 → SP[1]
  4+L                                                 row2 返回 → SP[2]
  5+L                                                 row3 返回 → SP[3]
  5+L+1                         bytes == 预期 → cmd_completed → rob_id → RS
```

要点：
- LoadController 在 cycle 5 就空闲了，DMA 到 cycle 5+L 才真正搬完
- 每个请求都等了 L 周期延迟，但因为发出间隔 1 周期，返回也间隔 1 周期（流水线：延迟不变，吞吐 1/cycle）

关键特性：

| 要点 | 说明 |
|------|------|
| 发完不等 | 请求全部发出后 LoadController 立即空闲，DMA 后台完成搬运 |
| 背压 | DMA 队列满时（`req.ready = false`），停在 `waiting_for_dma_req_ready` |
| 多命令重叠 | `DMACommandTracker` 可同时追踪多条 mvin 命令的搬运进度 |

### 2.4 完成回报机制

通过 `DMACommandTracker` 跟踪（行 94, 114-128）：

```text
1. 分配时：记录 rob_id + 预期字节数
2. DMA 响应：每次回报已搬运字节数
3. 累计达预期值：触发 cmd_completed，将 rob_id 送回 RS
```

### 2.5 零地址特殊处理

`vaddr === 0` 时标记 `all_zeros`（行 72），DMA 不实际读 DRAM，直接往 SP 写零（用于 padding）。

---

## 三、StoreController — 把结果搬出去

> 与 LoadController 结构对称，但方向相反：SP/Acc → DRAM。增加了后处理和 pooling。

### 3.1 状态机

4 个状态（行 32-34），比 LoadController 多一个 `pooling`：

```text
waiting_for_command ──→ waiting_for_dma_req_ready ──→ sending_rows ──→ 回到 waiting
                    ↓ (pooling enabled)
                    pooling ──→ 回到 waiting
```

### 3.2 与 LoadController 的关键差异

| 特性 | LoadController | StoreController |
|------|---------------|-----------------|
| 数据方向 | DRAM → SP | SP/Acc → DRAM |
| 后处理 | 无 | activation + acc_scale + normalization |
| 额外模式 | 无 | max pooling（嵌套 4 层循环）、1D mvout |
| 状态数 | 3 | 4（多一个 `pooling`） |
| 完成回报 | DMACommandTracker | 同（DMACommandTracker + rob_id） |

### 3.3 后处理参数

CONFIG_STORE 指令配置以下参数（行 45-51, 233-271），在 DMA 搬出时应用：

- `activation`：激活函数类型（ReLU 等）
- `acc_scale`：累加器缩放因子
- `igelu_qb/qc`、`iexp_qln2/qln2_inv`：归一化常数
- `norm_stats_id`：归一化统计 ID

### 3.4 Pooling 模式

当 `pool_stride ≠ 0` 时启用（行 73），使用 4 层嵌套循环（行 218-223）：

```text
外层：porow_counter × pocol_counter （输出位置）
内层：wrow_counter × wcol_counter   （池化窗口）
```

第一轮理解到此粒度即可，池化计数器细节可跳过。

---

## 四、ExecuteController — 在内部做计算

> 三大 Controller 中最复杂的。负责从 SP/Acc 读取操作数、送入 Mesh、写回结果。

### 4.1 命令队列与前瞻

```scala
// 行 64-69：TransposePreloadUnroller 预处理 + 3-head 多头队列
val unrolled_cmd = TransposePreloadUnroller(io.cmd, config, io.counter)
val (cmd, _) = MultiHeadedQueue(unrolled_cmd, ex_queue_length, cmd_q_heads = 3)
```

3-head 队列允许 ExecuteController **同时看到最多 3 条指令**，用于判断能否重叠执行。

### 4.2 状态机

```text
waiting_for_cmd ──→ compute ──→ 回到 waiting
              ↓
              flush ──→ flushing ──→ 回到 waiting（OS 数据流专用）
```

### 4.3 三种执行模式

| 模式 | 触发条件（行 584-621） | 含义 | 消费指令数 |
|------|----------------------|------|-----------|
| `perform_single_preload` | 队头是 PRELOAD | 只做预加载（把 D/权重送入阵列暂存） | pop 1 |
| `perform_single_mul` | 队头是 COMPUTE，无后续 PRELOAD | 只做计算（A×B） | pop 1 |
| `perform_mul_pre` | 队头 COMPUTE + 下一条 PRELOAD | **重叠执行**：当前计算和下一次预加载同时进行 | pop 2 |

`perform_mul_pre` 是性能关键——在同一批 `block_size` 行中同时完成当前 tile 的 A×B 计算和下一个 tile 的权重预加载，最大化 Mesh 利用率。

### 4.4 三路操作数送入 Mesh

```text
A 操作数（输入）   → mesh.io.a    来自 SP 读取
B 操作数（权重）   → mesh.io.b    来自 SP 读取
D 操作数（预加载） → mesh.io.d    来自 Acc 或 SP 读取
```

每路有独立的 fire_counter（0 到 block_size-1），逐行送入 Mesh。

### 4.5 Bank Conflict 仲裁

同一周期内如果两路操作数要读同一个 SP bank，会产生 bank conflict（行 316-327）：

```scala
def same_bank(addr1, addr2, ...): Bool = {
  !is_garbage &&
    ((both from acc) || (both from spad && same bank))
}
```

当检测到 bank conflict 时，低优先级操作数的 fire 被延迟一拍，避免同时读同一 bank。

### 4.6 结果写回

Mesh 计算完成后，结果通过 `mesh.io.resp` 逐行写回 Accumulator（行 946-963）：

```scala
io.acc.write(i).valid := start_array_outputting && w_bank === i.U && write_to_acc && !is_garbage_addr && write_this_row
io.acc.write(i).bits.addr := w_row
io.acc.write(i).bits.data := mesh.io.resp.bits.data  // Mesh 输出数据
io.acc.write(i).bits.acc := w_address.accumulate      // 是否累加到已有值
```

写回地址由 preload 指令的 `c_address` 决定。

### 4.7 完成回报

当 Mesh 最后一行结果写完时（`last = true`，行 970-974），将 `rob_id` 送回 RS。

对于 preload-only 指令，如果输出地址是 garbage，则立即在发射时标记完成（行 643-644），不等 Mesh 输出。

### 4.9 ExecuteController 总结框图

```text
         cmd queue (3-head lookahead)
                    ↓
    ┌──────── 模式判定 ────────┐
    │  preload / compute /     │
    │  mul_pre / config        │
    └──────────┬───────────────┘
               ↓
    ┌──── 操作数读取 ────┐
    │  A ← SP             │
    │  B ← SP             │    bank conflict 仲裁
    │  D ← Acc / SP       │
    └──────────┬──────────┘
               ↓
         Mesh (脉动阵列计算)
               ↓
         结果写回 Acc
               ↓
         rob_id → RS (完成回报)
```

---

## 五、三大 Controller 对比

| 维度 | LoadController | StoreController | ExecuteController |
|------|---------------|-----------------|-------------------|
| 职责 | DRAM → SP | SP/Acc → DRAM | SP/Acc → Mesh → Acc |
| 状态数 | 3 | 4 | 4 |
| 执行接口 | DMA（写 SP） | DMA（读 SP/Acc） | Mesh（计算） |
| 后处理 | 无 | activation + scale + norm | 无（由 Mesh 内部完成） |
| 额外模式 | 无 | max pooling、1D mvout | mul_pre（重叠执行） |
| 完成回报 | DMACommandTracker | DMACommandTracker | Mesh last 信号 |
| 复杂度 | ~190 行 | ~330 行 | ~1030 行 |

---

## 六、Day 3 自检

**1. 同一个 controller 内为什么默认按程序顺序收指令？**

Controller 是简单顺序状态机，每次只处理一条命令，没有内部重排序能力。例如 CONFIG_LOAD 必须在对应 mvin 之前到达 LoadController，否则会用错误配置搬数据。RS 通过同队列按序依赖（新 entry 依赖所有未发射的同队列 entry）保证这一点。

**2. 不同 controller 之间为什么可能并行？**

三个 Controller 是独立硬件单元（Load 用 DMA 写 SP、Execute 用 Mesh 计算、Store 用 DMA 读 SP/Acc），物理上可同时工作。RS 通过 `overlaps` 检测跨队列地址是否冲突：不重叠则无依赖，三路可同时发射。典型场景如 Store 搬出上一层结果 + Load 搬入下一层权重（双缓冲）。

**3. ReservationStation 到底在"调度什么"？**

RS 调度"哪些指令可以安全地同时发给不同 Controller 执行"。它缓冲指令流、追踪依赖、发射所有 `ready() = true` 的指令到对应 Controller。本质是**指令级并行度的提取器**——从串行指令流中找出可并行的部分，分发到三条独立执行通道。

### 补充讨论：依赖的单向性

RS 依赖只在**入队瞬间**对已有条目建立（快照），不检查后来的条目。这保证依赖关系是单向的（新 → 老），不可能形成环（A 等 B、B 等 A 的死锁）。后来者的依赖由后来者自己入队时建立，两个方向都不遗漏。

### 补充讨论：LoopConv 命令生成顺序与 RS 的分工

LoopConv 的 `ld_ahead` 信号保证的是**命令生成顺序**（同一 loop iteration 的 load 命令全部送入 RS 后，execute 才开始生成命令），不是 load 已执行完毕。实际执行顺序由 RS 地址依赖检测保证。

同一 loop iteration 内不同 tile 的 SP 地址不重叠（由 `tiled_conv_auto` 软件层保证 tile 尺寸，使数据能放进半个 SP），双缓冲（`concurrent_loops=2`）将 SP 地址空间一分为二，使两个 loop 的指令在 RS 中地址不冲突、可并行发射。

完整的正确性链条：

```
tiled_conv_auto（软件）：计算 tile 尺寸，保证放进半个 SP
    ↓
LoopConv（硬件 FSM）：按序生成命令，ld_ahead 保证 load 命令先于 execute 进入 RS
    ↓
FIFO 命令队列：保序传输
    ↓
RS：按序入队建立依赖 → 依赖清零后发射 → Controller 执行
```
