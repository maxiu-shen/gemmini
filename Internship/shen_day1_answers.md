# Day 1 准备产出：项目主线 + 自我介绍 + 架构图 + 追问应对

> 整理日期：2026-03-19
> 所有内容均基于工作区源码、文档和实际训练数据，禁止凭空编造。

---

## 一、30 秒自我介绍

> **使用场景**：面试官说"先做个自我介绍吧"

面试官您好，我叫沈伟龙，复旦大学电子信息硕士在读，2027 届。我的研究方向是计算机体系结构与 AI 芯片架构评估。目前我主要围绕 **RSNCPU 可重构脉动阵列处理器**做研究——它是一个将 RISC-V CPU 与 DNN 加速器统一在同一 PE 阵列中的架构，我以 **Gemmini** 脉动阵列加速器为参考，以 **YOLOv11n INT8** 目标检测为验证模型，推进从算子划分、性能建模到架构定义的全流程。此外我在本科阶段做过蜂鸟 E203 风格的 RISC-V 处理器设计与验证，以及基于 UVM 的 AXI 总线验证平台。我希望能加入字节的 IP 设计团队，在 AI 芯片架构评估方向贡献我的能力。

**时间控制**：约 30 秒，150 字左右。

---

## 二、2 分钟项目介绍（RSNCPU / Gemmini 主线）

> **使用场景**：面试官说"介绍一下你的主要项目"

### 第 1 段：问题与动机（20 秒）

我目前的核心项目是 **RSNCPU 可重构脉动阵列处理器研究**。它面向自动驾驶端侧推理场景——传统的异构方案是 CPU + 独立加速器，但这种架构存在三个问题：一是 CPU 和加速器之间的 DMA 搬运带来通信冗余；二是 CPU 成为瓶颈时加速器会空闲，资源协同效率低；三是固定架构难以高效映射 YOLOv11 这类多尺度、多分支的检测网络。

> **依据**：`RSNCPU/plan/shen_research_plan_2026-2027_v2.md` 第 28-48 行，明确提出三大问题。

### 第 2 段：方案选择（20 秒）

RSNCPU 的核心思想是 **将 CPU 和 DNN 加速器统一在同一个 10×10 可重构 PE 阵列中**——同一组 PE 可以按行组成 RISC-V CPU 核、也可以按列组成脉动阵列做矩阵乘，甚至可以混合模式运行。我选择 **Gemmini** 作为参考架构，因为它和 RSNCPU/SNCPU 在"CPU + 阵列协同、WS 数据流、Scratchpad 存储"这些关键设计选择上高度一致。

> **依据**：`.github/instructions/RSNCPU_DevGuide.instructions.md` 第 12-14 行定义 RSNCPU；`RSNCPU/group report/2026-03-16/周报-沈伟龙-2026-03-16.md` 说明从 Eyeriss 切换到 Gemmini 的原因。

### 第 3 段：我做了什么（40 秒）

具体来说，我做了四件事：

**第一**，我完成了 **YOLOv11n 在 BDD100K 上的训练与验证**，建立了硬件映射的模型基线。FP32 模型在 10000 张验证集上 mAP50 达到 **44.9%**、mAP50-95 达到 **25.3%**。

> **依据**：`yolo_v11/exports/reports/train4_fp32_val_report_2026-03-18.md`，验证集 10,000 images / 185,526 instances。

**第二**，我基于 YOLOv11n 的网络结构，完成了面向 Gemmini 的 **算子级软硬件划分**——Conv / Pointwise / DWConv / MatMul / ResAdd / Pool 等计算密集算子映射到脉动阵列，SiLU / Upsample / Concat / Split 等控制型操作回退到 CPU。

> **依据**：`RSNCPU/group report/shen_handwritten_c_analysis.md`，详细分析了 Gemmini 已覆盖和需要 CPU 实现的算子。

**第三**，我系统梳理了 **Gemmini 的完整硬件链路**——从 RoCC 命令入口，到 LoopConv/LoopMatmul 循环展开，到 ReservationStation 调度，到 Load/Store/Execute 三个 Controller，再到 Scratchpad/Accumulator 和 16×16 Mesh PE 阵列。我读了源码、做了 bare-metal C 测试验证，覆盖了 WS/OS 两种数据流和手动 tiling。

