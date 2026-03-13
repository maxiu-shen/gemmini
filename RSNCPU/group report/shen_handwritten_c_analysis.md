# Gemmini 部署 YOLO v11：手写 C 路线深度分析

> 本文档总结了关于在 Gemmini 加速器上使用手写 C 代码部署 YOLO v11 的完整讨论，涵盖行业定位、技术对比、适配性分析及结论。

---

## 一、背景与核心问题

我们的最终目标是在包含 Gemmini 脉动阵列加速器的 RISC-V SoC 上部署 YOLO v11 目标检测网络，并且后续还有两个延伸需求：

1. **在整个 SoC 上部署 Linux 系统**，在 Linux 用户态运行 YOLO v11 推理
2. **修改 SoC 硬件**，实现"融合可重构"架构——SoC 可被配置为加速器模式或 CPU 模式

在此背景下，我们系统性地分析了**手写 C 代码**这一部署路线的合理性。

---

## 二、行业边缘推理部署的四个层级

| 层级 | 方式 | 典型场景 | 代表 |
|------|------|----------|------|
| ① 通用推理框架 | 跨平台推理框架，通过可插拔后端适配不同硬件 | 手机/树莓派等有 OS 的通用处理器 | TFLite, ONNX Runtime, PyTorch Mobile |
| ② 厂商专用工具链 | 编译器（图优化+量化+代码生成）+ Runtime（加载执行），二者配套使用 | 商用成熟加速器 | TensorRT, OpenVINO, CANN, MagicMind |
| ③ **手写 C + 硬件 API** | 手动调用硬件加速指令 | **学术/早期/定制加速器** | **Gemmini 在此层级** |
| ④ 全定制 RTL | 硬件直接固化网络 | 极端功耗约束的 ASIC | 某些 IoT 芯片 |

**关键认识**：手写 C 不是"落后"的做法。所有商用加速器的第一版部署都是手写 kernel（TensorRT 最初也是工程师手写 CUDA kernel），层级②的工具链本质上是将层级③的工作自动化。

---

## 三、PyTorch 训练 ≠ PyTorch 部署

一个常见误解是"既然用 PyTorch 训练，部署也应该用 Python"。实际的行业标准流程是：

```
PyTorch (训练, Python)
    ↓ 导出
ONNX / TorchScript (中间表示)
    ↓ 编译/转换
目标平台原生代码 (C/C++/汇编)
    ↓ 运行
硬件加速器
```

即使是 ONNX Runtime，底层调用的也是 C++ 写的 kernel。**PyTorch 只负责训练和导出，从来不参与最终推理执行。**

---

## 四、芯片厂商编译器团队做了什么

芯片厂商的完整工具链 = **三大件**：

### 1. 算子库（Kernel Library）
- 手写各种高度优化的 C/C++/汇编计算 kernel（Conv、MatMul、Pooling、ReLU……）
- 每个算子针对不同参数可能有几十种实现，运行时选择最优
- 代表：cuDNN (NVIDIA)、MKL-DNN (Intel)
- **对应 Gemmini**：`gemmini.h` 中的 `tiled_conv_auto`、`tiled_matmul_auto` 等（已有）

### 2. 模型编译器/优化器（Model Compiler）
包含多个子步骤：
- **图优化**：算子融合（Conv+BN+ReLU → 一个融合 kernel）、常量折叠、死代码消除
- **量化**：FP32 → INT8 的量化参数校准
- **算子调度**：每个节点选择最优 kernel 实现，决定内存分配策略
- **代码生成**：生成调用 kernel 库的代码，或生成序列化的执行计划

### 3. 运行时（Runtime）
- 加载编译产物，动态调度执行 kernel
- 代表：TensorRT Runtime、ONNX Runtime
- **Runtime 是当前业界最主流的部署方式**
- 但 Runtime 能存在的前提是厂商已投入大量人力完成了上述三件套

### 与手写 C 的对应关系

