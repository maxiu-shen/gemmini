# 沈伟龙

复旦大学电子信息硕士在读（2027 届） | 求职方向：IP 设计实习生 / AI 芯片架构与 RISC-V 方向

- 电话：17816956376
- 邮箱：24212020160@m.fudan.edu.cn

## 教育背景

**复旦大学** | 电子信息（硕士） | 2024.09 - 至今  
- 研究方向：计算机体系结构、RISC-V、Gemmini 脉动阵列加速器、AI 芯片架构评估
- 相关课程：计算机体系结构、集成电路设计基础、先进数字集成电路设计、数字集成电路中的高级综合技术

**宁波大学** | 微电子科学与工程（本科） | 2020.09 - 2024.06  
- 相关课程：Verilog 程序设计、数字集成电路、模拟集成电路、信号与系统、C 语言基础

## 自我评价

- 具备扎实的芯片前端设计基础，熟悉 `Verilog/SystemVerilog/UVM` 开发流程，做过 `RISC-V CPU` 设计与验证、`AXI` 协议验证等项目。理解乱序执行微架构原理（`Tomasulo/ROB/寄存器重命名/分支预测/推测执行`），能对比分析顺序与乱序流水线在 ILP 开发、冒险处理与面积功耗间的设计权衡。
- 正在围绕 `RSNCPU + Gemmini` 推进可重构 AI 计算架构研究，能够从 `矩阵/卷积/数据流/片上存储/带宽` 视角分析算子与硬件映射关系，形成面向 AI 芯片设计评估的工程化理解。
- 具备较强的主动学习与问题分析能力，能够快速阅读 Chipyard/Gemmini 源码、搭建验证路径，并使用 `Python/tcl/Perl/Makefile + AI` 提升仿真、日志分析、源码理解与文档沉淀效率。

## 核心项目经历

### RSNCPU 可重构脉动阵列处理器研究（对标 SNCPU / Gemmini） | 2026.03 - 至今

- 围绕自动驾驶目标检测部署，完成 `YOLOv11n` 在 `BDD100K` 上的训练与验证流程，建立后续硬件映射所需的模型基线；结合公开轻量化检测结果设定并对齐关键指标，验证集 `AP50` 达到约 `48%`、`AP(0.5:0.95)` 约 `25%` 的可行区间。
- 基于训练后的 `YOLOv11n` 网络结构，完成面向 `Gemmini` 的算子级软硬件划分：将 `Conv/Pointwise/DWConv/MatMul/ResAdd/Pool` 等计算密集部分映射到脉动阵列，将 `SiLU/Upsample/Concat/Split/reshape` 等轻量或控制型操作划分到 CPU 侧，形成端到端部署方案。
- 系统梳理 `Gemmini` 主链实现，能够从 `RoCC` 命令入口追踪至 `LoopConv/LoopMatmul`、`ReservationStation`、`Load/Store/Execute Controllers`、`Scratchpad/Accumulator`、`Mesh/Tile/PE`，形成模块化导读与源码理解框架。
- 在此基础上对标 `SNCPU` 与 `Gemmini` 建立 `RSNCPU` 可重构统一处理器的架构评估口径，围绕 `10x10 INT8 WS` 阵列、`200MHz` FPGA 目标频率、`40 GOPS` 峰值算力、`DNN 模式 PE 利用率 >=95%` 与端到端时延改善 `>=30%` 分析设计空间。
- 围绕 `Gemmini` bare-metal C 路线推进矩阵乘与卷积验证，覆盖 `OS/WS` 数据流、手动 `tiling`、直接卷积与 `tiled_conv_auto` 等路径，建立从底层 `mvin/preload/compute/mvout` 指令到高级 API 的对应理解，为后续 `RSNCPU` 微架构定义与 INT8 部署验证提供基线。

### 基于蜂鸟 E203 风格的 RISC-V 处理器设计与验证 | 本科阶段

- 参考蜂鸟 `E203` 的轻量级 `RISC-V` 微架构，使用 `Verilog` 设计处理器核心，完成两级流水主通路、寄存器堆、`LSU`、`SRAM` 接口及基础控制逻辑划分，对低功耗 MCU 类内核的模块边界与数据通路组织形成直观理解。
- 搭建功能验证环境，围绕算术逻辑、分支跳转、`load/store`、访存时序与基础冒险处理编写定向测试，结合波形分析定位并修正流水线控制与访存路径问题，验证核心功能正确性。
- 结合课程对乱序执行架构的学习（`Tomasulo` 算法、`ROB`、分支预测与推测执行），对比分析顺序与乱序流水线在数据冒险处理、`ILP` 开发与面积功耗权衡上的差异，形成对不同复杂度处理器微架构设计空间的理解。

### 基于 UVM 的 AXI 总线协议验证平台 | 本科阶段

- 独立搭建 `UVM` 验证环境，完成 `transaction / sequencer / driver / monitor / scoreboard` 等组件开发，形成可扩展的 `AXI` 协议验证平台。
- 围绕 `AXI` 五个独立通道与 `FIXED/INCR/WRAP` 三类 `burst`，编写 `50+` 组随机与定向测试，覆盖基础读写、握手阻塞、突发传输等场景，功能覆盖率收敛至 `85%+`。

### FPGA CLB 建立保持时间内建自测试电路 | 本科阶段

- 基于 `BIST` 思路设计 `FPGA` 内部 `CLB` 寄存器建立/保持时间测量电路，相较片外测试具备更高的时间分辨率。
- 使用 `Python` 与 `Perl` 编写自动化脚本，完成从测试配置生成、任务执行到结果汇总的全流程自动化，提高实验迭代效率与可复现性。

## 专业技能

### 设计与验证

- 熟练使用：`Verilog`
- 掌握：`SystemVerilog`、`UVM`
- 了解并可用于架构原型开发：`Chisel/Scala`
- 熟悉协议与体系结构基础：`AXI`、`RISC-V`、存储层次结构、虚拟内存、并行计算
- 理解乱序执行微架构原理：`Tomasulo`、`ROB`、寄存器重命名、推测执行、分支预测

### AI 芯片架构与体系结构

- 熟悉 `Gemmini` 脉动阵列加速器基本架构与数据流，理解 `WS/OS`、`Scratchpad/Accumulator`、`ReservationStation`、`LoopMatmul/LoopConv` 等关键机制
- 能从算子特性出发分析 `Conv/MatMul/Depthwise/Residual/Pooling` 等在阵列上的映射方式，以及 `SiLU/Upsample/Concat` 等操作回退 CPU 的原因
- 具备 `INT8` 推理部署、算子级软硬件划分、片上数据复用与端到端时延评估意识
- 了解 `Transformer/Large Model` 常见核心算子（`MatMul`、`Softmax`、`LayerNorm`）的数据流特征与带宽瓶颈

### 工具与脚本

- EDA / 开发工具：`VCS`、`ModelSim`、`Vivado`、`ICC2`、`Chipyard/Gemmini` 开发环境
- 验证与分析：具备 `Spike` 裸机验证经验，理解从功能验证到周期级仿真的基本路径
- 脚本与自动化：`Python`、`tcl`、`Perl`、`Makefile`
- AI 提效：能够利用 AI 辅助 `Simulation/DFT` 流程自动化、日志归因分析、源码阅读、实验记录整理与文档生成

### 外语能力

- `CET-4/6`，具备英文技术文档与论文快速阅读能力
