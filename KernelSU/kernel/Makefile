kernelsu-objs := ksu.o
kernelsu-objs += allowlist.o
kernelsu-objs += apk_sign.o
kernelsu-objs += sucompat.o
kernelsu-objs += throne_tracker.o
kernelsu-objs += core_hook.o
kernelsu-objs += ksud.o
kernelsu-objs += embed_ksud.o
kernelsu-objs += kernel_compat.o

kernelsu-objs += selinux/selinux.o
kernelsu-objs += selinux/sepolicy.o
kernelsu-objs += selinux/rules.o

ccflags-y += -I$(srctree)/security/selinux -I$(srctree)/security/selinux/include
ccflags-y += -I$(objtree)/security/selinux -include $(srctree)/include/uapi/asm-generic/errno.h

ccflags-y += -Wno-implicit-function-declaration -Wno-strict-prototypes -Wno-int-conversion
ccflags-y += -Wno-declaration-after-statement -Wno-unused-function

obj-$(CONFIG_KSU) += kernelsu.o

obj-$(CONFIG_KPM) += kpm/

# .git is a text file while the module is imported by 'git submodule add'.
KSU_GIT_VERSION := 2666
# ksu_version: major * 10000 + git version + 200 for historical reasons
$(eval KSU_VERSION=$(shell expr 10000 + $(KSU_GIT_VERSION) + 200))
ccflags-y += -DKSU_VERSION=$(KSU_VERSION)

ifndef KSU_EXPECTED_SIZE
KSU_EXPECTED_SIZE := 0x35c
endif

ifndef KSU_EXPECTED_HASH
KSU_EXPECTED_HASH := 947ae944f3de4ed4c21a7e4f7953ecf351bfa2b36239da37a34111ad29993eef
endif

ifdef KSU_MANAGER_PACKAGE
ccflags-y += -DKSU_MANAGER_PACKAGE=\"$(KSU_MANAGER_PACKAGE)\"
$(info -- SukiSU Manager package name: $(KSU_MANAGER_PACKAGE))
endif

$(info -- SukiSU Manager signature size: $(KSU_EXPECTED_SIZE))
$(info -- SukiSU Manager signature hash: $(KSU_EXPECTED_HASH))
$(info -- Supported Unofficial Manager: 5ec1cff (GKI) rsuntk (Non-GKI) ShirkNeko udochina (GKI and non-GKI and KPM))

ifeq ($(strip $(CONFIG_KSU_MANUAL_HOOK)),y)
$(info -- SukiSU: Manual hooking enabled!)
else
$(info -- SukiSU: KPROBES hooking enabled!)
ccflags-y += -DCONFIG_KSU_KPROBES_HOOK
endif

KERNEL_VERSION := $(VERSION).$(PATCHLEVEL)
KERNEL_TYPE := Non-GKI
# Check for GKI 2.0 (5.10+ or 6.x+)
ifneq ($(shell test \( $(VERSION) -ge 5 -a $(PATCHLEVEL) -ge 10 \) -o $(VERSION) -ge 6; echo $$?),0)
# Check for GKI 1.0 (5.4)
ifeq ($(shell test $(VERSION)-$(PATCHLEVEL) = 5-4; echo $$?),0)
KERNEL_TYPE := GKI 1.0
endif
else
KERNEL_TYPE := GKI 2.0
endif
$(info -- KERNEL_VERSION: $(KERNEL_VERSION))
$(info -- KERNEL_TYPE: $(KERNEL_TYPE))

# Check if KPM is enabled
ifeq ($(CONFIG_KPM),y)
$(info -- KPM is enabled)
else
$(info -- KPM is disabled)
endif