| 厂商工具链 | 手写 C 等价操作 |
|------------|----------------|
| ① 算子库 | **已有** — `tiled_conv_auto` 等 Gemmini API |
| ② 图优化 | **手动完成** — 在 PyTorch 侧做 BN 融合、量化 |
| ③ 算子映射 | **手动完成** — Python 脚本把每层映射到 API 调用 |
| ④ 代码生成 | **手动完成** — 编写 `shen_yolov11n.c` |
| ⑤ Runtime | **不需要** — 裸机直接跑，无 runtime 开销 |

---

## 五、Runtime 是否更好？

> **补充说明**：层级②的厂商工具链（如 TensorRT）通常包含**编译器 + Runtime 两个组件**。编译器负责将模型优化编译为二进制执行计划（engine），Runtime 负责加载该 engine 并调度 kernel 执行。下表列出的是这些工具链中 Runtime 组件的部分：

Runtime 确实是业界主流：

| 工具链（Runtime 组件） | 厂商 | 目标硬件 | 对应编译器组件 |
|---|---|---|---|
| TensorRT Runtime | NVIDIA | GPU | TensorRT Builder/Optimizer |
| OpenVINO Runtime | Intel | CPU/iGPU/VPU | Model Optimizer |
| CANN Runtime | 华为 | 昇腾 NPU | ATC 编译器 |
| Core ML Runtime | Apple | ANE/GPU | coremltools 转换器 |
| TFLite Runtime | Google | ARM CPU/GPU/EdgeTPU | TFLite Converter |
| ONNX Runtime | 微软 | 多平台 | 各 EP 自带优化 |

**但 Runtime 对 Gemmini 不可行**，原因是：
- onnxruntime-riscv 项目已停滞 5 年（~2021），opset 仅支持 ≤11（YOLO v11 需要 ≥13）
- 缺少 SiLU、Resize、Concat、Split 等 YOLO v11 必需算子
- RISC-V 自定义加速器生态尚处早期，没有通用编译器能覆盖所有自定义指令集

**手写 C 和 Runtime 不是对立的**——手写 C 是 Runtime 的前身/底层。

---

## 六、手写 C 代码量分析

### 现有网络的代码量参考

| 网络 | 层数 | 参数量 | C 代码行数 |
|------|------|--------|------------|
| ResNet-50 | 50 层 | 23M | 2115 行 |
| MobileNet | ~28 层 | 3.4M | 1711 行 |
| AlexNet | 8 层 | 60M | 412 行 |

### 代码结构特点

每层的代码模式完全一致，~90% 是机械重复：

```c
// 每层就是一个 API 调用 + 改参数编号
tiled_conv_auto(
    conv_N_params.batch_size, conv_N_params.in_row_dim, ...
    (elem_t*)conv_N_in, (elem_t*)conv_N_w, (acc_t*)conv_N_b, (elem_t*)conv_N_out,
    RELU, conv_N_params.output_scale,
    conv_N_params.pool_size, conv_N_params.pool_stride, conv_N_params.pool_padding,
    tiled_matmul_type);
```

权重参数文件（`*_params.h`）由 Python 脚本从 PyTorch 模型自动导出。

### YOLO v11n 估算

| 部分 | 估算行数 | 备注 |
|------|----------|------|
| 主体网络层调用 | ~1500 行 | 60 层 × ~25 行/层 |
| SiLU 软件实现 | ~20 行 | 查表法或分段线性近似 |
| Resize/Upsample | ~40 行 | 最近邻插值，CPU 执行 |
| Concat / Split | ~20 行 | 内存拷贝/指针操作 |
| 检测头后处理 | ~100 行 | NMS、bbox 解码 |
| 样板代码 | ~100 行 | main/初始化 |
| **合计** | **~1800 行** | |

真正需要手写思考的自定义代码 < 200 行，其余可脚本自动生成。

---

## 七、Linux 部署适配性

