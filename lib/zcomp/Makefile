# SPDX-License-Identifier: GPL-2.0-only

libzcomp-y	:= zcomp.o

libzcomp-$(CONFIG_ZCOMP_BACKEND_LZO)	+= backend_lzorle.o backend_lzo.o
libzcomp-$(CONFIG_ZCOMP_BACKEND_LZ4)	+= backend_lz4.o
libzcomp-$(CONFIG_ZCOMP_BACKEND_LZ4HC)	+= backend_lz4hc.o
libzcomp-$(CONFIG_ZCOMP_BACKEND_ZSTD)	+= backend_zstd.o
libzcomp-$(CONFIG_ZCOMP_BACKEND_DEFLATE)	+= backend_deflate.o
libzcomp-$(CONFIG_ZCOMP_BACKEND_842)	+= backend_842.o

obj-$(CONFIG_ZCOMP)	+= libzcomp.o