# 检查原子函数
ifeq ($(shell grep -q "atomic_inc_not_zero" $(srctree)/kernel/cred.c; echo $$?),0)
ATOMIC_INC_FUNC = atomic_inc_not_zero
else ifeq ($(shell grep -q "atomic_long_inc_not_zero" $(srctree)/kernel/cred.c; echo $$?),0)
ATOMIC_INC_FUNC = atomic_long_inc_not_zero
else
$(info -- SukiSU compat: Neither atomic_inc_not_zero nor atomic_long_inc_not_zero found in kernel/cred.c)
endif
$(info -- SukiSU compat: Using $(ATOMIC_INC_FUNC) in get_cred_rcu patch.)

# 写入签名
ccflags-y += -DEXPECTED_SIZE=$(KSU_EXPECTED_SIZE)
ccflags-y += -DEXPECTED_HASH=\"$(KSU_EXPECTED_HASH)\"

# SELinux 驱动程序检查
ifeq ($(shell grep -q "current_sid(void)" $(srctree)/security/selinux/include/objsec.h; echo $$?),0)
ccflags-y += -DKSU_COMPAT_HAS_CURRENT_SID
endif

ifeq ($(shell grep -q "struct selinux_state " $(srctree)/security/selinux/include/security.h; echo $$?),0)
ccflags-y += -DKSU_COMPAT_HAS_SELINUX_STATE
endif

# 该功能在 linux 5.0-rc1 中引入
ifeq ($(shell grep -q "get_cred_rcu" $(srctree)/include/linux/cred.h; echo $$?),0)
ccflags-y += -DKSU_COMPAT_HAS_GET_CRED_RCU
else
ifeq ($(shell grep -q "atomic_long_t\s\+\usage" $(srctree)/include/linux/cred.h; echo $$?),0)
ccflags-y += -DKSU_COMPAT_ATOMIC_LONG
endif
ifeq ($(shell grep -q "int\s\+\non_rcu" $(srctree)/include/linux/cred.h; echo $$?),0)
ccflags-y += -DKSU_COMPAT_HAS_NONCONST_CRED
endif
endif

# 处理可选的backport
ifeq ($(shell grep -q "strncpy_from_user_nofault" $(srctree)/include/linux/uaccess.h; echo $$?),0)
ccflags-y += -DKSU_STRNCPY_FROM_USER_NOFAULT
endif

ifeq ($(shell grep -q "ssize_t kernel_read" $(srctree)/fs/read_write.c; echo $$?),0)
ccflags-y += -DKSU_KERNEL_READ
endif

ifeq ($(shell grep "ssize_t kernel_write" $(srctree)/fs/read_write.c | grep -q "const void" ; echo $$?),0)
ccflags-y += -DKSU_KERNEL_WRITE
endif

# 检查三星 UH 驱动程序
ifeq ($(shell grep -q "CONFIG_KDP_CRED" $(srctree)/kernel/cred.c; echo $$?),0)
ccflags-y += -DSAMSUNG_UH_DRIVER_EXIST
endif

ccflags-y += -DKSU_UMOUNT
ccflags-y += -DKSU_COMPAT_GET_CRED_RCU

# Add the get_cred_rcu function to cred.h if not already present
ifneq ($(shell grep -Eq "^static inline const struct cred \*get_cred_rcu" $(srctree)/include/linux/cred.h; echo $$?),0)
$(info -- SukiSU compat: adding function 'static inline const struct cred *get_cred_rcu(const struct cred *cred);' to $(srctree)/include/linux/cred.h)
GET_CRED_RCU = static inline const struct cred *get_cred_rcu(const struct cred *cred)\n\
{\n\t\
        struct cred *nonconst_cred = (struct cred *) cred;\n\t\
        if (!cred)\n\t\t\
                return NULL;\n\t\
        if (!$(ATOMIC_INC_FUNC)(&nonconst_cred->usage))\n\t\t\
                return NULL;\n\t\
        validate_creds(cred);\n\t\
        return cred;\n\
}\n
$(shell grep -qF "$(GET_CRED_RCU)" $(srctree)/include/linux/cred.h || sed -i '/^static inline void put_cred/i $(GET_CRED_RCU)' $(srctree)/include/linux/cred.h)

