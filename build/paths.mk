# build/paths.mk - 公共编译路径配置
#
# 约定：包含本文件的 Makefile 必须先定义变量 ROOT，
#       指向项目根目录的【绝对路径】。
#       顶级:   ROOT := $(CURDIR)
#       一层深: ROOT := $(abspath $(CURDIR)/..)
#       两层深: ROOT := $(abspath $(CURDIR)/../..)
#
# 设计目标：
#   1) 自动扫描 include/ 下所有子目录，生成 -I 列表。
#      以后在 include/ 下新建任何子目录（如 include/drivers/gpu），
#      无需改任何 Makefile，find 会自动发现。
#   2) 路径全部用 $(ROOT) 前缀的绝对路径，不受子 Makefile 深度影响，
#      一层深和两层深的子目录共用同一份 INCLUDES。
#   3) 源码目录也列出，供 .c 跨目录 include 同模块 .h 用。

# 1) include/ 树 含 include 自身和所有子目录，自动适配新增目录
INCDIRS := $(shell find $(ROOT)/include -type d 2>/dev/null)

# 2) 源码根与各模块源码目录
SRCDIRS := $(ROOT) $(ROOT)/kernel $(ROOT)/mm $(ROOT)/drivers \
           $(ROOT)/fs $(ROOT)/test $(ROOT)/utils $(ROOT)/boot \
           $(ROOT)/asm $(ROOT)/font

# 3) 合成 -I 列表（去重；find 结果与源码目录合并排序）
INCLUDES := $(addprefix -I,$(sort $(INCDIRS) $(SRCDIRS)))
