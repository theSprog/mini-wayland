## mini-wayland -- pure Makefile build (non-recursive)
##
##   make                     BUILD=debug, static lib + all demos
##   make BUILD=release
##   make SANITIZE=1          ASan + UBSan
##   make WERROR=1            warnings become errors (run before committing)
##   make check-headers       compile every .hpp on its own
##   make tidy                clang-tidy (skipped if not installed)
##   make cppcheck
##   make check               all of the above
##   make compile_commands.json
##   make V=1                 echo full command lines
##
## 设计说明：
##  - 非递归。子目录不写 Makefile，源文件靠 wildcard 收集。
##  - 依赖靠 -MMD -MP 自动生成。
##  - demos/ 下每个子目录 = 一个可执行文件，加目录即可，不用改 Makefile。

PROJECT    := mini-wayland
BUILD      ?= debug
V          ?= 0
WERROR     ?= 0
SANITIZE   ?= 0
EXCEPTIONS ?= 0

# 构建变体各自独立的输出目录。
# 否则 `make SANITIZE=1` 之后再 `make` 会把插桩过的 .o 和没插桩的混在一起链接，
# 报出来的错误（relocation against __asan_option_*）完全看不出根因。
O          := build/$(BUILD)$(if $(filter 1,$(SANITIZE)),-asan)$(if $(filter 1,$(EXCEPTIONS)),-exc)

CXX        ?= g++
AR         ?= ar
PKG_CONFIG ?= pkg-config
CLANG_TIDY ?= clang-tidy
CPPCHECK   ?= cppcheck

# ---------------------------------------------------------------------------
# 告警
# ---------------------------------------------------------------------------
# 这个项目里相当一部分代码是 AI 写的，幻觉主要表现为"看起来对的类型转换"和
# "忘了初始化的成员"。所以告警开到近乎苛刻，默认全开而不是按需加。
#
# 故意**不加**的两个：
#   -Wpedantic  : TRY() 用了 GNU statement-expression，会对每一处 TRY 报错
#   -Wpadded    : KMS 结构体天然有洞，噪音无意义

WARN := -Wall -Wextra                       \
        -Weffc++                            \
        -Wconversion                        \
        -Wsign-conversion                   \
        -Wsign-compare                      \
        -Wold-style-cast                    \
        -Wzero-as-null-pointer-constant     \
        -Wshadow                            \
        -Wnon-virtual-dtor                  \
        -Woverloaded-virtual                \
        -Wcast-qual                         \
        -Wcast-align                        \
        -Wdouble-promotion                  \
        -Wformat=2                          \
        -Wundef                             \
        -Wmissing-declarations              \
        -Wredundant-decls                   \
        -Wno-unused-parameter

# 这几类没有"先记着以后改"的余地，直接升级为错误
WARN += -Werror=return-type                 \
        -Werror=uninitialized               \
        -Werror=return-local-addr           \
        -Werror=unused-result               \
        -Werror=suggest-override            \
        -Werror=vla                         \
        -Werror=implicit-fallthrough

ifeq ($(WERROR),1)
  WARN += -Werror
endif

CXXSTD   := -std=c++17

ifeq ($(EXCEPTIONS),1)
  EXCFLAGS :=
else
  # 错误一律走 expected，不用异常。手滑 throw 会直接编译失败。
  EXCFLAGS := -fno-exceptions
endif

ifeq ($(BUILD),debug)
  OPT := -O0 -g3 -fno-inline -DMW_DEBUG=1
else ifeq ($(BUILD),release)
  OPT := -O2 -g -DNDEBUG
else
  $(error BUILD must be 'debug' or 'release', got '$(BUILD)')
endif

# 栈回溯要看得懂，两个构建都保留帧指针
OPT += -fno-omit-frame-pointer

ifeq ($(SANITIZE),1)
  SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all
endif

# ---------------------------------------------------------------------------
# 外部依赖
# ---------------------------------------------------------------------------
# Step 1 只要 libdrm。Step 2 起往 PKGS 里加 gbm / egl / glesv2。
#
# **必须用 -isystem 而不是 -I**：libdrm 的 fourcc_code() / fourcc_mod_code()
# 宏体里是 C 风格强转，用 -I 引入会被 -Wold-style-cast 打中，
# 而那不是我们能改的代码。-isystem 让 GCC 忽略系统头里展开的宏产生的告警。

PKGS       := libdrm
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null | sed 's/-I/-isystem /g')
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs   $(PKGS) 2>/dev/null)

INCLUDES   := -Iinclude

CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(EXCFLAGS) $(SANFLAGS) \
            $(INCLUDES) $(PKG_CFLAGS)                        \
            -ffunction-sections -fdata-sections -MMD -MP

# -rdynamic: panic 打栈需要导出动态符号表，否则全是 (no symbol)
LDFLAGS  := $(SANFLAGS) -rdynamic -Wl,--gc-sections
LDLIBS   := $(PKG_LIBS) -ldl

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------