> **依据**：`src/main/scala/gemmini/` 硬件源码（Controller.scala, Mesh.scala, PE.scala, Scratchpad.scala 等）；`software/gemmini-rocc-tests/bareMetalC/shen_test_*.c` 系列测试。

**第四**，在此基础上我建立了 **RSNCPU 的架构评估口径**——10×10 INT8 WS 阵列、200MHz FPGA 目标频率、40 GOPS 峰值算力，目标 PE 利用率 ≥95%、端到端时延改善 ≥30%。

> **依据**：`RSNCPU/plan/shen_research_plan_2026-2027_v2.md` 技术规格表。

### 第 4 段：价值（10 秒）

这个项目让我具备了从上层算法特性出发，经过算子分析和数据流映射，驱动微架构设计决策的能力——我认为这和字节 AI 芯片设计评估岗位的要求是高度匹配的。

**时间控制**：约 2 分钟。

---

## 三、5 分钟项目深挖版

> **使用场景**：面试官继续追问"再详细讲讲"

在 2 分钟版本基础上，扩展以下内容：

### 3.1 Gemmini 架构细节（额外 1 分钟）

Gemmini 是 RISC-V 的 RoCC 协处理器，默认配置是 **16×16 的 INT8 脉动阵列**，支持 WS 和 OS 两种数据流。

> **依据**：`Configs.scala` 第 23-37 行：inputType=SInt(8.W), meshRows=meshColumns=16, dataflow=Dataflow.BOTH。

它的存储层次包括：
- **Scratchpad**：256 KB，4 个 bank，每 bank 2048 行，每行 256 bits（= 16 × 16 × 8 bit）
- **Accumulator**：64 KB，2 个 bank，32-bit 累加精度

> **依据**：`Configs.scala` 第 41-45 行：sp_capacity=256KB, acc_capacity=64KB, sp_banks=4, acc_banks=2。

数据搬运通过 DMA 完成——LoadController 从 DRAM 搬数据到 Scratchpad（mvin），StoreController 从 Accumulator 搬回 DRAM（mvout）。ExecuteController 驱动 Mesh 计算——先 preload 权重/偏置，再 compute 做矩阵乘累加。

指令调度通过 **ReservationStation** 完成，它类似 ROB 风格的保留站，管理 Load（8 entries）/ Store（4 entries）/ Execute（16 entries）三类指令的依赖追踪和乱序发射。

> **依据**：`Configs.scala` 第 55-57 行：entries_ld=8, entries_st=4, entries_ex=16。

### 3.2 RSNCPU 与 Gemmini 的核心差异（额外 1 分钟）

| 维度 | Gemmini | RSNCPU |
|------|---------|--------|
| 架构模式 | 异构：Rocket CPU + RoCC 加速器，各自独立 | 统一：同一 PE 阵列可切换 CPU/DNN 模式 |
| 阵列规模 | 16×16（默认） | 10×10（对标 SNCPU） |
| 数据搬移 | DMA（mvin/mvout） | WS 双向脉动 + 自定义搬移指令 |
| 可重构 | 无 | 支持行/列级模式切换（如 30% CPU + 70% DNN） |
| ISA | Gemmini 自定义 RoCC 指令 | 融合 Gemmini ISA + SNCPU 的 ASI/MSI/SL2/LL2 |
| 验证平台 | Chipyard + Spike | ZCU102 FPGA（200MHz 目标） |

> **依据**：`.github/instructions/RSNCPU_DevGuide.instructions.md` Gemmini 对照表。

"可重构"具体是指：10×10 阵列中，每**行** 10 个 PE 可以构成一个 5 级 RISC-V CPU；每**列** 10 个 PE 可以构成一个脉动阵列的一列。在混合模式下，可以把部分行/列分配给 CPU 执行控制型操作（如 SiLU、Upsample），剩余部分继续做 DNN 计算。

> **依据**：`RSNCPU_DevGuide.instructions.md` 第 12-14 行。

### 3.3 当前进展与挑战（额外 30 秒）

目前我处于**架构评估阶段**：
- ✅ 完成了 YOLOv11n 训练、验证和算子划分
- ✅ 完成了 Gemmini 源码主链路梳理和 bare-metal C 验证
- ✅ 建立了 RSNCPU 的性能目标和设计空间
- 🔄 下一步是微架构定义——具体包括 PE 内部数据通路、模式切换控制逻辑、和 AOMEM 存储结构的 Chisel 实现