### 结论：零修改即可支持

Gemmini 的 C 代码从一开始就设计了裸机/Linux 双模式：

```c
#ifndef BAREMETAL
#include <sys/mman.h>
#endif

int main() {
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {  // Linux 下锁定内存页
        perror("mlockall failed");
        exit(1);
    }
#endif
    gemmini_flush(0);
    // 以下代码裸机/Linux 完全一样
    ...
}
```

构建系统自动生成两个版本：
- `*-baremetal`：裸机 Spike/RTL 仿真用
- `*-linux`：Linux 用户态程序

已有先例：`resnet50-linux` 已在 Gemmini SoC 的 Linux 上成功运行。

---

## 八、融合可重构硬件适配性

### 结论：手写 C 比 Runtime 更适合

| 考量维度 | Runtime 框架 | 手写 C |
|----------|-------------|--------|
| 模式切换粒度 | 整个模型只能选一个 EP | **逐层甚至逐算子**可选不同模式 |
| 硬件感知 | 框架隔离了硬件细节 | **直接控制**哪些操作走加速器、哪些走 CPU |
| 可重构适配 | 需要改框架源码（几十万行 C++） | **改一个 enum 值**即可 |
| 新硬件模式扩展 | 需要写新的 EP provider | **加一个 enum 分支**即可 |

### 分层解耦架构

```
┌─────────────────────────────────┐
│  shen_yolov11n.c（网络拓扑）     │ ← 不需要改，和硬件模式无关
├─────────────────────────────────┤
│  gemmini.h / shen_reconfig.h    │ ← 适配层：模式判断 + 指令选择
│  （硬件抽象 API）                │    硬件改了只改这里
├─────────────────────────────────┤
│  RoCC 自定义指令 / CSR 寄存器    │ ← 硬件改动
│  （可重构控制接口）               │
└─────────────────────────────────┘
```

Gemmini API 已内置模式切换：

```c
enum tiled_matmul_type_t { OS, WS, CPU };

// 扩展为：
enum tiled_matmul_type_t { OS, WS, CPU, HYBRID };  // 加一个融合模式

// 逐层指定执行模式
tiled_conv_auto(..., WS);       // 计算密集层 → 加速器
shen_silu_cpu(output, size);    // SiLU → CPU
tiled_conv_auto(..., HYBRID);   // 某些层 → 融合模式
```

网络代码写一次，硬件怎么改都只需调整中间 API 层。

---

## 九、手写 C 的利与弊总结

### ✅ 优势

| 优势 | 说明 |
|------|------|
| **性能无损** | 直接调用硬件加速 API，无框架调度开销，性能与商用编译器生成的代码持平甚至更优 |
| **完全可控** | 逐层决定执行设备、数据流、tiling 策略，适合研究和调优 |
| **Linux 零成本适配** | 同一份代码通过条件编译同时支持裸机和 Linux |
| **硬件改动友好** | 分层解耦设计，改硬件只需改中间 API 层，网络代码无需变动 |
| **可重构最佳选择** | 支持逐算子模式切换，天然适配融合可重构架构 |
| **代码量可控** | YOLO v11n 估计 ~1800 行 C + Python 自动生成权重，真正手动编写 < 200 行 |
| **调试直观** | Spike 快速功能验证 → Verilator 周期精确仿真 → Linux 实机运行，流程清晰 |

### ❌ 劣势

| 劣势 | 说明 | 缓解措施 |
|------|------|----------|
| **不可自动适配新模型** | 换一个网络需要重新写/生成 C 代码 | 编写 Python 脚本从 ONNX 自动生成 C 代码 |
| **量化需手动处理** | 需自己做 PTQ/QAT 并提取 scale 参数 | PyTorch 量化工具链 + 导出脚本 |
| **缺乏图优化** | 算子融合需手动决策 | YOLO v11 结构规整，手动融合工作量不大 |
| **维护成本** | 模型更新需重新生成代码和权重 | 维护好 Python 导出脚本即可一键重新生成 |
| **通用性差** | 绑定 Gemmini 硬件，不可移植到其他加速器 | 学术研究场景下这不是真正的问题 |

