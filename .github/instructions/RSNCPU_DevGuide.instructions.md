---
description: RSNCPU 统一主指南：开发、文档、研究计划与组会报告规范（编辑 RSNCPU/** 时自动加载）
applyTo: "RSNCPU/**"
---

# RSNCPU 统一主指南

本文件是 `RSNCPU/**` 的唯一主 instruction。凡是在 `RSNCPU/` 下编写代码、撰写文档、维护研究计划或编写组会报告，均以本文件为唯一入口，不再依赖其他 RSNCPU instruction 文件。

## 项目定位

**论文题目**：面向边缘计算，支持多样化神经网络高效推理的可重构融合处理器

RSNCPU（Reconfigurable Systolic Neural CPU）是 SNCPU 的升级版本，将 10×10 脉动 DNN 加速器与 RISC-V CPU 统一在可重配置 PE 阵列中，采用 WS 双向脉动数据流，并引入 Gemmini 风格自定义 ISA 控制。本项目位于 Gemmini 仓库的 `RSNCPU/` 子目录中，以 Gemmini 作为核心参考架构，可直接参考父目录中的硬件源码、C API 和 Spike 功能模型。

回答与 RSNCPU 相关的问题时，应以**资深硬件架构师和 AI 研究员**的视角思考，优先关注延迟、带宽、面积、功耗、时序风险、片上数据局部性和工程可落地性。

## 编写前必读清单

在 `RSNCPU/` 下进行任何编写前，必须完成以下检查：

1. **先读本文件**：本文件已整合项目说明、Gemmini 对照参考、研究计划与组会规范。
2. **理解 Gemmini 参考架构**：阅读 `.github/copilot-instructions.md` 的相关章节，以及 `src/main/scala/gemmini/` 下关键模块，作为 RSNCPU 设计基线。
3. **查阅 RSNCPU 现有材料**：优先阅读 `RSNCPU/` 目录下已有代码、文档、计划和图示，保证新增内容与当前方案一致。
4. **涉及指令设计或软硬件交互时**：参考 `software/gemmini-rocc-tests/include/` 和 `software/libgemmini/gemmini.cc`。
5. **涉及组会报告时**：除技术内容正确外，还必须遵守本文件中的组会报告格式规范。

## 项目结构与上下文

- `RSNCPU/doc/`：架构设计文档。
- `RSNCPU/doc/self_doc/`：内部设计材料，如 `System Architecture.vsdx`、`gemmini readme summary.docx`。
- `RSNCPU/doc/related_doc/`：相关背景知识与参考图示。
- `RSNCPU/group report/`：组会汇报材料。
- `RSNCPU/plan/`：研究计划、开题记录及相关材料。
- `RSNCPU/plan/images/`：计划或报告中用到的图片资源。
- `RSNCPU/related_thesis/`：重要参考论文全文库，如 SNCPU、Gemmini、Eyeriss v1/v2 等。

## 开发工作流

- **HDL**：使用 Verilog/SystemVerilog 或 Chisel/Scala 进行 RTL 设计。
- **仿真**：使用 Python/C++ 进行行为建模。
- **文档生成**：与 `RSNCPU/doc/self_doc/` 中定义的模块边界和系统结构保持一致。
- **新增设计文件**：保持 `RSNCPU/doc/` 目录结构稳定，不随意重组。
- **参考入口**：`RSNCPU/doc/self_doc/System Architecture.vsdx` 用于理解顶层组件交互关系。

## Gemmini 参考要点

> Gemmini 的完整硬件架构、ISA 指令集、Scala/Chisel 编码规范、C 软件规范与构建命令，请参考工作区根目录的 `.github/copilot-instructions.md` 与 `README.md`。本节仅保留 RSNCPU 设计直接需要的对照参考。

### Scratchpad 内存寻址（RSNCPU 存储设计参考）

- 按行寻址的私有存储器，每行宽度为 `DIM` 个元素。
- 32 位地址中，第 31 位选择 Scratchpad（0）或累加器（1）。
- 第 30 位控制覆写或累加。
- 第 29 位控制缩放读取或原始读取。
- RSNCPU 的 AOMEM 重配置可借鉴这一寻址思想，但需适配 CPU/加速器双模访问需求。

### 解耦访问/执行架构（RSNCPU 控制器设计参考）

- Gemmini 采用 `ExecuteController`、`LoadController`、`StoreController` 三个独立控制器。
- `ROB` 用于检测不同控制器之间的指令冒险。
- RSNCPU 在此基础上还需要额外考虑模式切换控制逻辑和多核 CPU 调度。

### Gemmini 与 SNCPU / RSNCPU 的核心差异

| 维度 | Gemmini | SNCPU | RSNCPU（目标） |
|------|---------|-------|----------------|
| 架构 | 异构（Rocket CPU + RoCC 加速器） | 统一（PE 阵列可重构） | 统一 + Gemmini 风格 ISA |
| 数据搬移 | DMA（`mvin` / `mvout`） | 无 DMA，WS 双向数据流 | WS 双向 + 自定义搬移/调度指令 |
| 控制方式 | 细粒度 ISA（`config` / `mvin` / `mvout` / `matmul`） | `ASI` / `MSI` / `SL2` / `LL2` 自定义指令 | 融合两者优势 |
| PE 复用 | 无（固定加速器） | 64%-80% 逻辑复用 | 逻辑复用 + ISA 级模式切换 |
| 存储 | Scratchpad + Accumulator（SRAM） | AOMEM 重配置 | AOMEM + Scratchpad 寻址借鉴 |

### CISC 循环指令（高层 API 设计参考）

- `gemmini_loop_ws`：大矩阵乘法自动分块并支持双缓冲。
- `gemmini_loop_conv_ws`：卷积与池化自动分块并支持双缓冲。
- RSNCPU 可参考这一模式，将复杂操作封装为更高层任务调度指令，由硬件展开执行。

## 文档与组会报告规范

### 通用文档要求

- 文档内容必须与当前 RSNCPU 微架构、接口命名和研究计划保持一致。
- 对于仍未实现或仍在论证中的内容，要明确写清楚“目标”“假设”“风险”或“待验证项”，避免写成既成事实。
- 涉及 Gemmini 对照时，应明确指出“借鉴点”和“差异点”，不要把 Gemmini 机制直接等同为 RSNCPU 现状。

### 组会报告格式

在撰写、润色或生成 `RSNCPU/group report/` 下的组会文档时，必须严格遵循以下结构：

## 1. 本周目标和计划

简明列出本周开始时制定的任务和核心目标。

## 2. 详细描述本周工作内容和进展

详细说明各项任务的执行情况、遇到的问题、技术难点、解决方案以及阶段性成果。

- 如涉及代码或架构设计（如微架构验证、逻辑开发等），应补充必要细节或数据支撑。

## 3. 列出下周计划

结合本周实际进展和 RSNCPU 整体时间表，明确列出下一周期的待办事项和预期目标。

> **格式要求**：保持严谨、客观的工程语言风格，专业术语如 PE、DNN、RISC-V 等使用英文原文。

## 研究计划摘要与强制引用

`RSNCPU/plan/shen_research_plan_2026-2027_v2.md` 是 RSNCPU 研究计划的唯一完整版本。

凡是涉及以下内容，**必须进一步阅读** `RSNCPU/plan/shen_research_plan_2026-2027_v2.md`，不能只依赖本 instruction：

- 完整时间规划与阶段里程碑
- `YOLOv11n` 验证递进线索
- 风险矩阵与缓解措施
- 完整参数表、性能目标与对比口径
- 参考文献、论证依据与学术表述

### 研究对象与约束

- 研究对象：自动驾驶场景中的二维视觉目标检测任务。
- 重点目标：车辆、行人、骑行者以及交通标志、交通灯等典型道路参与者与交通要素。
- 部署约束：面向边缘侧部署，关注计算资源、片上存储、功耗预算和实时响应。

### 核心研究问题

1. **数据通信冗余**：传统 CPU + 加速器分离式异构架构依赖总线和 DMA 频繁搬运数据，端到端延迟和能耗高。
2. **异构资源协同低效**：预处理、层间处理和后处理对 CPU 依赖强，容易造成加速器空闲和系统利用率下降。
3. **固定架构映射失配**：目标检测模型具有多尺度、多分支和跨层融合特征，固定脉动阵列易出现 PE 利用率下降。

### 研究目标

目标是在 `Xilinx ZCU102 FPGA` 上实现基于 SNCPU 架构、采用 Chisel 编写的 `RSNCPU` 处理器，以 `YOLOv11n INT8` 作为核心验证模型，完成自动驾驶场景二维视觉目标检测任务的功能、性能与可实现性验证。

### 技术路线摘要

- 以 SNCPU 的统一可重构架构为基础。
- 引入 Gemmini 风格的细粒度自定义 RISC-V ISA。
- 采用 WS 双向脉动数据流与 AOMEM 重配置。
- 围绕 `YOLOv11n INT8` 进行算法-硬件协同映射与 FPGA 验证。

### 需长期保持一致的创新点摘要

1. **统一可重构架构**：融合 CPU 与 DNN 加速阵列，降低端到端数据搬运开销。
2. **Gemmini 风格 ISA 扩展**：以指令级方式实现模式配置、模式切换与多核协同控制。
3. **任务导向阵列切分**：根据目标检测任务负载动态分配 CPU 与 DNN 资源。
4. **面向自动驾驶目标检测的完整验证**：以 `YOLOv11n INT8` 和 `ZCU102 FPGA` 为主线完成端到端验证。

### 研究计划使用要求

- 回答研究背景、创新点、实验路线、阶段进度时，应优先与 `shen_research_plan_2026-2027_v2.md` 保持一致。
- 若主 instruction 与研究计划正文存在冲突，以 `shen_research_plan_2026-2027_v2.md` 为准。
- 撰写计划类文档、开题材料、阶段总结和论文相关内容时，不得凭记忆重构计划细节，应显式参考该文件。

---

## 关键架构理解（SNCPU 核心设计）

### PE 阵列结构
- 10×10 可重构 PE 阵列
- 每**行**的 10 个 PE = 1 个完整 5 级 RISC-V CPU 流水线（行 CPU 模式）
- 每**列**的 10 个 PE = 1 个完整 5 级 RISC-V CPU 流水线（列 CPU 模式）
- 同一 PE 同时支持：行 CPU 的第 f(col) 流水线阶段 + 列 CPU 的第 g(row) 流水线阶段 + DNN MAC
- PE 设计是**位置相关（非均匀）**的：位于 `(row_i, col_j)` 的 PE 需同时支持两种 CPU 角色 + MAC

### 流水线阶段到 PE 的映射（以行为例，10 个 PE）
| PE 位置 | CPU 流水线阶段 | 复用 MAC 组件 | 逻辑复用率 |
|---------|--------------|--------------|-----------|
| PE1 | PC（程序计数器）| MAC 加法器 + 32-bit 寄存器 | 69% |
| PE2-3 | IF（取指）| 32-bit 内部寄存器（`c1/c2`）+ 8-bit 输入寄存器 | 80% |
| PE4-5 | ID（译码）| 8-bit 乘法器 + 32-bit 加法器重构为操作码/功能码解码器 | 80% |
| PE6-7-8 | EX（执行）| PE6=ALU+布尔+移位器，PE7=分支地址计算，PE8=寄存器透传 | 77% |
| PE9 | MEM（访存）| ALU 结果到/从 Dcache | 64% |
| PE10 | WB（写回）| Dcache→寄存器堆，MUX 逻辑 | 64% |

**整体面积开销：9.8%（PE 阵列 3.4% + MEM 6.4%）**

### 五种工作模式
| 模式 | AOMEM 角色 | 数据流向 |
|------|-----------|--------|
| 列加速器模式 | 行 AOMEM = 激活输入，底部 = 累加输出 | 数据从上到下 |
| 行 CPU 模式 | 右侧 AOMEM = 数据缓存，左侧指令缓存激活 | 指令从左到右流水 |
| 行加速器模式 | 底部 AOMEM = 输入，右侧 = 输出 | 数据从左到右 |
| 列 CPU 模式 | 底部 AOMEM = 数据缓存，顶部指令缓存激活 | 指令从上到下流水 |
| **混合模式** | 部分行/列为加速器 AOMEM + 部分为 CPU Dcache | 按子阵列划分独立运行 |

> **混合模式说明**（SNCPU 论文 Fig.5(c)）：50% PE 配置为 DNN 加速器、50% 配置为 5 核 RISC-V CPU，为不均衡 CPU/DNN 工作负载提供灵活分配。此模式是动态任务感知阵列切分的基础。

### 四步双向数据流（彻底消除 DMA）
- Step1: 行 CPU 模式（图像预处理）
- Step2: 列加速器模式（DNN 第 1 层推理）
- Step3: 列 CPU 模式（层间处理）
- Step4: 行加速器模式（DNN 第 2 层推理）
- 中间数据全程保留在 AOMEM，无需 DMA 搬运

### 自定义 ISA（4 条指令）
| 指令 | 功能 | 时序 |
|------|------|------|
| ASI | 保存 CSR 参数（输入向量数、输出向量数、缩放系数、conv/FC 选择） | 6 周期 |
| MSI | 模式切换（CPU→DNN：13-15 周期；DNN→CPU：3 周期） | — |
| SL2 | 多核 CPU 间通过 L2 SRAM 写入通信 | — |
| LL2 | 多核 CPU 间通过 L2 SRAM 读取通信 | — |

### ACT 模块（每行/列 DNN 后处理单元）
- 每行/列配备 1 个 ACT 模块，负责 DNN 后处理功能。
- 功能包括：部分和累积、池化（max/avg）、ReLU 激活、输出缩放。
- 与 AOMEM 协同工作：ACT 处理完成后数据直接写回 AOMEM，无需额外搬运。
- 在 CPU 模式下，ACT 模块通过时钟门控关闭以节省功耗。

---

## 研究计划详细内容引用

以下内容不再在主 instruction 中展开，统一以 `RSNCPU/plan/shen_research_plan_2026-2027_v2.md` 为准：

- 完整时间规划
- `YOLOv11n` 验证递进线索
- 风险矩阵
- 更完整的创新点展开
- 后续优化方向
- 参考文献
- 版本、日期、平台等完整计划元数据
