FROM mcr.microsoft.com/devcontainers/rust:latest AS devcontainer

# Install additional packages
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \         
    build-essential \
    cmake \
    git \
    iputils-ping \
    dnsutils \
    libssl-dev \
    v4l-utils \
    python3-dev \
    pipx \
    libglib2.0-dev \
    flex \
    bison \
    ninja-build \
    libx264-dev \
    libopenh264-dev \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    nasm \
    yasm \
    pkg-config


# Install meson and ensure it's in PATH
RUN pipx install meson
ENV PATH="/root/.local/bin:$PATH"

# Clone gstreamer (cached layer - only rebuilds if gstreamer repo changes)
WORKDIR /workspaces
RUN git clone https://gitlab.freedesktop.org/gstreamer/gstreamer.git

# Build gstreamer base (cached layer)
WORKDIR /workspaces/gstreamer
RUN meson setup --prefix=/usr/local --libdir=lib \
    -Dgpl=enabled \
    -Dugly=enabled \
    -Dbad=enabled \
    -Dlibav=enabled \
    --buildtype=release \
    build
RUN meson compile -C build

# Copy only the patch file (this layer rebuilds when patch changes)
COPY gstscream/udp_ecn_diff.txt udp_ecn_diff.txt
RUN patch -p1 < udp_ecn_diff.txt
RUN meson compile -C build
RUN meson install -C build

# Copy only the build script (this layer rebuilds when script changes)
WORKDIR /workspaces
COPY gstscream/ gstscream
COPY code/ code
RUN ./gstscream/scripts/build.sh