主要挑战是如何在保持 DNN 模式 PE 利用率 ≥95% 的同时，让 CPU 模式也能达到足够的 IPC——这涉及 PE 内部寄存器分配和指令调度的权衡。

> **依据**：`RSNCPU/plan/shen_research_plan_2026-2027_v2.md` 阶段规划和风险分析。

---

## 四、蜂鸟 E203 项目核心叙事（3-5 句话讲清）

> **使用场景**：面试官问"你还有什么其他项目经历"，或从 RSNCPU 自然过渡到 CPU 话题

### 叙事版本（约 1 分钟）

我本科时做过一个 **蜂鸟 E203 风格的 RISC-V 处理器设计与验证项目**。E203 是一个轻量级的两级流水线 RISC-V 内核，面向低功耗 MCU 场景。

我用 Verilog 完成了**两级流水主通路的 RTL 设计**——包括取指/译码/执行单元、寄存器堆、LSU（Load-Store Unit）、SRAM 接口和基础控制逻辑。两级流水将传统五级（IF-ID-EX-MEM-WB）压缩为 IFU（取指+译码）和 EXU（执行+访存+写回），这样做的好处是面积和功耗很低，代价是 IPC 上限为 1 且遇到数据冒险时必须 stall。

在验证方面，我搭建了功能验证环境，围绕**算术逻辑、分支跳转、load/store、访存时序和基础冒险处理**编写了定向测试用例。通过 **波形分析**定位并修正了流水线控制和访存路径中的问题——比如 load 指令完成后写回寄存器堆的时序与后续指令的读取存在冲突，需要插入前递逻辑或 stall。

这个项目让我对 **顺序核的模块边界和数据通路组织** 有了直观理解，也为我后来学习乱序执行架构（Tomasulo、ROB、寄存器重命名、分支预测）提供了对比基础——我能清楚地理解为什么乱序核能提升 ILP 但代价是面积和验证复杂度。

### 核心要点备忘

| 维度 | 内容 |
|------|------|
| 架构风格 | 蜂鸟 E203 风格，两级流水（IFU + EXU），RISC-V RV32IM |
| 我做的设计 | 两级流水主通路 RTL：取指/译码/执行单元、寄存器堆、LSU、SRAM 接口、控制逻辑 |
| 我做的验证 | 定向测试（算术/分支/load-store/冒险）、波形分析定位 bug |
| 定位过的典型 bug | load 指令写回与后续指令读取的时序冲突 |
| 从中学到的 | 顺序核模块边界理解、冒险处理、与乱序核的对比 |

---

## 五、面试官疑虑应对口径

### 疑虑 1："RSNCPU 目前实际做到哪一步了？还没有 RTL 对吗？"

**应对口径**：

是的，RSNCPU 目前处于**架构评估与设计空间探索阶段**，还没有开始 RTL 实现。但我认为这个阶段的工作正是 AI 芯片设计评估的核心能力——

我做的事情是：**从 workload 出发（YOLOv11n 的 100 层网络结构和算子特征），经过算子级软硬件划分，结合 Gemmini 的源码级理解，建立了 RSNCPU 的性能目标和设计空间**。

具体来说：
1. 我完成了 YOLOv11n 在 BDD100K 上的训练验证，模型有 **2.58M 参数、6.3 GFLOPs**，100 层
2. 我基于网络结构做了算子划分——哪些算子适合阵列（Conv/MatMul/Pool），哪些回退 CPU（SiLU/Upsample/Concat）
3. 我通过 Gemmini 源码和 bare-metal C 实验验证了 WS/OS 数据流、tiling 策略、Scratchpad 管理的实际行为
4. 在此基础上建立了 RSNCPU 的量化目标（40 GOPS、PE利用率 ≥95%、时延改善 ≥30%）

下一步是微架构定义和 Chisel RTL 实现，计划在 2026 年 7-9 月完成。

> **策略**：坦诚 + 转化为亮点。强调"workload-driven architecture evaluation"正是 JD 要求的能力。

### 疑虑 2："40 GOPS 这个数怎么算出来的？"

**应对口径**：

峰值算力计算如下：