# Modify get_task_cred in cred.c
$(info -- SukiSU compat: modifying 'get_task_cred' function in $(srctree)/kernel/cred.c)
$(shell sed -i "s/!$(ATOMIC_INC_FUNC)(&((struct cred \*)cred)->usage)/!get_cred_rcu(cred)/g" $(srctree)/kernel/cred.c)
endif

ifneq ($(shell grep -Eq "^static int can_umount" $(srctree)/fs/namespace.c; echo $$?),0)
$(info -- SukiSU compat: adding function 'static int can_umount(const struct path *path, int flags);' to $(srctree)/fs/namespace.c)
CAN_UMOUNT = static int can_umount(const struct path *path, int flags)\n\
{\n\t\
        struct mount *mnt = real_mount(path->mnt);\n\t\
        if (flags & ~(MNT_FORCE | MNT_DETACH | MNT_EXPIRE | UMOUNT_NOFOLLOW))\n\t\t\
                return -EINVAL;\n\t\
        if (!may_mount())\n\t\t\
                return -EPERM;\n\t\
        if (path->dentry != path->mnt->mnt_root)\n\t\t\
                return -EINVAL;\n\t\
        if (!check_mnt(mnt))\n\t\t\
                return -EINVAL;\n\t\
        if (mnt->mnt.mnt_flags & MNT_LOCKED)\n\t\t\
                return -EINVAL;\n\t\
        if (flags & MNT_FORCE && !capable(CAP_SYS_ADMIN))\n\t\t\
                return -EPERM;\n\t\
        return 0;\n\
}\n
$(shell sed -i '/^static bool is_mnt_ns_file/i $(CAN_UMOUNT)' $(srctree)/fs/namespace.c;)
endif

ifneq ($(shell grep -Eq "^int path_umount" $(srctree)/fs/namespace.c; echo $$?),0)
$(info -- SukiSU compat: adding function 'int path_umount(struct path *path, int flags);' to $(srctree)/fs/namespace.c)
PATH_UMOUNT = int path_umount(struct path *path, int flags)\n\
{\n\t\
        struct mount *mnt = real_mount(path->mnt);\n\t\
        int ret;\n\t\
        ret = can_umount(path, flags);\n\t\
        if (!ret)\n\t\t\
                ret = do_umount(mnt, flags);\n\t\
        dput(path->dentry);\n\t\
        mntput_no_expire(mnt);\n\t\
        return ret;\n\
}\n
$(shell sed -i '/^static bool is_mnt_ns_file/i $(PATH_UMOUNT)' $(srctree)/fs/namespace.c;)
endif

ifneq ($(shell grep -Eq "^int path_umount" $(srctree)/fs/internal.h; echo $$?),0)
$(shell sed -i '/^extern void __init mnt_init/a int path_umount(struct path *path, int flags);' $(srctree)/fs/internal.h;)
$(info -- SukiSU compat: adding 'int path_umount(struct path *path, int flags);' to $(srctree)/fs/internal.h)
endif

# Do checks before compile
ifeq ($(shell grep -q "int\s\+\path_umount" $(srctree)/fs/namespace.c; echo $$?),0)
$(info -- SukiSU: checks ok. Found path_umount)
else
$(info -- SukiSU: checks failed, abort.)
$(error -- Backporting path_umount is mandatory !! Read: https://kernelsu.org/guide/how-to-integrate-for-non-gki.html#how-to-backport-path-umount)
endif


## For susfs stuff ##
ifeq ($(shell test -e $(srctree)/fs/susfs.c; echo $$?),0)
$(eval SUSFS_VERSION=$(shell cat $(srctree)/include/linux/susfs.h | grep -E '^#define SUSFS_VERSION' | cut -d' ' -f3 | sed 's/"//g'))
$(info )
$(info -- SUSFS_VERSION: $(SUSFS_VERSION))
else
$(info -- You have not integrate susfs in your kernel.)
$(info -- Read: https://gitlab.com/simonpunk/susfs4ksu)
endif
# Keep a new line here!! Because someone may append config
