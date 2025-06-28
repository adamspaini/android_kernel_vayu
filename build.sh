#!/bin/bash

# === SETUP VARIABLE ===
kernel_dir="${PWD}"
objdir="${kernel_dir}/out"
builddir="${kernel_dir}/build"
anykernel="/workspace/jale/AnyKernel3"
clang_repo="https://gitlab.com/crdroidandroid/android_prebuilts_clang_host_linux-x86_clang-r547379.git"
CLANG_DIR="/workspace/jale/clang"
TC_DIR="/workspace"
CONFIG_FILE="vayu_defconfig"

# === ENVIRONMENT EXPORT ===
export ARCH="arm64"
export KBUILD_BUILD_HOST="AnymoreProject"
export KBUILD_BUILD_USER="t.me"
export KBUILD_BUILD_FEATURES="Dev-Jale"
export PATH="${CLANG_DIR}/bin:${PATH}"
export CCACHE=$(command -v ccache)

# === COLOR CODES ===
NC='\033[0m'
RED='\033[0;31m'
LRD='\033[1;31m'
LGR='\033[1;32m'

# === TOOLCHAIN CHECK ===
if ! [ -d "$CLANG_DIR" ]; then
    echo -e "${LGR}Toolchain not found! Cloning to $CLANG_DIR...${NC}"
    if ! git clone -q --depth=1 --single-branch "$clang_repo" -b 15.0 "$CLANG_DIR"; then
        echo -e "${LRD}Cloning failed! Aborting...${NC}"
        exit 1
    fi
fi

# === DEFCONFIG GENERATION ===
if [ "$VAYU_CONFIG_REGEN" = "true" ]; then
    echo -e "${LGR}Regenerating defconfig...${NC}"
    make ARCH=$ARCH O=$objdir vendor/sm8150-perf_defconfig \
        vendor/debugfs.config \
        vendor/xiaomi/sm8150-common.config \
        vendor/xiaomi/vayu.config
    cp "$objdir/.config" "arch/arm64/configs/${CONFIG_FILE}"
else
    echo -e "${RED}Not regenerating config${NC}"
fi

# === FUNCTION: Generate defconfig ===
make_defconfig() {
    echo -e "${LGR}Generating defconfig...${NC}"
    make -s ARCH=$ARCH O=$objdir $CONFIG_FILE -j$(nproc)
    if [ $? -ne 0 ]; then
        echo -e "${LRD}Failed to generate defconfig${NC}"
        exit 1
    fi
}

# === FUNCTION: Compile kernel ===
compile() {
    echo -e "${LGR}Compiling kernel...${NC}"
    make -j$(nproc) \
        ARCH=$ARCH \
        O=$objdir \
        CC="ccache clang" \
        CLANG_TRIPLE="aarch64-linux-gnu-" \
        CROSS_COMPILE="aarch64-linux-gnu-" \
        CROSS_COMPILE_ARM32="arm-linux-gnueabi-" \
        LD=ld.lld \
        LLVM=1 LLVM_IAS=1
    if [ $? -ne 0 ]; then
        echo -e "${LRD}Kernel build failed${NC}"
        exit 1
    fi
}

# === FUNCTION: Check output ===
completion() {
    local image="${objdir}/arch/arm64/boot/Image"
    local dtbo="${objdir}/arch/arm64/boot/dtbo.img"

    if [[ -f "$image" && -f "$dtbo" ]]; then
        echo -e "${LGR}############################################"
        echo -e "${LGR}############# OkThisIsEpic!  ##############"
        echo -e "${LGR}############################################${NC}"
    else
        echo -e "${LRD}############################################"
        echo -e "${LRD}##         This Is Not Epic :'(           ##"
        echo -e "${LRD}############################################${NC}"
        exit 1
    fi
}

# === FUNCTION: Pack flashable zip ===
pack_zip() {
    echo -e "${LGR}Packing kernel into flashable zip...${NC}"

    local image="$objdir/arch/arm64/boot/Image"
    local dtbo="$objdir/arch/arm64/boot/dtbo.img"

    if [[ ! -f "$image" || ! -f "$dtbo" ]]; then
        echo -e "${LRD}Missing Image or dtbo.img, cannot pack!${NC}"
        exit 1
    fi

    cp "$image" "$anykernel/"
    cp "$dtbo" "$anykernel/"

    # Gabungkan semua dtb menjadi dtb.img
    echo -e "${LGR}Generating dtb.img from sm8150-v2*.dtb...${NC}"
    find "$objdir/arch/arm64/boot/dts/qcom" -name 'sm8150-v2*.dtb' -exec cat {} + > "$anykernel/dtb.img"

    cd "$anykernel"
    local zip_name="Kernel_$(date +"%Y%m%d-%H%M").zip"
    zip -r9 "$zip_name" * -x '*.git*' '*.DS_Store' '*.zip'
    
    echo -e "${LGR}Flashable zip created: ${zip_name}${NC}"
}

# === BUILD EXECUTION ===
make_defconfig
compile
completion
pack_zip

cd "$kernel_dir"
