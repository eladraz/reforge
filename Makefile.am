# Autotools build of the reforge driver. The CMake build (CMakeLists.txt) is
# the primary one; this file mirrors it for the dismount-style toolchain.
# Paths: ./configure --with-xstl=<xStl> --with-pelib=<pe> --with-elflib=<elf>
#                    --with-dismount=<dismount>

AM_CXXFLAGS = -Wall -std=c++14
AM_CPPFLAGS = -I $(top_srcdir)/Include -I $(XSTL_PATH)/Include \
              -I $(PELIB_PATH)/Include -I $(ELFLIB_PATH)/Include \
              -I $(DISMOUNT_PATH)/Include \
              -DXSTL_UNICODE -D_UNICODE -DLINUX

bin_PROGRAMS = reforge
noinst_LIBRARIES = libreforge_rt.a

reforge_SOURCES = Source/reforge/main.cpp \
                  Source/reforge/Options.cpp \
                  Source/reforge/Amd64Lowerer.cpp \
                  Source/reforge/BlockLinker.cpp \
                  Source/reforge/Diagnostics.cpp \
                  Source/reforge/FlowRecovery.cpp \
                  Source/reforge/HostLinker.cpp \
                  Source/reforge/MetaBuilder.cpp \
                  Source/reforge/ObjectEmitter.cpp \
                  Source/reforge/PeLoader.cpp \
                  Source/reforge/Translator.cpp

# Runtime archive linked into every translated executable (the CMake target
# reforge_rt): dispatch loop, guest memory, fault handler, import adapters, main().
libreforge_rt_a_SOURCES = Runtime/Source/Adapter.cpp \
                          Runtime/Source/AdapterRegistry.cpp \
                          Runtime/Source/FaultLine.cpp \
                          Runtime/Source/GuestMemory.cpp \
                          Runtime/Source/Program.cpp \
                          Runtime/Source/Runtime.cpp \
                          Runtime/Source/RuntimeMain.cpp \
                          Runtime/Source/StandardStreams.cpp \
                          Runtime/Source/adapters/Kernel32ExitProcess.cpp \
                          Runtime/Source/adapters/MsvcrtPrintf.cpp
libreforge_rt_a_CPPFLAGS = $(AM_CPPFLAGS) -I $(top_srcdir)/Runtime/Include

# Host link step: the CMake build defines these from its targets. Here the
# runtime archive is the one built above; REFORGE_RT_LIB can override it.
REFORGE_RT_LIB ?= $(abs_top_builddir)/libreforge_rt.a
reforge_CPPFLAGS = $(AM_CPPFLAGS) -DREFORGE_HOST_CXX='"$(CXX)"' \
                   -DREFORGE_HOST_AS='"as"' \
                   -DREFORGE_RT_LIB='"$(REFORGE_RT_LIB)"' \
                   -DREFORGE_XSTL_LIB='"$(XSTL_PATH)/out/lib/libxStlcm_static.a"'
reforge_DEPENDENCIES = libreforge_rt.a

reforge_LDADD = $(DISMOUNT_PATH)/out/lib/libdismount_static.a \
                $(PELIB_PATH)/out/lib/libpe_static.a \
                $(ELFLIB_PATH)/out/lib/libelf_static.a \
                $(XSTL_PATH)/out/lib/libxStlcm_static.a \
                -lpthread