```
10 (rows) × 10 (cols) × 2 (ops/MAC: 一次乘法 + 一次加法) × 200MHz = 40 GOPS
```

其中：
- **10×10** 是 RSNCPU 的 PE 阵列规模，对标 SNCPU 论文
- **2 ops/MAC** 是标准算力统计方式——一次乘加算 2 个 INT8 操作
- **200MHz** 是 ZCU102 FPGA 的目标工作频率

作为参考，SNCPU 在 65nm ASIC 上跑到 400MHz，峰值是 80 GOPS。我们在 FPGA 上目标频率减半，所以峰值也减半。

> **依据**：`RSNCPU/plan/shen_research_plan_2026-2027_v2.md` 技术规格表。SNCPU 论文：400MHz / 65nm。

### 疑虑 3："PE 利用率 ≥95% 是实测还是估算？依据是什么？"

**应对口径**：

这是**基于 SNCPU 论文数据的目标估算**，不是我的实测值。

SNCPU 论文报告的数据是：在 65nm ASIC 上，**DNN 模式 PE 利用率约 99%，CPU 模式约 96%**。我们的 RSNCPU 目标设为 ≥95%，相比 SNCPU 略保守，因为：
1. RSNCPU 验证模型是 **YOLOv11n**（多尺度、多分支），比 SNCPU 论文用的 VGG16/ResNet18 结构更复杂
2. DWConv 这类算子在脉动阵列上利用率天然较低（因为通道维度无法充分展开）
3. FPGA 上的存储带宽和访存延迟与 ASIC 不同

后续会通过 Verilator RTL 仿真和 FPGA 实测来验证。

> **依据**：SNCPU 论文中 DNN ~99%、CPU ~96%（`RSNCPU/related_thesis/` 中双语文档）。YOLOv11n 的 DWConv 利用率分析见 `RSNCPU/group report/shen_handwritten_c_analysis.md`。

### 疑虑 4："时延改善 30% 相比什么 baseline？"

**应对口径**：

Baseline 是**传统异构架构的端到端推理延时**，预估约 250-270 ms。

这个 baseline 对应的是"Rocket CPU + 独立 Gemmini 加速器"的方案，CPU 负责前/后处理和控制型算子，加速器负责 Conv/MatMul。延时包括：
- CPU 前处理（图像归一化等）
- CPU ↔ 加速器之间的 DMA 搬运
- 加速器计算
- CPU 后处理（NMS 等）

RSNCPU 的核心优势在于**统一架构减少 CPU-加速器之间的数据搬运**，以及**混合模式下 CPU 和 DNN 可以并行执行**（比如一部分 PE 做 DNN 计算的同时，另一部分 PE 做 SiLU 激活），从而减少串行等待。

SNCPU 论文报告了 **39%-64%** 的端到端时延改善（相比纯 CPU 推理），我们目标设为 ≥30%（相比异构方案）是一个合理的保守目标。

RSNCPU 估算的端到端延时约 **180-190 ms**（其中纯 DNN 计算约 170 ms），相比 baseline 的 250-270 ms，改善约 30-35%。

> **依据**：`RSNCPU/plan/shen_research_plan_2026-2027_v2.md` 技术规格表中的延时估算。

### 疑虑 5："你简历写了'理解乱序执行微架构原理'，能具体讲讲吗？"

**应对口径**（简版，Day 2-3 会深入准备）：

我的理解来源于两个层面：

**第一是课程学习**。在复旦的计算机体系结构课上系统学习了 Tomasulo 算法、ROB、寄存器重命名和分支预测。核心理解是：
- **Tomasulo + 寄存器重命名**解决的是 WAW/WAR 假依赖问题，让更多指令可以乱序发射
- **ROB** 解决的是精确异常问题——虽然执行是乱序的，但提交必须顺序，这样异常发生时可以精确回滚
- **分支预测** 解决的是控制冒险——预测错了要 flush 流水线和恢复 RAT

**第二是项目对比**。做蜂鸟 E203 时体会到顺序核的局限——数据冒险只能 stall 或前递，控制冒险只能 flush，IPC 上限低。这让我更清楚地理解乱序核通过扩大调度窗口和推测执行来提升 ILP 的价值，以及代价是面积、功耗和验证复杂度的大幅增加。

---

## 六、项目高频追问清单（预判 + 答案要点）