---

## 十、已掌握技能 vs YOLO v11 需求差距分析

### 已有的 13 个学习测试（shen_* 系列）

通过系统性的递进式学习，已掌握以下 Gemmini 核心概念：

**矩阵乘法系列（7 个）：**

| 测试文件 | 掌握的概念 |
|----------|-----------|
| `shen_test_os.c` | OS 数据流基础：底层 `mvin/preload/compute/mvout` 指令 |
| `shen_test_ws.c` | WS 数据流基础：底层指令的 WS 模式切换 |
| `shen_test_ws_tiled.c` | 手动分块（tiling）：K 维分 2 块，累加器累加 |
| `shen_test_ws_tiled_opt.c` | 流水线优化：去掉 printf 恢复指令流水线，调整计算顺序 |
| `shen_test_ws_tiled_loop.c` | CISC 循环指令：`gemmini_loop_ws` 硬件循环展开器 |
| `shen_test_ws_unaligned.c` | 非对齐维度：30×40×50 矩阵，padding 处理 |
| `shen_test_os_tiled.c` | OS 数据流分块：K 维分块 + OS 累加模式 |

**卷积系列（6 个）：**

| 测试文件 | 掌握的概念 |
|----------|-----------|
| `shen_test_conv_manual.c` | 手动 im2col 卷积：CPU 做 im2col → 底层指令矩阵乘法 |
| `shen_test_conv_manual_opt.c` | 卷积流水线优化：消除 printf 中断，恢复指令流水 |
| `shen_test_conv_manual_2ch.c` | 多输出通道：扩展到 2 个输出通道，J 维分块 |
| `shen_test_conv_implicit.c` | 隐式 im2col：mvin 时直接按 kernel 窗口读取输入 |
| `shen_test_conv_direct.c` | 直接地址计算卷积：数据无重复存储，分组 compute |
| `shen_test_conv_auto.c` | 高级 API `tiled_conv_auto`：与手动版对比 |

### Gemmini 可用的高级 API 与硬件能力

**高级 API（`tiled_*_auto` 系列）：**

| API | 用途 |
|-----|------|
| `tiled_conv_auto` | 标准卷积（支持 stride, padding, dilation, 池化融合） |
| `tiled_conv_dw_auto` | Depthwise 卷积（每通道独立卷积） |
| `tiled_conv_downsample` | 特化下采样卷积（kernel=1, stride=2） |
| `tiled_matmul_auto` | 通用矩阵乘法（支持转置、缩放、bias、激活融合） |
| `tiled_resadd_auto` | 残差加法 C = scale_A·A + scale_B·B（支持 ReLU 融合） |
| `tiled_global_average_auto` | 全局平均池化 |
| `tiled_norm_auto` | LayerNorm / IGELU / Softmax 归一化 |

**硬件支持的激活函数：**

| 激活类型 | 值 | 备注 |
|---------|---|------|
| `NO_ACTIVATION` | 0 | 无激活 |
| `RELU` | 1 | 硬件融合在 store 阶段 |
| `LAYERNORM` | 2 | 需要 `HAS_NORMALIZATIONS` 硬件特性 |
| `IGELU` | 3 | 整数近似 GELU |
| `SOFTMAX` | 4 | 需要 `HAS_NORMALIZATIONS` 硬件特性 |

**注意：没有 SiLU/Swish、Sigmoid、LeakyReLU、Mish 等激活函数的硬件支持。**

### YOLO v11 各操作的支持情况

#### ✅ Gemmini 硬件已覆盖（计算密集型核心）

