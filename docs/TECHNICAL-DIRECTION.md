# 为什么选择静态重编译：与 d2r-mobile 的路线比较

这个项目受 [d2r-mobile](https://github.com/d2r-mobile/d2r-mobile) 启发。它的做法是让游戏原本的 AArch64 代码继续在 ARM 设备上执行，只改写那些无法直接运行的部分，再补上系统接口和图形后端。我们没有沿用这套执行模型，原因和目标平台有关：项目需要同时覆盖 x86-64 的 Windows、Steam Deck 和 ARM64 的 Android、macOS。把 AArch64 代码转换成 C++，再交给各平台编译器，是目前能统一处理这两种指令集的办法。

“更难”在这里说的是我们为这个目标多做了重编译器、运行时和多平台验证这几块工作。它不是《猎天使魔女 3》比《暗黑破坏神 II：重制版》更难移植的结论，也不是两个项目开发成本或性能的受控比较。两款游戏的负载、覆盖场景和完成度都不同，帧率数字不能直接排名。

## d2r-mobile 的路线

以下结论来自阅读源码，对照版本固定在提交 [`3e2d247d8c613d6fdf7362df8a1feb6b1329115a`](https://github.com/d2r-mobile/d2r-mobile/commit/3e2d247d8c613d6fdf7362df8a1feb6b1329115a)。这次研究没有运行它的游戏，也没有在它的设备上重新跑性能测试。

- [CPU 部分](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/dm/README.md#L18-L38)：绝大多数 AArch64 指令保持原样，离线只改写 `x18`、TLS、`svc` 和计数器读取，让它们跳到宿主代码。[gate 汇编](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/dm/host/entry.S#L38-L84)保存 AArch64 寄存器后调用宿主分派。这是二进制改写加兼容层，不是生成 C++。
- [Android 构建](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/dm/android/CMakeLists.txt#L88-L117)：包含 Android 前端、Vulkan present 和 `nvn_vk.c`，链接 `dm_vulkan_engine`、AAudio 等。它的 [Android 文档](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/docs/ANDROID.md#L7-L27)已经有 ARM64／Vulkan 实机记录，另有 [HarmonyOS 说明](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/docs/HARMONYOS.md#L1-L17)。所以不能说这个项目只支持苹果生态。它的 Windows/Linux 工具用于制作导入包，不代表能在 x86-64 上直接运行游戏。
- [Metal 后端](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/dm/gfx/nvn/nvn_mtl.m#L329-L339)会检查统一内存，[宿主构建](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/dm/host/Makefile#L122-L126)链接 Apple frameworks。这个后端搬不到别的系统，但整个项目不止这一个后端。
- [着色器](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/dm/gfx/README.md#L30-L57)：用 Ryujinx shader library 译到 SPIR-V，再经 SPIRV-Cross 转 Metal；[Android 那条路](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/dm/android/README.md#L157-L193)另外处理 Vulkan binding、验证和预热。CPU 代码能直接执行，不代表 GPU 代码也能直接执行。
- [冷缓存卡顿的记录](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/docs/android-notes/2026-10-22-first-play-stutter.md#L9-L53)提到运行中创建 pipeline 造成的卡顿、预热能改善多少，以及一个因物体延迟出现而被否掉的跳过绘制实验。这是对方作者的测量，不是我们对其设备的新测试。
- [Rift 模式](https://github.com/d2r-mobile/d2r-mobile/blob/3e2d247d8c613d6fdf7362df8a1feb6b1329115a/dm/host/dm_rift.c#L16-L29)用 tick hook、状态修改和调用原函数增加了新玩法。所以“二进制改写做不了定制”也不成立。

## 两种原生运行的区别

| | d2r-mobile | 本项目 |
| --- | --- | --- |
| 游戏 CPU 代码 | 在 ARM64 宿主上执行改写后的 AArch64 代码 | AArch64 生成 C++，由宿主编译器产出机器码；x86-64 和 ARM64 共用一套重编译框架 |
| 系统接口 | 装载、线程、内存、系统服务的兼容实现 | 同样需要 Horizon 服务、客体上下文、调用分派、内存与装载 |
| 图形 | Metal 和 Android Vulkan 两套后端 | NVN 转 Vulkan；macOS 走 Vulkan loader 和 MoltenVK |
| 可移植性的主要代价 | 新宿主得满足原指令集的执行、ABI 和内存约束；换到 x86-64 还要再加一层翻译 | 指令翻译语义、生成代码、宿主编译和多平台运行时都要自己做并验证 |

两边都是原生运行。我们的原生程序里一样有兼容层，静态重编译并不会让系统服务和图形 API 的差异消失。

[指令生成器](../recomp/emit.py#L1)把每个客体函数变成 `void sub_XXXXXXXX(nx::CPU& c)`：寄存器放在上下文里，函数内分支变成 `goto`，跳转表变成 `switch`，已知目标直接调用，未知目标走分派，没实现的指令进显式陷阱。[CPU 上下文](../runtime/include/nx/cpu.h#L46)保存通用寄存器、向量寄存器、标志位和独占访问状态。这些都是跨指令集之后落到我们头上的工作。

## 多出来的部分具体在哪

### 1. 换的不只是窗口后端，还有 CPU 指令集

ARM64 能直接跑的 AArch64 指令，x86-64 跑不了。想沿用同指令集的二进制改写，又要覆盖 Windows x86-64 和 Steam Deck，就得再加静态或动态翻译层，或者维护第二套 CPU 执行路径。

我们把这份成本集中在重编译器上。[原子指令生成](../recomp/emit_ext.py#L16)和 [CPU 辅助函数](../runtime/include/nx/cpu.h#L121)要保证原子性、内存顺序和独占访问语义；加密指令在 ARM 上有[硬件路径和软件实现的分支](../runtime/include/nx/cpu.h#L497)。换来的是源码层面的可移植性，代价是语义验证和编译时间。它不能让同一台 ARM 设备比直接执行客体指令更快。

### 2. 共用 Vulkan 之后，GPU 和驱动仍然各管各的

共用后端能复用代码，但改不了驱动行为和开销差异。

- [RADV 的 ASTC 路径](../runtime/src/nvn/nvn_vk.cpp#L181)会打开驱动的 GPU 解码；别的设备要么确认原生支持，要么走 [CPU ASTC 解码](../runtime/src/nvn/nvn_astc.h#L1)。格式相同不等于上传成本相同。
- [分块提交](../runtime/src/nvn/nvn_frames.cpp#L684)在 Apple 分支上记了一个反例：原先判断 MoltenVK 每次提交都很贵、分块没意义，实际测下来早一点把命令交给 GPU 能减少空等。别的平台的结论不能照搬。
- [间接绘制](../runtime/src/nvn/nvn_indirect.cpp#L245)得先判断是否真的需要复制和屏障，再决定要不要中断 render pass。序章里大量单图元间接绘制会把不必要的 pass 开关成本放大。

这些都是我们实际踩到的问题，不能反过来推断 d2r-mobile 没遇到过类似情况。具体帧率和测量条件见[性能说明](PERFORMANCE.md)。

### 3. 换了游戏，NVN 和着色器语义得重新验证

已有的 NVN 实现能提供方法和代码，但不构成对新游戏行为的兼容证明。曝光、HDR、间接绘制、着色器资源布局和过场路径，在《猎天使魔女 3》上都要单独验证。

[无符号浮点输出修补](../runtime/src/nvn/nvn_spirv_patch.cpp#L622)是一个具体例子：对写入特定渲染目标的片元输出处理 NaN、负数和溢出，避免错误值流进后面的 HDR pass。源码注释里直接写了参考 d2r-mobile Metal 层对 NaN 的处理，可见没有共用执行运行时，不等于没有借鉴。

另外，采用原生指令执行路线的其他《猎天使魔女 3》移植也遇到过无附件曝光绘制的问题。这说明游戏语义是共同的难点，不是静态重编译独有的。

### 4. 调性能不能牺牲 CPU／GPU 同步的正确性

[按命令位置处理 fence](../runtime/src/nvn/nvn_replay.cpp#L76)和提交流水线是为了让客体执行与图形录制重叠，同时还得保护 GPU 正在读的数据和写回顺序。单纯减少等待、或者提前返回成功，只是把性能问题换成画面问题或竞争问题。

静态重编译还能加一点可审查的、绑定版本的适配。[B3 hooks](../recomp/b3_hooks.py#L20)同时校验模块、输入 SHA-256 和入口地址，原始函数保留为回退。这确实方便调试和定制，但每个快路径都要单独证明等价性；二进制改写路线同样能挂 hook，定制能力不是静态重编译独有的。

### 5. 四个平台就是四套宿主集成

[构建目标](../CMakeLists.txt#L451)里能直接看到差别：Android 用 NativeActivity、AAudio 和 Media NDK；macOS 用 SDL3 和 MoltenVK；Linux 用 SDL3、线程和 Vulkan loader；Windows 用 XInput、XAudio2 和 Media Foundation。文件路径、线程、内存映射、视频解码和打包也要各写一遍。

这也是为什么“能编译”和“能正常游玩”要分开验收，也是[游玩指南](PLAYING.md)存在的原因。单元测试通过不等于全平台完成。

## 现在为什么不换路线

已经定下的方向是：用一套能同时面向 x86-64 和 ARM64 的 C++ 静态重编译框架。

从代码现状看，CPU 上下文、函数分派、NVN 接口和平台实现都围绕这套约定集成在一起。整体换成另一套执行运行时，要重接调用边界、装载、线程、地址空间和回归测试，而且依然解决不了 x86-64 上的执行问题，也消不掉《猎天使魔女 3》的图形和资源适配。对现在这个目标来说，继续完善已有路线比为了“原生”这个词换架构更划算。这是读代码得出的判断，不是两种路线做过对比实验的结论。

如果哪天目标收窄到只做 ARM64，直接执行并改写原始指令值得重新评估，它省下的通用指令翻译是实打实的。d2r-mobile 的后端语义、诊断方法和平台经验仍然值得学，按许可证保留归属。跨平台性能和更多自定义选项是这个项目的方向，但目前不能声称在每个设备上都优于别的实现。