### 6.1 Gemmini 架构追问

| # | 追问 | 答案要点 | 依据来源 |
|---|------|---------|---------|
| 1 | Gemmini 的数据流是怎样的？ | CPU → RoCC cmd → LoopConv/LoopMatmul（循环展开为 micro-ops）→ ReservationStation（依赖追踪 + 调度）→ LoadController（DRAM→SP, mvin）/ ExecuteController（SP→Mesh→Acc, preload+compute）/ StoreController（Acc→DRAM, mvout） | `Controller.scala` 顶层连线 |
| 2 | 为什么用 Scratchpad 而不是 Cache？ | ① 延迟确定——Cache miss 导致不可预测的 stall，Scratchpad 是软件管理的 SRAM，延迟固定 ② 适合 tiling——软件精确控制每个 tile 何时搬入/搬出 ③ 没有 tag/替换策略的面积开销 | 体系结构共识 + `Scratchpad.scala` 实现 |
| 3 | WS 和 OS 具体怎么切换？ | 通过 `gemmini_config_ex(dataflow, ...)` 指令设置，dataflow=0 是 OS，dataflow=1 是 WS。PE 内部通过 `PEControl.dataflow` 字段控制 c1/c2 累加寄存器的行为：WS 模式下权重驻留在 PE 中被复用，输入数据从左向右流经阵列；OS 模式下部分和驻留，输入/权重流入后原地累加 | `PE.scala` 第 31-147 行的 PEControl 逻辑 |
| 4 | Tiling 怎么决定分块大小？ | 由 `tiled_matmul_auto` 自动选择：在 Scratchpad 总行数（BANK_NUM × BANK_ROWS = 4 × 4096 = 16384 行）和 Accumulator 总行数（ACC_ROWS = 1024 行）的约束下，最大化 tile_I × tile_J × tile_K。tile 的每个维度以 DIM=16 为粒度 | `gemmini.h` 中 `tiled_matmul_auto` 实现 |
| 5 | ReservationStation 怎么调度？ | 类似 ROB：每条指令入站时记录 deps_ld/deps_ex/deps_st 三类依赖向量，只有所有依赖清零时才能 issue。Load/Store/Execute 分别有 8/4/16 个 entries，各自独立追踪 | `ReservationStation.scala`; `Configs.scala:55-57` |
| 6 | LoopConv 和 LoopMatmul 做什么？ | 硬件循环控制器——接收高层 LOOP_CONV_WS / LOOP_WS 指令后，自动展开为多条 LOAD/PRELOAD/COMPUTE/STORE micro-ops。支持 2 个 loop 重叠执行（double-buffering）。目的是减少 CPU 发指令的开销，让硬件自己管理循环 | `LoopConv.scala:805-1045`; `LoopMatmul.scala:667-927` |

### 6.2 RSNCPU 追问

| # | 追问 | 答案要点 | 依据来源 |
|---|------|---------|---------|
| 1 | RSNCPU 相比 Gemmini 想增强什么？ | 核心增强是**统一架构**——消除 CPU-加速器之间的 DMA 搬运开销，让 CPU 和 DNN 共享同一 PE 阵列和存储，支持混合模式并行 | `RSNCPU_DevGuide.instructions.md` |
| 2 | "可重构"体现在哪？ | 三个层面：① PE 级——同一 PE 可做 MAC（DNN）或 ALU（CPU）② 阵列级——行组成 CPU、列组成脉动阵列 ③ 模式切换——通过 ISA 指令动态切换 DNN/CPU/混合模式 | `RSNCPU_DevGuide.instructions.md` 第 12-14 行 |
| 3 | YOLOv11n 为什么适合验证 RSNCPU？ | ① 多算子类型——Conv/DWConv/MatMul 适合阵列，SiLU/Concat/Upsample 需要 CPU，能充分测试混合模式 ② 多尺度特征——FPN 结构有不同分辨率，能测试 tiling 策略的灵活性 ③ 实际应用需求——自动驾驶边缘部署是真实场景 | `shen_handwritten_c_analysis.md` 算子划分分析 |
| 4 | YOLOv11n 有多少算子？哪些走阵列？ | 模型 100 层、2.58M 参数、6.3 GFLOPs。阵列侧：Conv/Pointwise Conv/DWConv/MatMul/ResAdd/Pool；CPU 侧：SiLU（Sigmoid+Mul）/Upsample（最近邻）/Concat/Split/reshape/后处理 NMS。估计手写 C 核心逻辑 <200 行，总计约 1800 行 | `train4_fp32_val_report_2026-03-18.md`（层数/参数/FLOPs）; `shen_handwritten_c_analysis.md`（行数估计） |

