#!/usr/bin/env bash
# 在 GitHub Actions 的 MSYS2 UCRT64 环境中构建可重建的常见音视频转换组件。
set -euo pipefail

task_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
task_build="$task_repo/build-custom"
task_runtime="$task_build/conversion-ffmpeg-source-runtime"
task_work="$task_repo/out/conversion-ffmpeg-build"
task_cache="$task_repo/out/conversion-ffmpeg-sources"
task_prefix="$task_work/prefix"
for task_path in "$task_runtime" "$task_work" "$task_cache"; do
    [[ "$(realpath -m "$task_path")" == "$task_repo/"* ]] || { echo '构建路径越界'; exit 1; }
done
[[ "$task_repo" != *' '* ]] || { echo 'MSYS2 音视频构建需要无空格的工作目录'; exit 1; }
[[ ! -e "$task_runtime" && ! -e "$task_work" ]] || { echo '请使用新的空构建目录'; exit 1; }
mkdir -p "$task_cache" "$task_work" "$task_prefix" "$task_runtime/bin" "$task_runtime/sources" "$task_runtime/licenses"

fetch_source() {
    local task_name=$1 task_url=$2 task_digest=$3
    local task_file="$task_cache/$task_name"
    if [[ ! -f "$task_file" ]]; then
        curl --fail --location --retry 3 --max-time 180 --output "$task_file.download" "$task_url"
        printf '%s  %s\n' "$task_digest" "$task_file.download" | sha256sum --check --status
        mv -- "$task_file.download" "$task_file"
    fi
    printf '%s  %s\n' "$task_digest" "$task_file" | sha256sum --check --status
    cp -- "$task_file" "$task_runtime/sources/$task_name"
    tar -xf "$task_file" -C "$task_work"
}

fetch_source ffmpeg-9.0.2.tar.xz https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
    8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
fetch_source lame-3.100.tar.gz https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz \
    ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e

export PATH="${MSYSTEM_PREFIX:-/ucrt64}/bin:$PATH"
export PKG_CONFIG_PATH="$task_prefix/lib/pkgconfig"
(
    cd "$task_work/lame-3.100"
    ./configure --host=x86_64-w64-mingw32 --prefix="$task_prefix" \
        --disable-shared --enable-static --disable-frontend --disable-decoder \
        CFLAGS='-O2 -fno-strict-aliasing' LDFLAGS='-static -static-libgcc'
    make -j4
    make install
) > "$task_build/conversion-lame-build.log" 2>&1
(
    cd "$task_work/ffmpeg-9.0.2"
    ./configure --prefix="$task_prefix" --target-os=mingw32 --arch=x86_64 \
        --cc=gcc --cxx=g++ --disable-autodetect --disable-doc --disable-debug \
        --disable-shared --enable-static --disable-ffplay --disable-network \
        --disable-pthreads --enable-w32threads --enable-libmp3lame \
        --pkg-config-flags=--static --extra-cflags="-I$task_prefix/include" \
        --extra-ldflags="-L$task_prefix/lib -static -static-libgcc"
    make -j4 ffmpeg.exe ffprobe.exe
) > "$task_build/conversion-ffmpeg-build.log" 2>&1

cp -- "$task_work/ffmpeg-9.0.2/ffmpeg.exe" "$task_work/ffmpeg-9.0.2/ffprobe.exe" "$task_runtime/bin/"
cp -- "$task_work/ffmpeg-9.0.2/COPYING.LGPLv2.1" "$task_runtime/licenses/FFmpeg-LGPL-2.1.txt"
cp -- "$task_work/lame-3.100/COPYING" "$task_runtime/licenses/LAME-LGPL.txt"
cp -- "${BASH_SOURCE[0]}" "$task_runtime/sources/build_conversion_ffmpeg.sh"
"$task_runtime/bin/ffmpeg.exe" -version > "$task_runtime/licenses/ffmpeg-version.txt" 2>&1
"$task_runtime/bin/ffmpeg.exe" -buildconf > "$task_runtime/licenses/ffmpeg-buildconf.txt" 2>&1
"$task_runtime/bin/ffmpeg.exe" -encoders > "$task_work/encoders.txt" 2>&1
for task_encoder in mpeg4 aac libmp3lame flac pcm_s16le gif; do
    awk '{print $2}' "$task_work/encoders.txt" | grep -Fx "$task_encoder" >/dev/null || { echo "编码器缺失：$task_encoder"; exit 1; }
done
if grep -Eq -- '--enable-(gpl|nonfree|version3)' "$task_runtime/licenses/ffmpeg-buildconf.txt"; then
    echo '常见转换组件意外启用了 GPL/nonfree/version3 配置'; exit 1
fi
cat > "$task_runtime/sources/构建说明.md" <<'EOF'
# 音视频转换组件重建

本组件采用 FFmpeg 9.0.2 和 LAME 3.100 的公开源码，固定归档和 SHA256 随包保留。
在无空格的工作目录中检出对应 Pulse 定制版源码，使用 MSYS2 UCRT64，安装 gcc、make、nasm、pkgconf、curl、tar 和 xz 后执行：

```bash
bash scripts/build_conversion_ffmpeg.sh
```

完整配置与编译步骤均在该脚本中。FFmpeg 使用静态 LGPL 构建，未启用 GPL、nonfree 或 version3 配置；LAME 仅构建编码库，未构建 GPL 前端程序。视频输出使用 MPEG-4 Part 2/AAC，音频采用相应格式编码器。关闭网络输入，保留本地文件、管道和内置滤镜。
EOF
echo '常见音视频转换组件构建和编码器检查通过'
