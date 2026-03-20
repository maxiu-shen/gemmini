# 字节跳动 AI 芯片相关岗位面经汇总

> 本文档整理自公开面经（小红书等），涵盖**芯片研发-AI 芯片（验证岗）**、**AI 芯片架构（校招/实习）** 等技术岗的面试题目与经验，供备考与知识梳理使用。

---

## 一、岗位：芯片研发 - AI 芯片（校招，验证岗）

**来源**：26 字节 AI 芯片验证岗面经  
**岗位**：字节跳动 芯片研发 - AI 芯片（验证方向）  
**时长**：约 60 分钟  
**面试官评价**：专业度高、节奏快、逻辑清晰。

### 1.1 AXI 协议（高频）

1. 为什么 AXI 写有 5 个通道、读只有 2 个通道？
2. 什么是「outstanding」？如何验证？
3. 什么时候会发生背压（backpressure）？
4. 上游 64B、下游 32B 时，如何保证正确传输？
5. **追问**：若 burst length = 5 且第一拍未对齐，一共传多少拍？
6. valid 信号未初始化会有什么问题？
7. 如何实现乱序（Out-of-Order, OoO）？
8. `wstrb`（写选通）信号的作用？

### 1.2 PCIe

1. PCIe 有几层？（提示：Transaction / Data Link / Physical）
2. 各层分别实现什么？
3. **Transaction 层**：TLP 请求类型；posted 与 non-posted 请求的区别。
4. **Data Link 层**：流控机制是什么？
5. **Physical 层**：含 PCS、PMA、SerDes；Gen4 编码 128b/130b；LTSSM（Link Training and Status State Machine）。

### 1.3 RISC 处理器验证

1. 如何验证指令正确性？
2. 如何搭建 C-model？
3. 如何评估处理器性能？

### 1.4 手写 SystemVerilog（编程题）

- 每个元素取值在 100～1000 之间；
- 100～500 与 501～1000 的权重相等；
- 对某数组的 coverage 取值来自集合：{0, 200, 300, 500, 800}。

### 1.5 UVM 机制

1. 从 Sequence 向 Monitor 传参的方法；
2. 通过 transactor（seq_item）传递；
3. 说明 `uvm_config_db`；
4. TLM 通信机制；
5. Factory 机制；
6. 组件与对象的注册宏（`uvm_component_utils` / `uvm_object_utils`）。

### 1.6 SystemVerilog / OOP

1. 父类与子类的关系；
2. 父类句柄能否指向子类对象？
3. 子类句柄能否指向父类对象？如何做强制类型转换（`$cast`）？
4. **追问**：除父子句柄外，`$cast` 还在哪些场景使用？
5. OOP 三大特性；
6. **virtual**：为何在 config_db 里用 virtual？用于传递 virtual interface。

### 1.7 闲聊与流程

- 部门方向与需求：招聘需求多，芯片以自研、内用为主；
- 流程：多为一次技术面 + 一次主管面；
- 难度：从小模块逐步到大型 IP/SoC。

### 1.8 二面补充题（部分）

1. 你做过最有挑战的项目？（候选人自述在 AES 相关回答上翻车）
2. AES 的基本流程；
3. AXI4 为何不能跨 4KB 边界？为什么是 4KB？
4. Master 设 outstanding = 32 时，如何验证？
5. Reorder Buffer（ROB）如何实现与验证？
6. Slave 如何实现乱序？
7. 如何验证队列？常见实现问题？
8. Verilog 中如何产生 100MHz 时钟？为什么用 `initial clk`？
9. UVM 树形结构（组件层次）；
10. Sequence 与 Driver 的通信机制；
11. 如何保证 sequence 发送成功？
12. config_db 机制的原理。

---

## 二、岗位：AI 芯片架构 - 实习一面

**来源**：字节 AI 芯片架构实习一面  
**候选人**：C9 本科三年级  
**结果**：HR 反馈面评很不错，但最终未通过  
**时间**：2025-02-27 | 江苏

### 2.1 面试题目（11 点）

