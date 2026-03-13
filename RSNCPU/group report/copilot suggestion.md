总结得很好 👍，你现在就是想要一个**"为什么复现 SNCPU 的脉动阵列时要优先选 Gemmini 而不是 Eyeriss"**的清晰理由列表。下面我把我们前面聊过的点，整理成一套可以直接写进笔记 / 报告里的总结。

**1. 目标对齐度：SNCPU 是"CPU+脉动阵列"，Eyeriss 是"CNN 数据流"**

* **SNCPU 的核心关注点**：
  + 统一的 2D PE 阵列
  + 在不同模式下扮演 **CPU pipeline** 或 **典型 CNN 脉动阵列**
  + 目标是 **端到端性能 + CPU/NN 融合**，不是单纯卷积能效。
* **Gemmini 的设计目标**：
  + 为 RISC‑V SoC 提供一个**可配置的 GEMM 脉动阵列内核**
  + 强调 **矩阵乘 / NN 核心算子** 与 CPU 的协同
* **Eyeriss 的设计目标**：
  + 探索 **Row‑Stationary (RS) CNN 数据流**
  + 核心创新在于 **数据流和 NoC**，CPU 只是外围控制者

➡ 所以从一开始的「问题设定」上：
**SNCPU ↔ Gemmini 是同一类问题（CPU+脉动阵列协同）**，
而 **Eyeriss 是"如何让 CNN 更省能"的另一类问题**。

**2. 阵列形态：Gemmini 的阵列更接近 SNCPU 的"标准脉动阵列"**

* SNCPU 论文里的加速模式，采用的是**典型 systolic dataflow（WS/OS 风格）**，即规则 2D MAC 阵列，数据沿行/列按拍流动。
* **Gemmini 的核心就是一个参数化的 2D systolic array**：
  + 支持 **output‑stationary / weight‑stationary** 等经典数据流
  + 非常贴近 SNCPU 里描述的"典型脉动 MAC 阵列"。
* **Eyeriss v1 的阵列并不是传统意义上的"纯 systolic array"**：
  + 它依赖一套复杂的 **多播 NoC + Row‑Stationary dataflow**
  + 更像是为卷积设计的「特化阵列 + NoC」，而不是一个"通用矩阵脉动内核"。

➡ 你要复现的是 **SNCPU 里的"典型 systolic 阵列"**，
Gemmini 在结构和数据流上都**一脚踩在这个点上**，
Eyeriss 则会把你带向 RS/NoC 方向，**偏离 SNCPU 的核心。**

**3. 与 CPU 的关系：Gemmini 生来就是给 RISC‑V CPU 紧耦合的**

* **Gemmini**：
  + 原生就是 Chipyard 生态里，挂在 Rocket/BOOM 上的 **指令级可控加速器**。
  + 它已经解决了：
    - CPU 如何向阵列下发指令
    - 数据如何在 CPU 缓存 / 阵列 scratchpad 之间搬运
  + 这些问题和 SNCPU 关心的"CPU–阵列协同"高度重合，只是耦合程度还不如 SNCPU 深，你可以在此基础上进一步改。
* **Eyeriss**：
  + 更像一个**独立 NPU**：
    CPU 在外面喂配置、喂数据，阵列内部由自己的控制和 NoC 运作。
  + 架构假设是"**CPU 和加速器是两个世界**"，这点和 SNCPU 的"统一阵列"思路是相反的。

➡ 复现 SNCPU 时，你后面一定要做的事是：
**研究"CPU ↔ 阵列"边界怎么消除、怎么变紧耦合。**
Gemmini 已经在这个方向上走了一半，而 Eyeriss 一开始就把 CPU 放得很远。

**4. 工程可行性：Gemmini 开源成熟、可裁剪、可直接跑 GEMM**

* **Gemmini**：
  + 完整开源、社区活跃、文档和例程丰富。
  + 你可以：
    - 只裁剪出 **systolic array + 必要控制 + scratchpad/acc**
    - 很快让它在仿真 / FPGA 上跑通矩阵乘 / 简单网络层
  + 作为"第一阶段：先把 SNCPU 的阵列原型跑起来"，非常适合。
* **Eyeriss**：
  + 官方 RTL 没有开源，只有第三方复现，质量和细节不易保证。
  + 工程复杂度高：NoC、RS 数据流、buffer 层级都需要大量时间理解和验证。
  + 对于你现在的目标（**先复现 SNCPU 的 systolic 阵列**），是明显"绕远路"。

➡ 从**工程投入 / 收益比**来看：
**Gemmini = 少量改动就能跑通 SNCPU 风格的阵列**，
Eyeriss = 先花很长时间做"另一类问题"的复现。

**5. 未来演进空间：Gemmini 更适合作为"往 SNCPU 靠拢"的起点**

后面如果你想做的不只是"把阵列跑通"，还包括：

* 尝试让 **阵列行/列在不同模式下扮演 CPU/NN 的角色**
* 研究 **如何把阵列从一个"外设"推向"执行单元"甚至"统一计算阵列"**

那么：

* **从 Gemmini 出发**：
  + 可以一步步：
    - 改 RoCC 接口 → 更紧耦合
    - 调整 memory 路径 → 减少搬运
    - 把部分控制逻辑向 CPU pipeline 靠拢
  + 逐步演化出一个 **SNCPU‑like 的统一阵列架构**。
* **从 Eyeriss 出发**：
  + 你首先要"拆掉"很多专门为 CNN RS dataflow 设计的东西，
  + 再把它变成通用 systolic，再谈 CPU 融合，相当于**绕大圈**。

➡ 作为**"SNCPU 的演化原型"**，Gemmini 给你的改造空间和路径都明显更清晰。

**6. 一句话总结（可以直接写在报告里的版本）**

**在复现 SNCPU 工程、尤其是其内部脉动阵列时，优先选择 Gemmini 而不是 Eyeriss，是因为 Gemmini 在架构目标、阵列形态、与 RISC‑V CPU 的协同方式以及开源工程成熟度上，都与 SNCPU 的"典型 systolic 阵列 + CPU/NN 融合"思路高度一致；而 Eyeriss 的核心创新是 CNN Row‑Stationary 数据流和 NoC，对应的是另一类问题，会把精力从"复现 SNCPU 的阵列与融合思想"转移到"复现特定 CNN 数据流与网络结构"，不利于你当前的工程目标。**

如果你接下来打算**写个小设计文档**来说明你的选型理由，我可以帮你把上面这些点整理成：

* 一个 **1 页的选型说明（背景 + 比较 + 结论）**，
* 或者一张 **"SNCPU vs Gemmini vs Eyeriss" 对比表**，可以直接放进你的开题 / 项目汇报里。

你更想要的是「文字版说明」还是「对比表 + 图示」？
