# SPDX-License-Identifier: GPL-2.0-only

# Unlike the kernel space, exported headers are written in standard C.
#  - Forbid C++ style comments
#  - Use '__inline__', '__asm__' instead of 'inline', 'asm'
#
# -std=c90 (equivalent to -ansi) catches the violation of those.
# We cannot go as far as adding -Wpedantic since it emits too many warnings.
UAPI_CFLAGS := -std=c90 -Wall -Werror=implicit-function-declaration

# In theory, we do not care -m32 or -m64 for header compile tests.
# It is here just because CONFIG_CC_CAN_LINK is tested with -m32 or -m64.
UAPI_CFLAGS += $(filter -m32 -m64 --target=%, $(KBUILD_CPPFLAGS) $(KBUILD_CFLAGS))

override c_flags = $(UAPI_CFLAGS) -Wp,-MMD,$(depfile) -I$(objtree)/usr/include

# The following are excluded for now because they fail to build.
#
# Do not add a new header to the blacklist without legitimate reason.
# Please consider to fix the header first.
#
# Sorted alphabetically.
header-test- += asm/ipcbuf.h
header-test- += asm/msgbuf.h
header-test- += asm/sembuf.h
header-test- += asm/shmbuf.h
header-test- += asm/signal.h
header-test- += asm/ucontext.h
header-test- += drm/vmwgfx_drm.h
header-test- += linux/am437x-vpfe.h
header-test- += linux/android/binder.h
header-test- += linux/android/binderfs.h
header-test-$(CONFIG_CPU_BIG_ENDIAN) += linux/byteorder/big_endian.h
header-test-$(CONFIG_CPU_LITTLE_ENDIAN) += linux/byteorder/little_endian.h
header-test- += linux/coda.h
header-test- += linux/elfcore.h
header-test- += linux/errqueue.h
header-test- += linux/fsmap.h
header-test- += linux/hdlc/ioctl.h
header-test- += linux/ivtv.h
header-test- += linux/kexec.h
header-test- += linux/matroxfb.h
header-test- += linux/netfilter_ipv4/ipt_LOG.h
header-test- += linux/netfilter_ipv6/ip6t_LOG.h
header-test- += linux/nfc.h
header-test- += linux/omap3isp.h
header-test- += linux/omapfb.h
header-test- += linux/patchkey.h
header-test- += linux/phonet.h
header-test- += linux/reiserfs_xattr.h
header-test- += linux/scc.h
header-test- += linux/sctp.h
header-test- += linux/signal.h
header-test- += linux/sysctl.h
header-test- += linux/usb/audio.h
header-test- += linux/v4l2-mediabus.h
header-test- += linux/v4l2-subdev.h
header-test- += linux/videodev2.h
header-test- += linux/vm_sockets.h
header-test- += sound/asequencer.h
header-test- += sound/asoc.h
header-test- += sound/asound.h
header-test- += sound/compress_offload.h
header-test- += sound/emu10k1.h
header-test- += sound/sfnt_info.h
header-test- += xen/evtchn.h
header-test- += xen/gntdev.h
header-test- += xen/privcmd.h

# More headers are broken in some architectures

ifeq ($(SRCARCH),arc)
header-test- += linux/bpf_perf_event.h
endif

ifeq ($(SRCARCH),ia64)
header-test- += asm/setup.h
header-test- += asm/sigcontext.h
header-test- += asm/perfmon.h
header-test- += asm/perfmon_default_smpl.h
header-test- += linux/if_bonding.h
endif

ifeq ($(SRCARCH),mips)
header-test- += asm/stat.h
endif

ifeq ($(SRCARCH),powerpc)
header-test- += asm/stat.h
header-test- += linux/bpf_perf_event.h
endif

ifeq ($(SRCARCH),riscv)
header-test- += linux/bpf_perf_event.h
endif

ifeq ($(SRCARCH),sparc)
header-test- += asm/stat.h
header-test- += asm/uctx.h
header-test- += asm/fbio.h
endif

# asm-generic/*.h is used by asm/*.h, and should not be included directly
header-test- += asm-generic/%

extra-y := $(patsubst $(obj)/%.h,%.hdrtest, $(shell find $(obj) -name '*.h' 2>/dev/null))

quiet_cmd_hdrtest = HDRTEST $<
      cmd_hdrtest = \
		$(CC) $(c_flags) -S -o /dev/null -x c /dev/null \
			$(if $(filter-out $(header-test-), $*.h), -include $<); \
		$(PERL) $(srctree)/scripts/headers_check.pl $(obj) $(SRCARCH) $<; \
		touch $@

$(obj)/%.hdrtest: $(obj)/%.h FORCE
	$(call if_changed_dep,hdrtest)

clean-files += $(filter-out Makefile, $(notdir $(wildcard $(obj)/*)))