1. **近似计算电路**：具体实现方式。
2. **CNN 的 PE**：CNN 处理单元（PE）的接口与电路结构。
3. **精度评估**：近似计算的精度如何评估。
4. **MoE**：简要介绍 MoE 大模型及其需要的计算模块。
5. **Transformer**：Transformer 结构及所需计算模块。
6. **Attention**：Attention 的输入含义以及如何得到这些输入。
7. **矩阵乘法**：矩阵乘法电路的实现方式及性能估计。
8. **项目深挖**：详细讨论项目，需共享屏幕画电路架构图。
9. **大模型训练**：大模型训练相关问题（候选人自述未答上来）。
10. **DeepSeek**：是否看过 DeepSeek 的论文。
11. **手撕代码**：序列检测。

---

## 三、岗位：AI 芯片架构 - 实习二面

**来源**：字节 AI 芯片架构 intern 二面  
**时间**：2025-06-05 | 江苏  

**候选人感受**：二面结束，难度比一面更大/更绕；要点有提到，但可能漏了部分关键词；部分题目一开始没完全理解，在面试官追问/提示后反推出了过程。

### 3.1 面试题目

1. **简单自我介绍**
2. **画 AI 芯片架构**：开草稿纸，要求画出 AI 芯片架构图。
3. **DMA 架构**
4. **Scratchpad 的乒乓（Ping-Pong）Pipeline**
5. **矩阵 Tiling**
6. **跨时钟域合流**：异步 FIFO + 仲裁。
7. **多时钟建模**
8. **多 Chiplet 中 SerDes 的同步**
9. **Chiplet 之间的接口及行为**

---

## 四、按主题归类速查

| 主题           | 出现岗位/轮次           | 典型问题摘要 |
|----------------|-------------------------|--------------|
| AXI            | 芯片研发-AI 芯片（验证岗） | 五通道/两通道原因、outstanding、背压、位宽转换、wstrb、OoO、4KB 边界 |
| PCIe           | 芯片研发-AI 芯片（验证岗） | 三层结构、TLP、posted/non-posted、流控、PCS/PMA/SerDes、LTSSM |
| UVM / 验证     | 芯片研发-AI 芯片（验证岗） | config_db、TLM、Factory、Sequence–Driver、组件/对象宏、ROB/队列验证 |
| SV / OOP       | 芯片研发-AI 芯片（验证岗） | 父子类、$cast、virtual、config_db 传参 |
| RISC/处理器    | 芯片研发-AI 芯片（验证岗） | 指令正确性验证、C-model、性能评估 |
| AI 芯片架构    | 架构实习一/二面   | 整体架构图、DMA、Scratchpad 乒乓、矩阵 Tiling、PE 接口与结构 |
| 近似计算       | 架构实习一面      | 实现方式、精度评估 |
| Transformer/MoE | 架构实习一面      | 结构、计算模块、Attention 输入与获取方式 |
| 矩阵乘法       | 架构实习一面      | 电路实现与性能估计 |
| 时钟与 CDC     | 架构实习二面      | 跨时钟域合流、异步 FIFO、仲裁、多时钟建模 |
| Chiplet/SerDes | 架构实习二面      | 多 Chiplet SerDes 同步、Chiplet 间接口与行为 |
| 手写/手撕      | 多岗位            | SV 约束/coverage 题、序列检测 |

---

## 五、备考建议（简要）

- **芯片研发-AI 芯片（验证岗）**：重点掌握 AXI（通道、outstanding、背压、位宽、wstrb、OoO）、PCIe 三层与 TLP、UVM（config_db、TLM、Factory、Sequence–Driver）、SV 面向对象与 virtual/$cast，以及 RISC 验证与 C-model；可顺带准备时钟生成、ROB/队列验证等二面题。
- **AI 芯片架构**：能画出 AI 芯片整体架构（含 DMA、Scratchpad、计算阵列）；熟悉 Scratchpad 乒乓流水、矩阵 Tiling、PE 接口；了解 Transformer/MoE 与 Attention 所需计算模块；准备矩阵乘实现与性能估计；补充大模型训练与 DeepSeek 等论文；掌握跨时钟域、多时钟、Chiplet/SerDes 等系统级问题；手撕序列检测要熟练。

---

*文档整理日期：2026-03-19。内容仅供个人学习与备考，非官方题库。*
