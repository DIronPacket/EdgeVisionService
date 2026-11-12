# ============================================================================
# EdgeVisionService · 顶层 Makefile（薄包装，不是第二套构建系统）
# ----------------------------------------------------------------------------
# 定位：CMake 的"短命令前端"。这里不列源文件、不写头文件依赖、不写编译规则，
#       真正的规则仍由 CMake 生成在 $(BUILD)/Makefile 里。
#       所以不存在"Makefile 与 CMakeLists 两套规则打架"的问题。
#
# 它替你省掉的重复劳动：
#   1. 首次构建自动 configure（含 compile_commands.json，供 VSCode / Cline 索引）
#   2. run / run-node 直接起程序，不依赖 $(BUILD)/run.sh（该脚本不入库，见 HANDOFF §1.5）
#   3. clean 只清编译产物，保留 $(BUILD)/ 下手写的 run.sh（盲目 rm -rf $(BUILD) 会把它删掉）
#
# 常用：
#   make                 # = cmake -S . -B $(BUILD) && cmake --build $(BUILD) -j$(nproc)
#   make run             # 起 EdgeVisionService（冒烟）
#   make run-node        # 起真实主链路 testnode
#   make test            # 依次跑 scripts/tests/run_t*.sh
#   make clean           # 清编译产物（保留 run.sh）
#   make help            # 看全部目标
#
# 可覆盖变量（写在命令后面）：
#   make JOBS=4
#   make BUILD=build-dbg CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=Debug"
#   make CMAKE_FLAGS="-DBUILD_FREETYPE_OSD=ON" reconfigure
#   make run-node CFG=/abs/path/node.json
#   make distclean FORCE=1        # ⚠ 会连 $(BUILD)/run.sh 一起删
# ============================================================================

BUILD       ?= out
JOBS        ?= $(shell nproc 2>/dev/null || echo 4)
CMAKE       ?= cmake
CMAKE_FLAGS ?=
PLATFORM    ?= rk3588
APP         ?= EdgeVisionService
NODE        ?= testnode
CFG         ?= ../config/node.json
ARGS        ?=
FORCE       ?= 0

# $(BUILD)/run.sh 干的两件事：cwd 切到 $(BUILD)/、导出 $(BUILD)/lib 与 install/<平台>/lib
RUN_ENV = LD_LIBRARY_PATH="$(abspath $(BUILD)/lib):$(abspath install/$(PLATFORM)/lib):$$LD_LIBRARY_PATH"

.DEFAULT_GOAL := all
.PHONY: all build configure reconfigure rebuild clean distclean \
        run run-app run-node run-sh test test-t5 info help

# ------------------------------------------------------------------ 构建
all: build

build:
	@test -f $(BUILD)/Makefile || $(MAKE) --no-print-directory configure
	$(CMAKE) --build $(BUILD) -j$(JOBS)

# 首次生成构建系统；改过 CMakeLists / 换了 CMAKE_FLAGS 后重跑（-S -B 幂等）
configure:
	$(CMAKE) -S . -B $(BUILD) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $(CMAKE_FLAGS)

reconfigure: configure

# 两步刻意串行：先清再编，避免 -j 下 clean 与 build 交错
rebuild:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory build

# ------------------------------------------------------------------ 清理
clean:
	@if [ -f $(BUILD)/Makefile ]; then \
		$(CMAKE) --build $(BUILD) --target clean; \
		echo "clean: 已删除 $(BUILD)/ 下编译产物（保留 run.sh 等手工文件）"; \
	else \
		echo "clean: $(BUILD)/ 没有 CMake 缓存，跳过"; \
	fi

# 会删掉整个 $(BUILD)/，其中 run.sh 不入库；因此要求显式 FORCE=1
distclean:
	@if [ "$(FORCE)" != "1" ]; then \
		echo "拒绝执行：distclean 会 rm -rf $(BUILD)/，里面有："; \
		for f in $(BUILD)/run.sh; do \
			[ -e "$$f" ] && echo "    $$f   （重建方法见 docs/HANDOFF.md §1.5）"; \
		done; \
		echo "确认要删就执行：make distclean FORCE=1"; \
		exit 1; \
	fi
	@rm -rf $(BUILD)
	@echo "distclean: 已删除 $(BUILD)/（run.sh 需按 docs/HANDOFF.md §1.5 重建）"

