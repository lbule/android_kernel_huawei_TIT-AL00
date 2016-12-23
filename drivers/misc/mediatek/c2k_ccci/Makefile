include $(srctree)/drivers/misc/mediatek/Makefile.custom

#ifeq ($(CONFIG_MTK_C2K_SUPPORT), yes)

#ifeq ($(EVDO_VIA_HAVE_VOLT_PROTECTION), yes)#if we have voltage protection in design
#ccflags-y += -DOEM_HAVE_VOLT_PROTECTION
#endif

#ccflags-y += -DWAKE_HOST_BY_SYNC

#obj-y := core.o power.o oem.o sync.o
#obj-y	+= cbp_sdio.o modem_sdio.o
#obj-y += f_rawbulk.o rawbulk.o rawbulk_transfer.o

#else

#obj- :=dummy.o

#endif

ifeq ($(CONFIG_MTK_C2K_SUPPORT), y)

ifeq ($(EVDO_VIA_HAVE_VOLT_PROTECTION), yes) #if we have voltage protection in design
ccflags-y += -DOEM_HAVE_VOLT_PROTECTION
endif

ifeq ($(CONFIG_MTK_SECURITY_SW_SUPPORT), y)
ccflags-y  += -DENABLE_MD_IMG_SECURITY_FEATURE
endif
ifeq ($(CONFIG_MTK_SEC_MODEM_NVRAM_ANTI_CLONE), y)
ccflags-y  += -DMTK_SEC_MODEM_NVRAM_ANTI_CLONE
endif

ccflags-y += -I$(srctree)/drivers/misc/mediatek/ccmni/
ccflags-y += -DWAKE_HOST_BY_SYNC

obj-$(CONFIG_MTK_C2K_SUPPORT) += c2k_sdio.o

c2k_sdio-y := core.o power.o c2k_hw.o sync.o
c2k_sdio-y += cbp_sdio.o modem_sdio.o
c2k_sdio-y += f_rawbulk.o rawbulk.o rawbulk_transfer.o

obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/

else

obj- :=dummy.o

endif
