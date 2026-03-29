# 硬件基础知识记录

> 记录阅读 Gemmini 源码和芯片架构学习过程中积累的硬件知识理解。

---

## 一、DMA（Direct Memory Access）

### 1.1 为什么需要 DMA

访问 DRAM 延迟极高（几十到几百周期），如果让功能模块（如 LoadController）自己等每行数据从 DRAM 返回再发下一个请求，整个流水线会严重阻塞。

DMA 是**专用硬件搬运引擎**，功能模块只需告诉它"从哪搬、搬到哪、搬多少"，DMA 独立完成搬运，功能模块可以继续处理其他事务。

```
传统方式：模块 ──读──→ DRAM ──写──→ SRAM    （模块全程阻塞）
DMA 方式：模块发起请求 → DMA 独立搬运 → 完成后通知    （模块解放）
```

### 1.2 Gemmini 中 DMA 的角色

```
DRAM（主存）  ←──DMA──→  Scratchpad（片上 SRAM）  ←──→  Mesh（脉动阵列）
```

LoadController / StoreController 是 DMA 的请求发起方，DMA 模块实现在 `DMA.scala` 中。

### 1.3 LoadController ↔ DMA 交互流程

以一条 `mvin` 指令搬 8 行数据为例：

1. LoadController 解析 mvin 指令（vaddr、SP 地址、rows、cols）
2. `row_counter` 从 0 到 7，**逐行**发 DMA 请求（每行带独立的 DRAM 地址和 SP 地址）
3. 8 行请求全部发完后，LoadController **立即空闲**可接新命令（不等 DMA 搬完）
4. DMA 在后台逐行搬运，每搬完一部分向 `DMACommandTracker` 回报字节数
5. 累计字节数达到预期值时，触发 `cmd_completed`，将 `rob_id` 送回 RS

关键特性：

| 要点 | 说明 |
|------|------|
| 逐行发请求 | 因为 DRAM 中行间步幅（stride）可能不连续，必须逐行计算地址 |
| 发完不等 | 请求发完即空闲，DMA 后台搬运（生产者-消费者解耦） |
| 完成追踪 | `DMACommandTracker` 可同时追踪多条命令的搬运进度 |
| 背压 | DMA 队列满时（`req.ready = false`），LoadController 暂停在 `waiting_for_dma_req_ready` |
| 零地址优化 | `vaddr == 0` 时不读 DRAM，直接往 SP 写全零（用于 padding） |

### 1.4 DMA 请求字段

```scala
io.dma.req.bits.vaddr    // DRAM 虚拟地址
io.dma.req.bits.laddr    // Scratchpad 本地地址（哪一行）
io.dma.req.bits.cols     // 本行搬多少个元素
io.dma.req.bits.repeats  // 是否重复填充同一行（pixel_repeat，用于上采样）
io.dma.req.bits.scale    // 搬入时的缩放因子
io.dma.req.bits.shrink   // 是否压缩位宽
```

---

## 二、DRAM 访问延迟

### 2.1 延迟构成

DRAM 总访问延迟 = **物理访问时间（主因）** + **路径上流水线寄存器（次因）**

**物理访问时间**：DRAM 用电容存储数据，每次访问需要：

1. **行激活（Row Activate, tRCD）**：给电容阵列充电、感应放大（~13ns）
2. **列读取（Column Access, CL）**：从已激活行中选列（~13ns）
3. **预充电（Precharge, tRP）**：关闭当前行，准备下一次访问（~13ns）

**路径寄存器延迟**：从加速器到 DRAM 的路径上有多级寄存器隔离：

```
Gemmini DMA → TileLink 总线 → L2 Cache → 总线桥 → 内存控制器 → PHY → DRAM
          reg            reg        reg       reg       reg
```

每级增加 1 个周期，合计约 10-20 周期。加上 DRAM 物理访问（~40-60ns，@1-2GHz 对应 40-120 周期），总延迟可达 50-200 周期。

### 2.2 DRAM 的定时协议

DRAM 不使用 valid/ready 握手协议，而是**基于固定时序**的协议。

**CAS Latency（CL）**：内存控制器发出 READ 命令后，不等任何应答信号，而是知道"恰好 CL 个时钟周期后，数据会出现在 DQ 总线上"。CL 值从 DIMM 上的 **SPD（Serial Presence Detect）** 芯片读取，是出厂时固定的参数（如 DDR4-3200 的 CL=22）。

**DQS（Data Strobe）信号**：现代 DDR 中，DRAM 输出数据时同步翻转 DQS，内存控制器用 DQS 边沿精确采样数据：

```
DQS:  ──┐  ┌──┐  ┌──┐  ┌──
        └──┘  └──┘  └──┘
DQ:   [D0][D1][D2][D3]...     ← 每个 DQS 边沿采样一个数据
```

