# auto-detect subdirs
ifneq ($(CONFIG_ARCH_ATOLL), y)
ifeq ($(CONFIG_ARCH_SDM845), y)
include $(srctree)/techpack/audio/config/sdm845auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sdm845auto.conf)
endif
ifeq ($(CONFIG_ARCH_SDM660), y)
include $(srctree)/techpack/audio/config/sdm660auto_static.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sdm660auto_static.conf)
endif
ifeq ($(CONFIG_ARCH_SDM670), y)
include $(srctree)/techpack/audio/config/sdm670auto_static.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sdm670auto_static.conf)
endif
ifeq ($(CONFIG_ARCH_SDXPOORWILLS), y)
include $(srctree)/techpack/audio/config/sdxpoorwillsauto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sdxpoorwillsauto.conf)
endif
ifeq ($(CONFIG_ARCH_SM6150), y)
include $(srctree)/techpack/audio/config/sm6150auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sm6150auto.conf)
endif
ifeq ($(CONFIG_ARCH_SDMMAGPIE), y)
include $(srctree)/techpack/audio/config/sm6150auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sm6150auto.conf)
endif
ifeq ($(CONFIG_ARCH_SM8150), y)
include $(srctree)/techpack/audio/config/sm8150auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sm8150auto.conf)
endif
ifeq ($(CONFIG_ARCH_SDMSHRIKE), y)
include $(srctree)/techpack/audio/config/sm8150auto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/sm8150auto.conf)
endif
ifeq ($(CONFIG_ARCH_TRINKET), y)
include $(srctree)/techpack/audio/config/trinketauto.conf
export $(shell sed 's/=.*//' $(srctree)/techpack/audio/config/trinketauto.conf)
endif

# Use USERINCLUDE when you must reference the UAPI directories only.
USERINCLUDE     += \
                -I$(srctree)/techpack/audio/include/uapi \

# Use LINUXINCLUDE when you must reference the include/ directory.
# Needed to be compatible with the O= option
LINUXINCLUDE    += \
                -I$(srctree)/techpack/audio/include/uapi \
                -I$(srctree)/techpack/audio/include/elliptic \
                -I$(srctree)/techpack/audio/include

#for mius start
ifeq ($(CONFIG_US_PROXIMITY), y)
LINUXINCLUDE    += \
                -I$(srctree)/techpack/audio/include/mius
endif
#for mius end

ifeq ($(CONFIG_ARCH_SDM845), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sdm845autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDM660), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sdm660autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDM670), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sdm670autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDXPOORWILLS), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sdxpoorwillsautoconf.h
endif
ifeq ($(CONFIG_ARCH_SM6150), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sm6150autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDMMAGPIE), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sm6150autoconf.h
endif
ifeq ($(CONFIG_ARCH_SM8150), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sm8150autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDMSHRIKE), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/sm8150autoconf.h
endif
ifeq ($(CONFIG_ARCH_TRINKET), y)
LINUXINCLUDE    += \
                -include $(srctree)/techpack/audio/config/trinketautoconf.h
endif

obj-y += soc/
obj-y += dsp/
obj-y += ipc/
obj-y += asoc/
else
obj-y += 4.0/
endif
