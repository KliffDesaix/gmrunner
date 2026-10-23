# Build environment for the PortMaster (Linux handheld) runner: Debian 10 "buster" on arm64.
# Its glibc 2.28 is older than any current handheld firmware's (ArkOS has 2.30), so the binary
# runs on all of them; libstdc++ is linked in statically and SDL2 comes from the device.
FROM debian:buster
RUN sed -i 's|deb.debian.org|archive.debian.org|g; s|security.debian.org|archive.debian.org|g; /buster-updates/d' /etc/apt/sources.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends g++ make cmake pkg-config libsdl2-dev binutils curl ca-certificates xz-utils \
 && rm -rf /var/lib/apt/lists/*

# FFmpeg for video_open, linked into the runner statically: only the decoders, demuxers and
# parsers games use (H.264/HEVC/MPEG-4/VP8/VP9/Theora video, AAC/MP3/Vorbis/Opus/FLAC/PCM audio
# in MP4/MOV, WebM/MKV, Ogg, AVI and MPEG-TS files), under the LGPL (no --enable-gpl).
ARG FFMPEG_VERSION=7.1.1
ARG FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
RUN curl -fsSL "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" -o /tmp/ffmpeg.tar.xz \
 && echo "${FFMPEG_SHA256}  /tmp/ffmpeg.tar.xz" | sha256sum -c - \
 && mkdir /tmp/ffmpeg && tar -xJf /tmp/ffmpeg.tar.xz -C /tmp/ffmpeg --strip-components=1 \
 && cd /tmp/ffmpeg \
 && ./configure --prefix=/opt/ffmpeg --enable-static --disable-shared --enable-pic \
      --disable-programs --disable-doc --disable-network --disable-autodetect --disable-everything \
      --disable-avdevice --disable-avfilter --disable-postproc \
      --enable-protocol=file \
      --enable-demuxer=mov,matroska,ogg,avi,mpegts,mp3,wav,flac \
      --enable-decoder=h264,hevc,mpeg4,vp8,vp9,theora,aac,mp3,mp3float,vorbis,opus,flac,pcm_s16le,pcm_f32le \
      --enable-parser=h264,hevc,mpeg4video,vp8,vp9,aac,mpegaudio,vorbis,opus,flac \
 && make -j"$(nproc)" install \
 && cd / && rm -rf /tmp/ffmpeg /tmp/ffmpeg.tar.xz
ENV PKG_CONFIG_PATH=/opt/ffmpeg/lib/pkgconfig