### 6.3 YOLOv11n 训练追问

| # | 追问 | 答案要点 | 依据来源 |
|---|------|---------|---------|
| 1 | BDD100K 是什么数据集？ | Berkeley DeepDrive 100K——10 万张驾驶场景图片，10 个目标类别（行人、骑手、汽车、卡车、公交、火车、摩托、自行车、交通灯、交通标志）。验证集 10,000 张 / 185,526 instances | `train4_fp32_val_report_2026-03-18.md` 分类别表 |
| 2 | 你的训练配置是什么？ | 预训练 yolo11n.pt → BDD100K 微调，100 epochs，batch=16，imgsz=640，lr0=0.01，AMP 混合精度，数据增强含 randaugment + erasing(0.4) + mosaic(最后10 epoch关闭) | `yolo_v11/runs/detect/train4/args.yaml` |
| 3 | 这个精度够用吗？ | 对于轻量级模型（仅 2.58M 参数）在 BDD100K 上的表现是合理的。car 类 mAP50 达 74.4%，是最高的；train 类因样本极少（仅 15 个 instance）AP 为 0。作为**硬件验证的模型基线**，核心目的是验证算子映射和端到端流水线的正确性，不需要追求 SOTA 精度 | `train4_fp32_val_report_2026-03-18.md` 分类别结果 |

---

## 七、三张架构图的文字描述

> 以下是你手画架构图时应该画的内容。面试时建议在白纸上画，边画边讲。

### 7.1 Gemmini 主链路图

```
┌──────────────────────────────────────────────────────────────────┐
│                          Rocket CPU                              │
│                    (RISC-V, 顺序核, RV64GC)                      │
└───────────────────────┬──────────────────────────────────────────┘
                        │ RoCC Interface (custom instructions)
                        ▼
┌──────────────────────────────────────────────────────────────────┐
│                     Gemmini Controller                           │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────────────────┐ │
│  │  LoopConv   │  │  LoopMatmul  │  │  raw_cmd_q (指令队列)    │ │
│  │ (卷积展开)   │  │ (矩阵乘展开)  │  │                         │ │
│  └──────┬──────┘  └──────┬───────┘  └────────────┬────────────┘ │
│         │                │                        │              │
│         └────────┬───────┘                        │              │
│                  ▼                                │              │
│  ┌───────────────────────────────┐                │              │
│  │     ReservationStation        │ ◄──────────────┘              │
│  │  (ld:8 / st:4 / ex:16 entries)│                               │
│  │  依赖追踪 + 乱序调度           │                               │
│  └───┬──────────┬──────────┬─────┘                               │
│      │          │          │                                     │
│      ▼          ▼          ▼                                     │
│ ┌─────────┐ ┌─────────┐ ┌──────────────┐                        │
│ │  Load   │ │  Store  │ │   Execute    │                        │
│ │Controller│ │Controller│ │  Controller  │                        │
│ │(DMA Read)│ │(DMA Write)│ │(驱动 Mesh)   │                        │
│ └────┬────┘ └────┬────┘ └──────┬───────┘                        │
│      │          │              │                                 │
│      ▼          ▼              ▼                                 │
│ ┌────────────────────────────────────────────────────┐           │
│ │              Scratchpad (256 KB, 4 banks)           │           │
│ │         + Accumulator (64 KB, 2 banks, INT32)      │           │
│ │              + TLB + DMA (StreamReader/Writer)      │           │
│ └──────────────────────┬─────────────────────────────┘           │
│                        │                                         │
│                        ▼                                         │
│ ┌────────────────────────────────────────────────────┐           │
│ │           Mesh (16×16 PE Array)                     │           │
│ │                                                    │           │
│ │    a ──►  ┌────┬────┬────┬────┐                    │           │
│ │           │ PE │ PE │ PE │... │ ──► (a 右传)        │           │
│ │    b ▼    ├────┼────┼────┼────┤                    │           │
│ │           │ PE │ PE │ PE │... │                    │           │
│ │           ├────┼────┼────┼────┤                    │           │
│ │           │... │... │... │... │                    │           │
│ │           └────┴────┴────┴────┘ ──► out_c (结果)   │           │
│ │    支持 WS (权重驻留) + OS (输出驻留)                │           │
│ │    每个 PE: INT8 MAC + 累加寄存器                   │           │
│ └────────────────────────────────────────────────────┘           │
└──────────────────────────────────────────────────────────────────┘
                        │
                        ▼
                   DRAM (通过 TileLink 总线)
```