| YOLO v11 操作 | Gemmini 支持方式 |
|---------------|------------------|
| 标准卷积 (Conv2d) | `tiled_conv_auto` — 任意 stride、padding、dilation |
| 1×1 点卷积 (Pointwise) | `tiled_conv_auto`（kernel_dim=1） |
| Depthwise 卷积 | `tiled_conv_dw_auto` |
| 矩阵乘法（检测头） | `tiled_matmul_auto` |
| 残差连接 (Add) | `tiled_resadd_auto` |
| ReLU 激活 | 硬件融合在 store 阶段 |
| 最大池化 (MaxPool) | 融合在 `tiled_conv_auto` 的 pool 参数中 |
| 全局平均池化 | `tiled_global_average_auto` |
| Batch Norm | 推理时折叠进卷积的权重和 bias（标准做法，不需要额外操作） |

#### ❌ 需要软件实现的操作（关键差距）

| YOLO v11 操作 | 差距说明 | 实现方案 |
|---------------|---------|----------|
| **SiLU/Swish** ($x \cdot \sigma(x)$) | 硬件无此激活，这是 YOLO v11 最核心的激活函数 | INT8 查找表 (LUT) 或分段线性近似，~20 行 C |
| **Sigmoid** ($\sigma(x)$) | SiLU 的组成部分 | 与 SiLU 一起用 LUT 实现 |
| **上采样 (Upsample/Nearest)** | Gemmini 完全没有 upsample 支持 | CPU 端最近邻插值，~40 行 C |
| **Concat 拼接** | 通道维度拼接 | CPU 端内存拷贝/重排，~15 行 C |
| **Split** | 通道维度分割 | 指针偏移操作，~5 行 C |
| **C2f/C3k2 模块** | YOLO v11 特有的瓶颈模块 | 用上述基础操作组合编排（多次卷积 + Split + Concat） |
| **SPPF** | 空间金字塔池化 | 多次 MaxPool 级联（可用硬件融合）+ Concat（软件） |
| **检测头 reshape/permute** | 张量维度变换 | CPU 端纯内存操作 |
| **量化方案** | INT8 量化参数校准 | PyTorch PTQ + 导出 per-channel scale |

#### 学习路线总结

```
已掌握 ✅                          需要学习 ❌
─────────────────────────────────────────────────────────
底层指令 (mvin/compute/mvout)      SiLU/Sigmoid 的 INT8 近似实现
OS / WS 数据流                     上采样 (Nearest Neighbor) 实现
手动 tiling 与累加器控制            Concat/Split 内存操作
流水线优化                          C2f/SPPF 模块的组合编排
非对齐维度处理                      INT8 量化方案设计（PTQ + scale 导出）
im2col 策略对比                     多层网络内存管理（buffer 复用）
tiled_conv_auto 高级 API            Python 导出脚本（PyTorch → params.h）
                                    检测头后处理（NMS、bbox 解码）
                                    NHWC 张量布局下的数据编排
```

---

## 十一、最终结论与推荐路线

**手写 C 是当前 Gemmini 上部署 YOLO v11 的最优且唯一实际可行的路线。**

推荐实施路径：

```
Phase 1: 裸机验证（当前阶段）
  ├── Python 脚本：PyTorch YOLO v11n → 量化 → 导出 params.h + 生成 C 代码框架
  ├── 手写：SiLU(查表)、Resize(最近邻)、Concat/Split、检测头后处理
  ├── Spike 功能验证
  └── Verilator 周期精确仿真

Phase 2: Linux 部署
  ├── 同一份代码编译为 *-linux 版本
  ├── 打包进 rootfs overlay
  └── 在 SoC Linux 上运行验证

Phase 3: 融合可重构硬件
  ├── 修改 Chisel 硬件（新增可重构控制逻辑）
  ├── 扩展 enum tiled_matmul_type_t 增加新模式
  ├── 新增 shen_reconfig.h 适配层
  └── 网络代码 shen_yolov11n.c 基本无需修改
```

这条路线确保了：**写一次网络代码，三个阶段全部复用。**
