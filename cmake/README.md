# cmake/ —— 依赖查找模块与交叉编译工具链

> 这个目录**不含任何源文件**，只有两类东西：① 让 `find_package()` 认识本项目三方库的 `Find*.cmake`；② 交叉编译用的 toolchain 文件。
> 它通过顶层 `CMakeLists.txt` 的 `list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")` 生效 —— **不要删这一行**，否则所有 `find_package` 会退化成系统查找。

## 1. 六个文件一览

| 文件 | 实际调用方（`find_package` 位置） | 查找位置 | 导出的变量 | 缺失时 |
| --- | --- | --- | --- | --- |
| `FindFFMPEG.cmake` | `src/platform/capture`(:36) · `src/platform/decoder`(:17) · `src/platform/encoder`(:17) · `src/main`(:1) · `src/main/test`(:1) | **仅** `install/${TARGET_PLATFORM}/include` 与 `lib`（`NO_DEFAULT_PATH`，不认系统路径） | `FFMPEG_INCLUDE_DIRS` `FFMPEG_LIBRARIES` `FFMPEG_LIB_DIR` | `FATAL_ERROR` 并打印搜索路径 |
| `FindZLMediaKit.cmake` | `src/platform/rtsp`(:8) · `src/main`(:2) | 同上（`NO_DEFAULT_PATH`） | `ZLM_INCLUDE_DIRS` `ZLM_LIBRARIES` `ZLM_LIB_DIR` | `FATAL_ERROR`，提示期望 `libmk_api.so` |
| `FindMosquitto.cmake` | `src/comm`（`:29`，`comm_api` 目标） | 同上（`NO_DEFAULT_PATH`，找 `mosquitto/mosquitto.h` + `libmosquitto.so`） | `MOSQUITTO_INCLUDE_DIRS` `MOSQUITTO_LIBRARIES` `MOSQUITTO_LIB_DIR` | `FATAL_ERROR`，提示期望 `install/<平台>/lib/libmosquitto.so` |
| `FindMPP.cmake` | `src/platform/decoder`(:23) · `src/platform/encoder`(:23) · `src/common`(:59)（均在条件分支内） | `install/<平台>/{include,lib}` → `/usr/local` → `/usr` → `/usr/lib/aarch64-linux-gnu`（顺序回退） | `MPP_INCLUDE_DIRS` `MPP_LIBRARIES` | `find_package_handle_standard_args` 报 not found |
| `FindRGA.cmake` | 预留（当前**无任何引用**） | 同 MPP | `RGA_INCLUDE_DIRS` `RGA_LIBRARIES` | 同上 |
| `rk3588.toolchain.cmake` | 不自动使用，需 `-DCMAKE_TOOLCHAIN_FILE=` | 只设置编译器路径 | —（设置 `CMAKE_SYSTEM_*` 与编译器） | 路径写死 `/usr/aarch64-linux-gnu`，需按实际改 |

> 唯一**不**走本目录的依赖是 `Freetype`（`src/common/CMakeLists.txt:45`，`BUILD_FREETYPE_OSD=ON` 时才找）与 `Threads`：它们用系统自带的 Find 模块。

## 2. 两种查找策略（差异很重要）

| | FFMPEG / ZLMediaKit / Mosquitto | MPP / RGA |
| --- | --- | --- |
| 是否看系统库 | **否**（`NO_DEFAULT_PATH`） | 是（`PATHS` 只是优先提示） |
| 理由 | 系统自带的 ffmpeg 版本/ABI 与板端 `install/rk3588` 不一致，混用会在运行期炸 | RK 的 `rockchip_mpp` / `rga` 通常由系统或 SDK 安装，允许回退 |
| 后果 | `install/<平台>/lib` 缺失 → **configure 阶段就失败**（早失败优于运行期段错误） | 系统里有同名库就会静默用系统的（版本不符时行为异常） |

## 3. `TARGET_PLATFORM` 从哪来

`FindFFMPEG.cmake` / `FindZLMediaKit.cmake` 开头强制要求 `TARGET_PLATFORM` 已定义，否则直接 `FATAL_ERROR`。它由顶层 `CMakeLists.txt` 自动判定：

```text
指定了 CMAKE_TOOLCHAIN_FILE            → rk3588
未指定，且 uname -m == x86_64          → x86_64
其他（aarch64 板端本机编译）            → rk3588
```

所以板端本机编译**不需要**传 toolchain，`install/rk3588` 会被自动选中。

## 4. 怎么覆盖搜索路径（不改文件）

现有五个 `Find*.cmake`（FFMPEG / ZLMediaKit / Mosquitto / MPP / RGA）都**没有**读取 `FFMPEG_ROOT` / `ZLM_ROOT` 之类的缓存变量（`FFMPEG_ROOT` 被函数内部 `set()` 固定为 `install/${TARGET_PLATFORM}`）。因此覆盖方式只有两种：

```bash
# 方式 A（推荐）：让 install/<平台> 指向你的实际目录
ln -s /opt/rk3588_sdk/ffmpeg install/rk3588      # 目录级替换

# 方式 B：改 TARGET_PLATFORM 间接切目录（同时影响三套依赖）
cmake -S . -B out -DTARGET_PLATFORM=rk3588_v2 && cmake --build out -j8
```

`MPP` / `RGA` 额外支持把库装到 `/usr/local` 或 `/usr`（它们会回退）。

## 5. 交叉编译用法

```bash
# 需先按实际路径改 toolchain 里的 TOOLCHAIN_ROOT（当前 /usr/aarch64-linux-gnu）
cmake -S . -B out-x86 -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588.toolchain.cmake
cmake --build out-x86 -j8
```

注意：`CMAKE_FIND_ROOT_PATH_MODE_LIBRARY/INCLUDE = ONLY` 会让 `find_*` **只在工具链根下找**，所以交叉编译时 `install/<平台>` 也必须落在能找到的位置；实际项目中该文件尚未用于产出可用二进制（路线图见 [../docs/design/ROADMAP.md](../docs/design/ROADMAP.md) 的交叉编译条目）。

## 6. 新增一个依赖的标准写法

```cmake
# cmake/FindXXX.cmake
if(NOT DEFINED TARGET_PLATFORM)
    message(FATAL_ERROR "TARGET_PLATFORM not defined!")   # 与现有两个模块保持一致
endif()
set(XXX_ROOT "${PROJECT_SOURCE_DIR}/install/${TARGET_PLATFORM}")
find_path(XXX_INCLUDE_DIR NAMES xxx/xxx.h HINTS ${XXX_ROOT}/include NO_DEFAULT_PATH)
find_library(XXX_LIBRARY NAMES xxx HINTS ${XXX_ROOT}/lib NO_DEFAULT_PATH)
if(NOT XXX_INCLUDE_DIR OR NOT XXX_LIBRARY)
    message(FATAL_ERROR "XXX for platform [${TARGET_PLATFORM}] NOT FOUND!\nSearch path: ${XXX_ROOT}")
endif()
set(XXX_INCLUDE_DIRS ${XXX_ROOT}/include)
set(XXX_LIBRARIES ${XXX_LIBRARY})
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(XXX DEFAULT_MSG XXX_INCLUDE_DIRS XXX_LIBRARIES)
mark_as_advanced(XXX_INCLUDE_DIRS XXX_LIBRARIES)
```

三条约定：① 变量名用 `XXX_INCLUDE_DIRS` / `XXX_LIBRARIES`（复数，与调用方一致）；② 缺失要 `FATAL_ERROR` 且**打印搜索路径**（否则只能靠猜）；③ `mark_as_advanced` 防止 CMake GUI 里被误改。
