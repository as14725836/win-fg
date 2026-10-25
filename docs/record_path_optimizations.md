# record() 路径优化实测（第 2/3/4 项）

本文件记录 `record_impl.inc` 与 `framegen.cpp::onResize/ensureScratch` 的三项逐帧
优化，以及用一个**离线 harness** 实测到的结果。

## 为什么用「API 调用计数 / 设备内存字节数」而不是帧时间

环境里只有 lavapipe（Mesa 的软件 Vulkan 1.4，LLVM 20.1.2）。它能验证「SPIR-V 能加载、
pipeline 能创建、descriptor 能绑、命令能录制」，但它的光栅/软算性能与 Adreno/Turnip 相差
两个数量级，用它跑出来的耗时**没有任何参考价值**，不能写进结论。

而这四项优化全部是 **emit 侧**的收益 —— 减少的是每帧发往驱动的 `vkCmd*` 调用数和
`vkAllocateMemory` 的字节数，与具体 GPU 无关，因此可以直接数出来：

- `record()` 只是录制命令缓冲，**从不提交**；把 `DeviceDispatch` 里的函数指针换成计数器
  再转发给真函数，就能得到精确调用次数，一个着色器都不会执行；
- 内存量从 `vkGetImageMemoryRequirements` 读取，本身也是纯查询。

这就是 `tests/record_apicount.cpp` 的做法（`-DWINFG_BUILD_TESTS=ON` 构建）。

```bash
cmake -S . -B build -DWINFG_BUILD_TESTS=ON -DVULKAN_INCLUDE=/usr/include
cmake --build build -j4
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./build/record_apicount 1920 1080 4 1
DUMP_TRANS=1 FRAMES=3 ./build/record_apicount 320 180 4 1   # 布局转换表 / 多帧
```

## 改动内容

| # | 改动 | 位置 |
|---|------|------|
| 2 | pipeline-bind 缓存：`disp()` 记住上次绑定的 `VkPipeline`，同名 Pipe 连发不再重复 bind；两处直连 `CmdBindPipeline`（`pGmReduce_`、`pFlowReg_`）后同步更新缓存 | `record_impl.inc` |
| 2 | 初始 layout transition 从约 27 次 `CmdPipelineBarrier` 合并成 1 次（批量 `VkImageMemoryBarrier` 数组）；stage/access 逐字对齐 `FrameGen::barrier()` 的 COMPUTE→COMPUTE + SHADER_WRITE→SHADER_READ\|WRITE | `record_impl.inc` |
| 3 | flow pyramid 只清最顶层：只有 `l = kLevels-1` 的 predictor 是它自己（`pred = l+1 < kLevels ? l+1 : l`），其余层本帧必被 `of3_flow` 完整覆写 | `record_impl.inc` |
| 4 | `flowLvl_` 按需分配：`l < flowFinest_` 的层不 `makeImage`（R16G16B16A16，1080p 下 level0 就有 16.6 MB）；同时省掉对应的 `flowUbo` 与 `sFlow` | `framegen.cpp` |

## 实测：每帧 API 调用数（320×180，model=4）

| 计数项 | 优化前 | 优化后 | 变化 |
|---|---|---|---|
| `CmdBindPipeline` | 27 | 7 | **−74%** |
| `CmdDispatch` | 27 | 27 | 不变（本就该如此） |
| `CmdBindDescriptorSets` | 27 | 27 | 不变 |
| `CmdPipelineBarrier` | 60 | 30 | **−50%** |
| 其中携带的 image barrier 结构体 | 59 | 55 | −4 |
| `CmdClearColorImage` | 5 | 1 | **−80%** |

`CmdDispatch`/`CmdBindDescriptorSets` 完全不变，说明**没有少做任何一步计算**，省下的纯粹是
冗余的驱动调用 —— 这是本次改动正确性的第一层证据。

三个 perf_preset（`flowFinest` = 1/2/3）与三段分辨率下 `CmdBindPipeline` 恒定落在 7，说明
缓存命中率与档位无关（档位只改变 pyramid 深度，不改变 Pipe 序列）。

## 实测：设备内存（`ALLOC bytes`，含所有内部 scratch）

| 分辨率 | preset | 优化前 | 优化后 | 节省 |
|---|---|---|---|---|
| 320×180 | 0 | 2.36 MB | 1.92 MB | −0.44 MB (−19%) |
| 320×180 | 1 | 2.14 MB | 1.59 MB | −0.55 MB (−26%) |
| 320×180 | 2 | 2.08 MB | 1.50 MB | −0.58 MB (−28%) |
| 1920×1080 | 0 | 84.40 MB | 68.58 MB | **−15.8 MB (−19%)** |
| 1920×1080 | 1 | 76.50 MB | 56.73 MB | **−19.8 MB (−26%)** |
| 1920×1080 | 2 | 74.51 MB | 53.74 MB | **−20.8 MB (−28%)** |

## 第二层证据：被省掉的图像确实「从未被使用」

`DUMP_TRANS=1` 会打印每张 `vkCreateImage` 出来的图在本帧收到过的 layout 转换。优化前：

```
img2  : <none>
img5  : <none>
```

`img2`/`img5` 正是 `flowLvl_[0]` 与 `flowLvl_[1]` —— **分配了、却从未 transition、也从未被
dispatch 读写**。优化后这两张图根本不再创建。这直接证明第 4 项删掉的是纯 VRAM 浪费，而不是
「碰巧没人发现的功能代码」。

其余每张图的转换序列在优化前后逐条一致（`0->1` 进入 GENERAL，或最顶层 `0->7 -> 1->1 -> 7->1`
走 clear 路径），说明合并 barrier 没有漏 transition、也没有给同一张图做两次 transition。

## 已知限制（诚实标注）

- 没有跑过真机（Adreno / Turnip），所以**帧时间收益未测**。上表是调用数与字节数，不是 ms。
- lavapipe 在这台 aarch64 机器上，一旦真正执行复杂计算着色器就会 SIGILL（LLVM JIT 问题；
  单个平凡 compute 用 `GALLIVM_PERF=nopt` 可以跑通，`of3_luma` 则需要同时设 `LP_NUM_THREADS=0`，
  而完整的 flow+synth 图无论如何都会崩）。因此「像素级正确性」未在本机验证，只有
  「命令流结构正确 + 无漏 transition」这一层证据。
- 环境没有 Khronos Validation Layer，非法用法不会主动报错，只能靠上述结构性比对。