LIB_SRCS   := $(wildcard src/*/*.cpp) $(wildcard src/*.cpp)
LIB_OBJS   := $(LIB_SRCS:%.cpp=$(O)/%.o)
LIB        := $(O)/lib$(PROJECT).a

# check-headers 只查我们自己写的，不查 include/internal（那是既有库）
HEADERS    := $(wildcard include/mw/*/*.hpp) $(wildcard include/mw/*.hpp)

DEMO_DIRS  := $(sort $(dir $(wildcard demos/*/)))
DEMO_NAMES := $(patsubst demos/%/,%,$(DEMO_DIRS))
DEMO_BINS  := $(addprefix $(O)/bin/,$(DEMO_NAMES))

ALL_SRCS   := $(LIB_SRCS) $(wildcard demos/*/*.cpp)
DEPS       := $(ALL_SRCS:%.cpp=$(O)/%.d)

ifeq ($(V),1)
  Q :=
  say = @true
else
  Q := @
  say = @printf '  %-8s %s\n' $(1) $(2)
endif

# ---------------------------------------------------------------------------

.PHONY: all lib demos clean distclean check check-headers check-deps tidy cppcheck help

all: check-deps lib demos

lib: $(LIB)

demos: $(DEMO_BINS)

$(LIB): $(LIB_OBJS)
	$(call say,AR,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $(LIB_OBJS)

$(O)/%.o: %.cpp
	$(call say,CXX,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

define DEMO_RULE
$(O)/bin/$(1): $$(patsubst %.cpp,$(O)/%.o,$$(wildcard demos/$(1)/*.cpp)) $(LIB)
	$$(call say,LINK,$$@)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CXX) $$(LDFLAGS) -o $$@ $$^ $$(LDLIBS)
endef
$(foreach d,$(DEMO_NAMES),$(eval $(call DEMO_RULE,$(d))))

# ---------------------------------------------------------------------------
# 头文件自洽性
# ---------------------------------------------------------------------------
# 每个 .hpp 生成一个只 include 它的 TU 单独编译。
# 缺 include、忘了前向声明、宏污染，全在这里暴露。

check-headers:
	$(Q)mkdir -p $(O)/hdrchk
	$(Q)fail=0; \
	for h in $(HEADERS); do \
		printf '  %-8s %s\n' 'HDRCHK' "$$h"; \
		echo "#include \"$${h#include/}\"" > $(O)/hdrchk/tu.cpp; \
		$(CXX) $(CXXSTD) $(WARN) $(EXCFLAGS) $(INCLUDES) $(PKG_CFLAGS) \
			-fsyntax-only $(O)/hdrchk/tu.cpp || fail=1; \
	done; \
	exit $$fail

check-deps:
	$(Q)$(PKG_CONFIG) --exists $(PKGS) || { \
		echo "error: missing build dependency: $(PKGS)"; \
		echo "  Kylin / Ubuntu / Debian:  apt install libdrm-dev"; \
		echo "  or set PKG_CONFIG_PATH if libdrm lives outside the default prefix"; \
		exit 1; \
	}

# ---------------------------------------------------------------------------
# 静态分析
# ---------------------------------------------------------------------------

tidy: compile_commands.json
	$(Q)if ! command -v $(CLANG_TIDY) >/dev/null 2>&1; then \
		echo "  SKIP     clang-tidy not installed"; \
		exit 0; \
	fi; \
	for s in $(ALL_SRCS); do \
		printf '  %-8s %s\n' 'TIDY' "$$s"; \
		$(CLANG_TIDY) -p . "$$s" || exit 1; \
	done

cppcheck:
	$(Q)if ! command -v $(CPPCHECK) >/dev/null 2>&1; then \
		echo "  SKIP     cppcheck not installed"; \
		exit 0; \
	fi; \
	$(CPPCHECK) --enable=warning,performance,portability \
		--inline-suppr --error-exitcode=1 --std=c++17 --quiet \
		--suppress=missingIncludeSystem \
		--suppress='*:include/internal/*' \
		-Iinclude src demos

check: check-headers cppcheck tidy

# ---------------------------------------------------------------------------

compile_commands.json:
	$(Q)printf '[\n' > $@
	$(Q)first=1; for s in $(ALL_SRCS); do \
		if [ $$first -eq 0 ]; then printf ',\n' >> $@; fi; first=0; \
		printf '  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
			"$(CURDIR)" "$$s" "$(CXX)" "$(CXXFLAGS)" "$$s" >> $@; \
	done
	$(Q)printf '\n]\n' >> $@
	$(call say,GEN,$@)

clean:
	$(Q)rm -rf build

distclean: clean
	$(Q)rm -f compile_commands.json

help:
	@sed -n '1,20p' Makefile | sed 's/^## \{0,1\}//'

-include $(DEPS)