CL 告诉控制器"大概什么时候来"，DQS 告诉控制器"精确在哪个边沿采"。

### 2.3 Bank 级并行与流水线吞吐

DRAM 芯片内部有多个 **Bank**（通常 8-16 个），每个 Bank 有独立的状态机，可以并行操作。内存控制器将请求交错分发到不同 Bank，使多个 Bank 的等待时间互相重叠：

```
              Bank 0    Bank 1    Bank 2    Bank 3
req0 → 分配    ●
req1 → 分配              ●
req2 → 分配                        ●
req3 → 分配                                  ●
返回:         cycle 2+L   3+L      4+L       5+L
```

每个请求都等了 L 周期，但因为分布在不同 Bank，返回间隔 1 周期——**延迟不变，吞吐接近 1/cycle**。

如果多个请求落在**同一个 Bank**，则无法并行，必须串行等待（延迟退化为 N×L）。因此地址映射通常将连续地址交错散布到不同 Bank，最大化 Bank 级并行度。

### 2.4 内存控制器：定时协议 ↔ 握手协议的桥

内存控制器是两种协议之间的转换桥：

- **面向 DRAM**：靠计数器定时采样，无握手
- **面向 SoC**：用标准 valid/ready 握手回报，对上游（DMA、L2）来说就是普通的总线响应

```
SoC 侧（握手协议）       内存控制器              DRAM 侧（定时协议）
──────────────       ──────────────          ──────────────
req valid/ready →    记录请求，启动计数器      → 发 READ 命令
                     等 CL 周期（计数器倒计时）
                     计数器到期，采样 DQ        ← 数据出现在总线
rsp valid ←          打包数据，拉高 rsp.valid
```

---

## 三、SRAM 访问延迟

### 3.1 与 DRAM 的本质区别

| | DRAM | SRAM |
|---|---|---|
| 存储单元 | 1 电容 + 1 晶体管 | 6 晶体管（锁存器） |
| 需要刷新 | 是（电容漏电） | 否 |
| 访问延迟 | 几十到几百周期 | **1 周期**（同步读） |
| 协议 | 定时协议（CL + DQS） | 时钟边沿驱动，无需复杂控制器 |
| 密度 | 高（面积小） | 低（面积大，约 DRAM 的 6 倍） |

### 3.2 SRAM 读时序

Chisel 中 `SyncReadMem`（Gemmini Scratchpad 使用）的行为：

```
周期 N:    提供地址 addr
周期 N+1:  数据出现在读端口
```

地址给进去，下一拍数据就出来，时序完全由时钟边沿保证，不需要计数器或握手协议。

### 3.3 为什么 Gemmini 需要 Scratchpad

脉动阵列每拍消费一行数据，要求数据源必须以 **1 周期延迟**稳定供数。DRAM 的几十到几百周期延迟无法满足，因此先用 DMA 将数据从 DRAM 搬到片上 SRAM（Scratchpad），Mesh 再从 SRAM 读取：

```
DRAM ──DMA（高延迟，批量搬运）──→ Scratchpad（SRAM） ──1 周期读──→ Mesh（每拍消费）
```

---

## 四、片上存储器的组织方式

片上 SRAM 根据并发访问需求有三种典型组织方式。

### 4.1 总览对比

| | 单体 SRAM + 仲裁器 | 行级分区 Bank | 字节交错 Bank |
|---|---|---|---|
| **结构** | 1 块 SRAM，多源仲裁 | 多块 SRAM，按行分区 | 多块 SRAM，按字节交错 |
| **并发能力** | 每周期仅 1 个访问者 | 不同 Bank 可并行 | 同一访问内多 Bank 并行读出 |
| **核心目的** | 简单、面积小 | 多请求者并行访问 | 字节级写掩码 + 单周期非对齐访问 |
| **实例** | **E203 ITCM** | **Gemmini SP** | **Y86-64 bmemory** |

### 4.2 单体 SRAM + 仲裁器（E203 ITCM）

最简单的方案：只用 1 块 SRAM，多个访问源通过**优先级仲裁器**串行化，同一时刻只能有 1 个源访问。

```
IFU（取指）──┐
LSU（存取）──┼──→ 优先级仲裁器 ──→ 单体 SRAM（64bit × 8192，64KB）
EXT（外部）──┘        ↑
                同周期仅 1 个通过
```

E203 的 ITCM 就是这种结构：1 块 64 位宽、8192 深度的单端口 SRAM，IFU/LSU/EXT 三个源仲裁访问。LSU 和 EXT 的 32 位接口通过位宽转换模块扩展到 64 位匹配 SRAM 宽度，写入时用 8 位写掩码（`MW=8`）实现字节级写入（这里的字节级写掩码是靠存储器本身实现的，而非多bank结构，对于不能实现字节级写掩码的存储器，可以通过字节交错 Bank实现该功能）。

