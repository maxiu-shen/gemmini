# Byte Resume Rewrite Implementation Plan

> **路径约定**：计划中 `RSNCPU/...` 等路径以 Chipyard `generators/` 为根（与 `gemmini/` 同级）；位于 Gemmini 子仓内的材料写作 `gemmini/...`。

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 生成一版面向字节 `IP设计实习生` 岗位的中文简历源稿，并在项目中保存为可继续排版的 `Markdown` 文件。

**Architecture:** 基于旧简历、岗位要求和仓库内 RSNCPU/Gemmini 真实材料重建简历叙事。简历以“芯片前端设计基础 + AI 芯片架构评估能力”双主线展开，第一项目突出 RSNCPU/Gemmini，其他项目压缩为支撑性经历。

**Tech Stack:** Markdown、岗位 JD 分析、PDF 文本提取结果、RSNCPU 研究计划、Gemmini 导读笔记。

---

### Task 1: 收集并约束事实边界

**Files:**
- Read: `沈伟龙实习简历.pdf`
- Read: `字节实习要求.txt`
- Read: `RSNCPU/plan/shen_research_plan_2026-2027_v2.md`
- Read: `RSNCPU/group report/2026-03-16/周报-沈伟龙-2026-03-16.md`
- Read: `gemmini/src/shen_src_explain/shen_Controller_scala_first_pass_reading_guide.md`

**Step 1: 提取岗位关键词**

- 记录岗位更关注的能力：`RTL`、`乱序CPU/GPGPU`、`矩阵/卷积结构评估`、`主动分析问题`

**Step 2: 标记可直接写入简历的事实**

- 教育背景
- 原有 CPU/UVM/AXI/JTAG/BIST 项目
- Gemmini 源码理解、YOLOv11n 算子映射分析、RSNCPU 架构指标口径

**Step 3: 标记只能写成“推进中”的内容**

- bare-metal 部署基线
- RSNCPU 微架构定义
- YOLOv11n INT8 功能验证

**Step 4: 排除不可写内容**

- 未完成的 RTL 集成结果
- 未得到的 FPGA 实测性能
- 虚构训练指标或 tapeout 结果

### Task 2: 设计新的简历结构

**Files:**
- Create: `docs/plans/2026-03-16-byte-resume-design.md`
- Create: `shen_byte_resume_2026.md`

**Step 1: 确定简历定位标题**

- 目标定位为 `微电子/计算机体系结构方向硕士，聚焦 RISC-V、Gemmini 与 AI 芯片架构评估`

**Step 2: 重写章节顺序**

- 基本信息
- 教育背景
- 自我评价
- 核心项目经历
- 专业技能

**Step 3: 提升第一项目权重**

- 将 `RSNCPU/Gemmini` 放到首位
- 用 4-5 条 bullet 写清架构理解、算子映射、指标口径和推进状态

**Step 4: 压缩其他项目**

- `RISC-V CPU`
- `UVM AXI`
- `JTAG FIR / FPGA BIST`

### Task 3: 生成面向岗位的简历正文

**Files:**
- Create: `shen_byte_resume_2026.md`

**Step 1: 重写自我评价**

- 兼顾 RTL 基础、架构分析能力、AI 提效能力

**Step 2: 重写 RSNCPU/Gemmini 项目**

- 用招聘方能快速理解的语言表达系统能力
- 必须包含真实指标口径和具体模块名

**Step 3: 调整项目措辞**

- 少写“学习了什么”
- 多写“建立了什么能力”“完成了什么分析”“支撑了什么目标”

**Step 4: 重写技能清单**

- RTL/验证
- 体系结构/AI 加速器
- 工具链/脚本/AI 提效

### Task 4: 做一次文字级校对

**Files:**
- Modify: `shen_byte_resume_2026.md`

**Step 1: 检查是否贴合字节岗位**

- 是否体现 `芯片前端设计`
- 是否体现 `AI 计算结构评估`
- 是否体现 `学习能力和分析能力`

**Step 2: 检查是否存在夸张表述**

- 删除无法自证的“已完成”“显著提升”类措辞

**Step 3: 检查可读性**

- 每条项目不超过 2 行左右
- 尽量有数字、模块名、目标指标

**Step 4: 完成交付**

- 保存最终 `Markdown` 简历源稿
- 向用户说明该版本适合继续排版成 PDF

### Task 5: 验证文件完整性

**Files:**
- Read: `docs/plans/2026-03-16-byte-resume-design.md`
- Read: `docs/plans/2026-03-16-byte-resume.md`
- Read: `shen_byte_resume_2026.md`

**Step 1: 检查文件是否存在**

Run: 使用文件读取工具打开上述 3 个文件
Expected: 均可正常读取

**Step 2: 检查结构是否完整**

Expected:
- 设计文档包含目标、岗位需求拆解、能力放大边界
- 实施计划包含任务拆分
- 简历正文包含完整简历章节

**Step 3: 不执行 git commit**

Reason: 当前用户未要求提交版本控制