**画图要点**：
1. 从上到下：CPU → RoCC → LoopConv/LoopMatmul → RS → 三个 Controller → Scratchpad/Acc → Mesh
2. 标注关键参数：16×16、INT8、256KB SP、64KB Acc
3. 标注数据流方向：mvin（DRAM→SP）、compute（SP→Mesh→Acc）、mvout（Acc→DRAM）

### 7.2 RSNCPU 总体架构草图

```
┌──────────────────────────────────────────────────────────┐
│                    RSNCPU 可重构处理器                     │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │          10×10 可重构 PE 阵列                        │  │
│  │                                                    │  │
│  │  ┌─────────────────────────────────────────────┐   │  │
│  │  │ 模式 A: 全 DNN 模式 (10×10 脉动阵列, WS)    │   │  │
│  │  │ → Conv/MatMul/Pool 等计算密集算子            │   │  │
│  │  └─────────────────────────────────────────────┘   │  │
│  │                                                    │  │
│  │  ┌─────────────────────────────────────────────┐   │  │
│  │  │ 模式 B: 全 CPU 模式 (10 个 5 级 RISC-V)     │   │  │
│  │  │ → 通用计算 / 控制操作                        │   │  │
│  │  └─────────────────────────────────────────────┘   │  │
│  │                                                    │  │
│  │  ┌─────────────────────────────────────────────┐   │  │
│  │  │ 模式 C: 混合模式                             │   │  │
│  │  │   ┌─────────┐ ┌──────────────────────────┐  │   │  │
│  │  │   │ CPU 子阵 │ │    DNN 子阵列 (脉动阵列)  │  │   │  │
│  │  │   │ (30%)    │ │    (70%)                 │  │   │  │
│  │  │   │ SiLU     │ │    Conv/MatMul           │  │   │  │
│  │  │   │ Concat   │ │                          │  │   │  │
│  │  │   └─────────┘ └──────────────────────────┘  │   │  │
│  │  └─────────────────────────────────────────────┘   │  │
│  └────────────────────────────────────────────────────┘  │
│                          │                                │
│                          ▼                                │
│  ┌────────────────────────────────────────────────────┐  │
│  │        AOMEM (可重配置片上存储)                      │  │
│  │        类 Scratchpad 寻址，无传统 DMA               │  │
│  └────────────────────────────────────────────────────┘  │
│                          │                                │
│  ┌────────────────────────────────────────────────────┐  │
│  │        融合 ISA (Gemmini 风格 + SNCPU ASI/MSI)     │  │
│  │        模式切换指令 / 搬移指令 / 计算指令            │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  目标: 10×10 INT8 WS | 200MHz | 40 GOPS                 │
│  PE 利用率: DNN ≥95% | CPU ≥90%                          │
│  验证模型: YOLOv11n INT8 on BDD100K                       │
│  平台: Xilinx ZCU102 FPGA                                │
└──────────────────────────────────────────────────────────┘
```

**画图要点**：
1. 突出"三种模式"——全 DNN / 全 CPU / 混合
2. 标注与 Gemmini 的核心差异：统一架构、无 DMA、可重构
3. 标注目标参数

### 7.3 蜂鸟 E203 两级流水图