# ------------------------------------------------------------------ 运行
# 等价 $(BUILD)/run.sh：切到 $(BUILD)/、导出两个库路径、起 bin/<name>；
# 关键差别：不依赖那个不入库的脚本，删掉 $(BUILD)/ 后照样能跑
run run-app: build
	@cd $(BUILD) && $(RUN_ENV) ./bin/$(APP) $(ARGS)

run-node: build
	@test -x $(BUILD)/bin/$(NODE) || { \
		echo "run-node: $(BUILD)/bin/$(NODE) 不存在 —— 该可执行当前未参与构建。"; \
		echo "          原因：src/main/CMakeLists.txt 里 add_subdirectory(test) 处于注释状态（当前有意关闭）。"; \
		echo "          需要时取消注释后执行 make reconfigure && make。"; \
		exit 1; }; \
	cd $(BUILD) && $(RUN_ENV) ./bin/$(NODE) $(CFG)

# 老用法兼容：$(BUILD)/run.sh 存在就走它（该脚本重建方法见 HANDOFF §1.5）
run-sh:
	@if [ -x $(BUILD)/run.sh ]; then \
		echo "run-sh: 走 $(BUILD)/run.sh"; \
		$(BUILD)/run.sh $(APP) $(ARGS); \
	else \
		echo "run-sh: $(BUILD)/run.sh 不存在（不入库）；请用 make run / make run-node"; \
		exit 1; \
	fi

# ------------------------------------------------------------------ 测试
# scripts/tests 下的 T1/T2/T3/T5 验收脚本逐个跑，任一失败即停
test: build
	@set -e; for s in scripts/tests/run_t*.sh; do \
		echo "=== $$s ==="; \
		bash "$$s"; \
	done

test-t5: build
	@./scripts/tests/run_t5_builder.sh config/node.json

# ------------------------------------------------------------------ 信息
info:
	@echo "BUILD       = $(BUILD)$(if $(wildcard $(BUILD)/Makefile),（已 configure）,（未 configure，make 会自动生成）)"
	@echo "JOBS        = $(JOBS)"
	@echo "PLATFORM    = $(PLATFORM)"
	@echo "CMAKE_FLAGS = $(CMAKE_FLAGS)"
	@echo "APP / NODE  = $(APP) / $(NODE)"
	@echo "CFG         = $(CFG)"
	@echo -n "CMAKE       = "; $(CMAKE) --version 2>/dev/null | head -1 || echo "未安装"
	@echo -n "CXX         = "; $(CXX) --version 2>/dev/null | head -1 || echo "未安装"

help:
	@echo "EdgeVisionService · 顶层 Makefile（薄包装，规则归 CMake）"
	@echo ""
	@echo "  构建"
	@echo "    make / make all      configure（首次，自动）+ cmake --build -j JOBS"
	@echo "    make configure       只生成构建系统（含 compile_commands.json）"
	@echo "    make reconfigure     同上（改过 CMakeLists / CMAKE_FLAGS 后用）"
	@echo "    make rebuild         先 clean 再 build"
	@echo "    make clean           清编译产物（保留 $(BUILD)/run.sh）"
	@echo "    make distclean       删整个 $(BUILD)/（必须 FORCE=1）"
	@echo ""
	@echo "  运行（不依赖 $(BUILD)/run.sh）"
	@echo "    make run             $(APP)"
	@echo "    make run ARGS=...    带参数"
	@echo "    make run-node        $(NODE) ../config/node.json"
	@echo "    make run-node CFG=/abs/path/node.json"
	@echo "    make run-sh          走老脚本 $(BUILD)/run.sh（存在时）"
	@echo ""
	@echo "  测试 / 信息"
	@echo "    make test            依次跑 scripts/tests/run_t*.sh"
	@echo "    make test-t5         只跑 T5 builder 验收"
	@echo "    make info            打印构建参数与 configure 状态"