**优点**：硬件简单、面积小。**缺点**：多源同时访问时产生结构冲突，被仲裁阻塞的源必须等待。

### 4.3 行级分区 Bank（Gemmini Scratchpad）

SP 按行划分为多个 Bank，每个 Bank 是独立的单端口 SRAM。地址格式为 `[bank_id | row_within_bank]`（高位选 Bank，低位选行），每个 Bank 持有**连续的一段地址空间**：

```
Bank 0: row 0     ~ row 4095    (地址 0x0000 - 0x0FFF)
Bank 1: row 4096  ~ row 8191    (地址 0x1000 - 0x1FFF)
Bank 2: row 8192  ~ row 12287   (地址 0x2000 - 0x2FFF)
Bank 3: row 12288 ~ row 16383   (地址 0x3000 - 0x3FFF)
```

不做硬件地址交错——LoopConv/LoopMatmul 在软件层面将输入、权重、偏置分配到不同 Bank 的地址段，从而避免冲突。

**目的**：解决单端口 SRAM 的并发访问冲突。同一周期内可能有多个请求者：

| 请求者 | 操作 |
|--------|------|
| LoadController | 写入（DMA 搬入数据） |
| ExecuteController 读 A | 读（输入操作数） |
| ExecuteController 读 B | 读（权重操作数） |
| StoreController | 读（搬出到 DRAM） |

只要访问落在不同 Bank，就可以并行。同 Bank 冲突时（如 A 和 B 读同一 Bank），低优先级操作延迟一拍。

### 4.4 字节交错 Bank（Y86-64 bmemory）

将每个地址的不同字节分散到不同 Bank，每个 Bank 宽 8 位、有独立地址输入：

```
              Bank0    Bank1    Bank2    ... Bank15
行地址 0:     [0x00]   [0x01]   [0x02]  ... [0x0F]
行地址 1:     [0x10]   [0x11]   [0x12]  ... [0x1F]
```

**功能 1：字节级写掩码**

执行 `sb`（写 1 字节）时，只使能目标 Bank 的写信号，其余 Bank 不受影响，无需 read-modify-write。

**功能 2：单周期非对齐访问**

每个 Bank 有**独立地址输入**，当访问跨越行边界时，部分 Bank 读当前行地址、部分读下一行地址，16 个 Bank 同时读出后通过**旋转 MUX** 拼接成连续字节序列：

```
例：从地址 0x0D 读 10 字节（跨 16 字节行边界）

Bank 13-15: 读行地址 0（当前行） → byte 0-2
Bank 0-6:   读行地址 1（下一行） → byte 3-9
                                    ↓
                              旋转 MUX 拼接
                                    ↓
                           10 字节连续输出（1 周期完成）
```

关键前提：Bank 数量 ≥ 最大访问宽度，保证任意连续 N 字节在每个 Bank 最多命中 1 次，不产生冲突。

### 4.5 组合方案：行级分区 + 字节级写掩码（ARM Cortex-A57 L2 Cache）

两种 Bank 方案不互斥，可以组合。ARM Cortex-A57 的 L2 Cache 是一个典型实例，采用**两层 Bank 结构**：

```
第一层：Tag Bank 分区（行级分区思想）
  Tag Bank 0 ←── 请求 A
  Tag Bank 1 ←── 请求 B     ← 两个请求可同时访问不同 Tag Bank

第二层：Data Bank 分区（每个 Tag Bank 内部再分）
  Tag Bank 0 → Data Bank 0, Data Bank 1, ...   ← 支持流式并行数据读出

最底层：每个 Data Bank 的 SRAM 带字节写掩码
  → 实现字节级写入粒度
```

但 Cache **不需要**字节交错的独立地址非对齐访问，原因是：
- Cache 以 **cache line**（如 64 字节）为粒度搬入，内部按 line 对齐存储
- CPU 的非对齐访问在 cache line 内部通过**移位器/拼接逻辑**解决
- 只有无 Cache 的 TCM/SRAM 直接访问场景（如 Y86-64），才需要字节交错 Bank 硬件解决非对齐

### 4.6 三种方案的取舍

| 方案 | 优点 | 缺点 | 适用场景 |
|------|------|------|---------|
| 单体 + 仲裁 | 面积最小、逻辑最简单 | 多源冲突时阻塞 | 低成本嵌入式（E203） |
| 行级分区 | 多请求者可并行 | Bank 数受面积限制，软件需分配地址避免冲突 | 加速器片上缓存（Gemmini） |
| 字节交错 | 任意对齐单周期访问 | 旋转 MUX 面积开销大，Bank 数 ≥ 最大访问宽度 | 无 Cache 的 TCM/SRAM 直接访问（Y86-64） |
| 行级 + 字节写掩码 | 多端口并行 + 灵活写入 | 面积开销较大 | 高性能 Cache（ARM Cortex-A57 L2） |
