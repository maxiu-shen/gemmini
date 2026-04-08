# `Controller.scala` 中 `ExecuteController <-> Scratchpad/Accumulator` 连接说明

> 范围：解释 `Controller.scala` 中下面 5 组连接  
> `ex_controller.io.srams.read <> spad.module.io.srams.read`  
> `ex_controller.io.srams.write <> spad.module.io.srams.write`  
> `spad.module.io.acc.read_req <> ex_controller.io.acc.read_req`  
> `ex_controller.io.acc.read_resp <> spad.module.io.acc.read_resp`  
> `ex_controller.io.acc.write <> spad.module.io.acc.write`

---

## 1. 先记住整体分工

- `ExecuteController`：决定算子从哪里读 A/B/D，结果写回哪里
- `spad.srams`：scratchpad bank 的读写口
- `spad.acc`：accumulator bank 的读写口

所以这几条连接的本质是：

**把 ExecuteController 的“执行期访存需求”，直接接到 Scratchpad/Accumulator 的物理端口上。**

---

## 2. `ex_controller.io.srams.read <> spad.module.io.srams.read`

这条连接表示：

**执行单元要从 scratchpad bank 读操作数时，读请求直接送到 spad 的 SRAM 读口。**

电路上它对应一组按 bank 分开的 `Decoupled` 读请求/响应通路：

- `req.valid / req.ready`：这一拍是否发起读
- `req.bits.addr`：读哪一行
- `resp.valid / resp.ready`：返回数据是否有效
- `resp.bits.data`：读出的整行数据

它不是“函数调用”，而是：

**ExecuteController 和 ScratchpadBank 之间的整组读总线对接。**

---

## 3. `ex_controller.io.srams.write <> spad.module.io.srams.write`

这条连接表示：

**执行阵列输出如果要写回 scratchpad，就直接驱动 spad 的 SRAM 写口。**

电路上这组信号包含：

- `valid / ready`
- `addr`
- `data`
- `mask`

也就是说，ExecuteController 不自己存数据，而是：

**给出“写哪一行、写什么数据、哪些 byte 生效”，真正落地由 spad bank 完成。**

---

## 4. `spad.module.io.acc.read_req <> ex_controller.io.acc.read_req`

这条连接表示：

**如果执行阶段要从 accumulator 取 A/B/D 或其他操作数，ExecuteController 会发起 acc 读请求。**

这里要注意方向：

- 语义上是 `ExecuteController` 产生请求
- `spad.acc` 接收请求

请求里不只是地址，还包括：

- `scale`
- `act`
- `full`
- `fromDMA`

说明 accumulator 读不是简单 SRAM 读，而是可能伴随后处理语义。

---

## 5. `ex_controller.io.acc.read_resp <> spad.module.io.acc.read_resp`

这条连接表示：

**accumulator 读出的结果，从 spad 返回给 ExecuteController。**

这里返回的不只是裸数据，还可能是：

- 缩放后的数据
- 不同位宽版本的数据

所以这条线本质上是：

**Accumulator -> ExecuteController 的读响应通路。**

---

## 6. `ex_controller.io.acc.write <> spad.module.io.acc.write`

这条连接表示：

**当阵列结果需要写入 accumulator 时，ExecuteController 直接向 acc bank 发写请求。**

这里的写请求包含：

- `addr`
- `data`
- `mask`
- `acc`（是否累加）

其中 `acc` 很关键，它说明 accumulator 写不是普通覆盖写，还可能是“读改写式累加”。

---

## 7. scratchpad 路径和 accumulator 路径的区别

### scratchpad (`srams`)

- 更像普通片上输入/中间缓冲区
- 读写接口更接近 SRAM bank

### accumulator (`acc`)

- 更像部分和/结果存储区
- 读写可能伴随 scale / activation / accumulate 语义

所以 `ExecuteController` 同时接这两类口，是因为：

**Gemmini 执行时既可能从 scratchpad 取数，也可能从 accumulator 取数；结果也可能写回 scratchpad，也可能写回 accumulator。**

---

## 8. 一句话收束

这 5 组连接的本质是：

**把 ExecuteController 的执行期读写需求，分别接到 scratchpad bank 和 accumulator bank 的真实物理端口上，从而让阵列计算可以直接和 Gemmini 的片上存储体系闭环。**
