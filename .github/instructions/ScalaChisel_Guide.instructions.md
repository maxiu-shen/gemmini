---
description: Gemmini Scala/Chisel 开发指南：编写须知与编码规范（编辑 .scala 文件时自动加载）
applyTo: "**/*.scala"
---

# Scala/Chisel 编码规范与编写须知

## 编写前必读清单

编写任何 Scala/Chisel 代码前，**必须按顺序完成以下参考阅读**：

1. **阅读项目 README**：**必须先阅读** 项目根目录 [README.md](../../README.md)，理解 Gemmini 整体架构和数据流水线
2. **查阅硬件配置**：阅读 [GemminiConfigs.scala](../../src/main/scala/gemmini/GemminiConfigs.scala)，理解所有硬件参数定义、`GemminiArrayConfig` 结构和 `generateHeader()` 方法
3. **参考相关硬件模块**：根据要修改/新增的功能，阅读 `src/main/scala/gemmini/` 下的相关模块源码，理解现有架构和接口：
   - [Controller.scala](../../src/main/scala/gemmini/Controller.scala) — 顶层控制器、指令解码
   - [LoopMatmul.scala](../../src/main/scala/gemmini/LoopMatmul.scala) / [LoopConv.scala](../../src/main/scala/gemmini/LoopConv.scala) — CISC→RISC 展开逻辑
   - `PE.scala` → `Tile.scala` → `Mesh.scala` → `MeshWithDelays.scala` — 脉动阵列层次
   - [Arithmetic.scala](../../src/main/scala/gemmini/Arithmetic.scala) — 类型类系统
4. **参考 Spike 功能模型**：阅读 `software/libgemmini/gemmini.cc`，确认硬件行为与功能模型一致
5. **参考 C API**：查看 `software/gemmini-rocc-tests/include/gemmini.h`，理解软硬件接口约定
6. **参考 Chipyard 配置**：查看 [chipyard/GemminiConfigs.scala](../../chipyard/GemminiConfigs.scala) 和 [Configs.scala](../../src/main/scala/gemmini/Configs.scala)，理解配置组合方式

## 配置传递模式
所有参数通过 `GemminiArrayConfig[T, U, V]`（定义在 [GemminiConfigs.scala](../../src/main/scala/gemmini/GemminiConfigs.scala)）作为构造参数传递。模块内使用 `import config._` 直接访问字段。Rocket Chip 的 `p(Key)` 仅用于核心级参数（如 `xLen`）。

## 类型参数化
- 容器类使用 context bound：`T <: Data : Arithmetic`
- 计算模块使用显式隐式参数：`(implicit ev: Arithmetic[T])` 配合 `import ev._`
- 类型类定义在 [Arithmetic.scala](../../src/main/scala/gemmini/Arithmetic.scala)，提供 `SInt`/`UInt`/`Float` 实例

## 创建配置变体
在 [Configs.scala](../../src/main/scala/gemmini/Configs.scala) 的 `GemminiConfigs` object 中使用 `defaultConfig.copy(meshRows = 8, ...)`。Chipyard 级 SoC 配置在 [chipyard/GemminiConfigs.scala](../../chipyard/GemminiConfigs.scala) 中组合 Gemmini mixin + 核心配置 + 总线宽度。

## 命名约定
- `snake_case`：大多数参数和 val（`sp_banks`、`acc_capacity`、`reservation_station_entries`）
- `camelCase`：类型相关名称（`inputType`、`meshRows`）
- `SCREAMING_SNAKE`：派生常量（`DIM`、`BLOCK_ROWS`、`SP_ROWS`）

## 常见模式
- **IO 声明**：匿名 `Bundle` 内联在 `val io = IO(new Bundle { ... })`
- **状态机**：`val s_idle :: s_active :: Nil = Enum(2)`
- **双 Gemmini 配置**：`DualGemminiConfig` 实例化 Int8 + BF16 两个 Gemmini，分别使用 `custom3`/`custom2` 操作码，通过 `SharedExtMem` 共享外部存储
