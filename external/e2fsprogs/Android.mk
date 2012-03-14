ifneq ($(TARGET_SIMULATOR),true)
use_e2fsprog_module_tags := optional
include $(call all-subdir-makefiles)
endif
