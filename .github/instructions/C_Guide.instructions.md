---
description: Gemmini C 测试开发指南：编写须知与规范（编辑 bareMetalC/*.c 文件时自动加载）
applyTo: "**/bareMetalC/*.c"
---

# C 测试开发指南

## 编写前必读清单

编写任何 C 测试代码前，**必须按顺序完成以下参考阅读**：

1. **阅读 README**：**必须先阅读** [README.md](README.md)，理解指令语义和整体软件架构
2. **参考 include 目录**：**重点参考** `software/gemmini-rocc-tests/include/` 目录中的自定义指令宏和高级 API，确保代码正确性和兼容性
   - **API 层级**：底层指令（`gemmini_mvin`、`gemmini_mvout`、`gemmini_preload`、`gemmini_compute_preloaded`）和高层 API（`tiled_matmul_auto`、`tiled_conv_auto`），均定义在 [include/gemmini.h](../../software/gemmini-rocc-tests/include/gemmini.h)
3. **参考已有测试**：查看 `bareMetalC/` 目录下功能相近的已有测试文件，了解实际 API 用法（如矩阵乘法看 `matmul.c` 系列，卷积看 `conv.c` 系列，数据搬运看 `mvin_mvout.c` 系列）；新测试应基于 `bareMetalC/template.c` 模板
4. **查阅硬件源码**：阅读 `src/main/scala/gemmini/` 下的硬件源码，确认硬件架构、自定义宏、高级 API 的实际行为，重点包括：
   - [GemminiConfigs.scala](../../src/main/scala/gemmini/GemminiConfigs.scala) — 硬件参数定义、`generateHeader()` 生成的宏
   - [Controller.scala](../../src/main/scala/gemmini/Controller.scala) — 顶层控制器、指令解码逻辑
   - [LoopMatmul.scala](../../src/main/scala/gemmini/LoopMatmul.scala) / [LoopConv.scala](../../src/main/scala/gemmini/LoopConv.scala) — CISC 指令展开为 RISC 微操作的逻辑
5. **参考 Spike 功能模型**：阅读 `software/libgemmini/gemmini.cc`，理解每条指令的精确语义
6. **编译 + Spike 仿真验证**：代码编写完成后，必须编译并通过 Spike 仿真验证正确性

## 测试模板

所有裸机测试在 `software/gemmini-rocc-tests/bareMetalC/`，唯一需要的头文件是 `"include/gemmini_testutils.h"`。

```c
#include "include/gemmini_testutils.h"
int main() {
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);
    // ... 黄金计算 → Gemmini 执行 → 结果比较 ...
    exit(0);  // 0=通过, 1=失败
}
```

## 关键要点

- **数据类型**：使用自动生成的 `elem_t`、`acc_t`、`scale_t` 类型别名，数组用 `row_align(N)` 对齐
- **`#ifdef FAST`**：CI 环境下使用较小维度，正常测试使用完整维度
- **Makefile 注册**：新增的 `.c` 文件必须将文件名主干添加到 `software/gemmini-rocc-tests/bareMetalC/Makefile` 的 `tests` 列表中

