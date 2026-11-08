# src/common/preprocess —— 预处理模块（框架）

> 只写"这层能做什么、怎么调、哪里还没实现"。架构关系见 [docs/ARCHITECTURE.md](../../../docs/ARCHITECTURE.md)，
> 红线规定见 [docs/HANDOFF.md §2](../../../docs/HANDOFF.md)。

## 1. 能力边界

| 能力 | 入口 | 状态 |
| --- | --- | --- |
| 像素格式互转（NV12 / YUV422P / YUV420P / BGR888） | `FrameConverter` | 校验+分派+同格式深拷贝已实现；7 个转换对是 TODO（返回 `ERR_NOT_IMPL`） |
| 画框（空心/实心）、贴图（MASK8 / RGBA / BGRA + alpha） | `FrameDrawer` | `locate_pixel` / `rgb_to_yuv` 已实现；像素写入是 TODO |
| 截图（帧深拷贝 + 24bit BMP 落盘） | `FrameDrawer::snapshot` / `save_snapshot` | TODO |
| OSD 字符叠加（UTF-8、换行、测量、底条） | `OsdRenderer` | 上层逻辑已实现；底下的 `blit` 是 TODO |
| 内置点阵字体（零第三方依赖） | `BitmapGlyphSource` | 已实现（`.evsfont` 解析/取字形） |
| FreeType 矢量字体（可选） | `FreeTypeGlyphSource` | 编译开关 `BUILD_FREETYPE_OSD`，默认 OFF；实现是 TODO |
| 门面：一帧进 → 转码+叠加 → 一帧出 | `ImagePreprocessor::process` | 流程已实现；依赖上面各层补齐 |

**当前是框架**：结构、校验、分派、错误码、UTF-8 解码、字体解析都通了；像素级循环留空，
统一返回 `pre::PreStatus::ERR_NOT_IMPL`，用 `grep -rn ERR_NOT_IMPL src/common/preprocess/` 可列出全部待填点。

## 2. 目录与文件

```text
preprocess/
├── preprocess_define.h        # 返回码 / 颜色 / Rect / ImageLayout / 叠加样式 / 贴图参数
├── frame_converter.{h,cpp}    # 格式互转（转换矩阵 + 逐对函数）
├── frame_drawer.{h,cpp}       # 画框 / 贴图 / 截图（含 YUV 色度定位）
├── glyph_source.{h,cpp}       # IGlyphSource 抽象 + 内置点阵字库解析
├── freetype_glyph_source.{h,cpp} # FreeType 实现（EVS_ENABLE_FREETYPE 宏隔离）
├── osd_renderer.{h,cpp}       # 字符叠加门面
└── image_preprocessor.{h,cpp} # 对外唯一门面：process(src, dst)
```

## 3. 三条使用约定

1. **不分配帧内存**：`dst` 的 buffer 由调用方准备（内存池另一块 / 节点自有缓冲），
   本模块只往已有内存写。布局用 `ImagePreprocessor::calc_dst_layout()` 先算。
2. **失败分支必须归还内存块**：节点里 `getBlock()` 之后任何 `process()` 失败都要 `release`（HANDOFF §2.4-1）。
3. **非线程安全**：一个实例只在一个线程里用；需要多线程就每线程各建一份。

## 4. 最小用法

```cpp
#include "image_preprocessor.h"

pre::PreprocessConfig cfg;
cfg.dst_fmt = PixelFormat::FMT_NV12;   // 不转码就留 FMT_UNKNOWN
cfg.align   = 16;
cfg.font_path = "/data/font16.evsfont"; // 点阵字体；用 ttf 时置 use_freetype 并开 BUILD_FREETYPE_OSD

pre::ImagePreprocessor proc;
if (proc.open(cfg) != pre::PreStatus::OK) { /* 记日志并失败退出，别硬跑 */ }

// 每帧：
//   pre::MemBlock *blk = m_queue.getBlock(100);
//   FrameBuffer *src = ..., *dst = ...;          // dst 的格式/尺寸按 calc_dst_layout 准备
//   if (proc.process(*src, *dst) != pre::PreStatus::OK) { m_queue.release(blk); continue; }
```

## 5. 建议的实现顺序

1. `FrameConverter`：先 NV12 ↔ YUV422P（本项目主需求），再 NV12 ↔ YUV420P，最后 BGR888 两个方向。
2. `FrameDrawer::fill_span` / `blend_pixel`（`locate_pixel` 已给好色度定位）。
3. `FrameDrawer::draw_box` → `blit` → `snapshot` → `save_snapshot`。
4. `FreeTypeGlyphSource`（等 `install/` 里备好 freetype，再 `cmake -DBUILD_FREETYPE_OSD=ON`）。

## 6. 已验证 / 未验证

- 已验证（`cmake --build build -j8` + 临时测试程序）：编译零告警、支持矩阵、布局计算、
  同格式深拷贝、EVSF 字库解析、文本测量、`open()` 配置校验、未实现分支返回 `ERR_NOT_IMPL`。
- 未验证：任何像素级结果（因为实现为空），以及接真实采集帧的效果。

## 7. 两个已知坑（改实现前必读）

1. `PixelFormat::FMT_YUV422` 的实际布局是 **YUV422P（三平面）**，不是打包的 YUYV/UYVY。
2. `FrameBuffer::BindDataPointers()` 给 `FMT_YUV422` 算 `data[2]` 偏移时用的是 YUV420P 的公式
   （`stride[1]*(height/2)`），比真实 U 平面小一半 —— 接真实 YUV422 数据前必须先确认上游布局。
   本模块内部的 `FrameConverter::calc_layout` 按 YUV422P 正确布局计算。