```
┌──────────────────────────────────────────────────────────┐
│                蜂鸟 E203 风格两级流水线                     │
│                                                          │
│  Stage 1: IFU (取指单元)                                  │
│  ┌──────────────────────────────────────────────────┐    │
│  │                                                  │    │
│  │  PC → I-SRAM → 指令寄存器 → 译码 → 操作数读取     │    │
│  │   ▲                              │               │    │
│  │   │                              │ 寄存器堆读端口  │    │
│  │   │                              ▼               │    │
│  │   │                     ┌──────────────┐         │    │
│  │   │                     │  Register File│         │    │
│  │   │                     │  (RV32IM)     │         │    │
│  │   │                     └──────────────┘         │    │
│  └───│──────────────────────────────┬───────────────┘    │
│      │                              │                    │
│      │ 分支/跳转目标                  │ 操作数 + 控制信号   │
│      │                              ▼                    │
│  Stage 2: EXU (执行单元)                                  │
│  ┌──────────────────────────────────────────────────┐    │
│  │                                                  │    │
│  │  ┌─────┐  ┌─────┐  ┌─────┐  ┌────────────────┐ │    │
│  │  │ ALU │  │ BJU │  │ CSR │  │  LSU           │ │    │
│  │  │(算术)│  │(分支)│  │(状态)│  │(Load/Store)   │ │    │
│  │  └──┬──┘  └──┬──┘  └──┬──┘  └───────┬────────┘ │    │
│  │     │        │        │              │          │    │
│  │     │        │        │         D-SRAM 接口      │    │
│  │     │        │        │              │          │    │
│  │     └────────┴────────┴──────────────┘          │    │
│  │                    │                            │    │
│  │              写回 (WB)                           │    │
│  │                    │                            │    │
│  │                    ▼                            │    │
│  │           Register File (写端口)                  │    │
│  └──────────────────────────────────────────────────┘    │
│                                                          │
│  冒险处理:                                                │
│  - 数据冒险: stall + 前递 (forwarding)                    │
│  - 控制冒险: flush IFU + 重新取指                         │
│  - 结构冒险: LSU 访存与取指共享总线时可能 stall            │
└──────────────────────────────────────────────────────────┘
```

**画图要点**：
1. 明确两级划分：IFU（取指+译码+读寄存器）和 EXU（执行+访存+写回）
2. EXU 内部标注 ALU/BJU/CSR/LSU 四个执行子单元
3. 标注冒险处理方式
4. 标注 SRAM 接口（I-SRAM 取指、D-SRAM 访存）

---

## 八、异步 FIFO / 序列检测器实现需求列表

> Day 1 只需要明确需求，不需要写代码。

### 8.1 异步 FIFO

| 模块 | 功能 | 关键点 |
|------|------|--------|
| FIFO Memory | 双端口 RAM | 一读一写，用 addr 直接索引 |
| Write Pointer | 写时钟域计数器 | Binary → Gray code 转换 |
| Read Pointer | 读时钟域计数器 | Binary → Gray code 转换 |
| wptr→rclk 同步器 | 写指针同步到读时钟域 | 2FF 同步，用于判空 |
| rptr→wclk 同步器 | 读指针同步到写时钟域 | 2FF 同步，用于判满 |
| Full 逻辑 | 写时钟域判满 | Gray code: 最高两位不同 + 其余相同 |
| Empty 逻辑 | 读时钟域判空 | Gray code: 完全相同 |

### 8.2 序列检测器

| 版本 | 检测序列 | 类型 | 重叠 |
|------|---------|------|------|
| V1 | `1011` | Mealy | 允许重叠 |
| V2（可选） | `1011` | Moore | 允许重叠 |
| V3（可选） | `1101` | Mealy | 允许重叠 |

状态数：`1011` 检测需要 5 个状态（S0-S4），Mealy 型输出在转移上。

---

## 九、Day 1 产出自检

| # | 产出 | 状态 |
|---|------|------|
| 1 | 30 秒自我介绍稿 | ✅ 见第一节 |
| 2 | 2 分钟项目介绍稿 | ✅ 见第二节 |
| 3 | 5 分钟深挖版本 | ✅ 见第三节 |
| 4 | 蜂鸟项目核心叙事 | ✅ 见第四节 |
| 5 | 面试官疑虑应对口径（5 条） | ✅ 见第五节 |
| 6 | 项目高频追问清单（13 题） | ✅ 见第六节 |
| 7 | Gemmini 架构图文字描述 | ✅ 见 7.1 |
| 8 | RSNCPU 架构图文字描述 | ✅ 见 7.2 |
| 9 | 蜂鸟流水图文字描述 | ✅ 见 7.3 |
| 10 | 异步 FIFO / 序列检测器需求列表 | ✅ 见第八节 |
