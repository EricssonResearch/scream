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

# Patch the gstreamer build to enable ECN
COPY gstscream/udp_ecn_diff.txt udp_ecn_diff.txt
RUN patch -p1 < udp_ecn_diff.txt
RUN meson compile -C build
RUN meson install -C build

# Build the scream sender and receiver
WORKDIR /workspaces
COPY gstscream/ gstscream
COPY code/ code
RUN ./gstscream/scripts/build.sh

# ---- Runtime Stage ----
FROM debian:12-slim

# Install runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3-dev v4l-utils libx264-164 libglib2.0-dev libssl3 pkg-config psmisc \
    libpng16-16 libjpeg62 libwebp7 libxml2 libopenh264-7 libcurl4 gstreamer1.0-x \
    && rm -rf /var/lib/apt/lists/*


# Copy GStreamer runtime from build stage
COPY --from=devcontainer /usr/local/lib /usr/local/lib
COPY --from=devcontainer /usr/local/share /usr/local/share
COPY --from=devcontainer /usr/local/libexec/gstreamer-1.0 /usr/local/libexec/gstreamer-1.0
COPY --from=devcontainer /usr/local/bin/gst* /usr/local/bin

ENV PYTHONPATH="/usr/local/lib/python3/dist-packages:$PYTHONPATH"

WORKDIR /app
# Create the expected directory structure for the script
RUN mkdir -p /app/scripts/../target/debug/
COPY --from=devcontainer /workspaces/gstscream/target/debug/scream_sender /app/scripts/../target/debug/scream_sender
COPY --from=devcontainer /workspaces/gstscream/target/debug/scream_receiver /app/scripts/../target/debug/scream_receiver
COPY --from=devcontainer /workspaces/gstscream/target/debug/libgstscream.so /usr/local/lib/gstreamer-1.0/libgstscream.so
COPY --from=devcontainer /workspaces/code/wrapper_lib/libscream.so /usr/local/lib/libscream.so
COPY --from=devcontainer /workspaces/gstscream/scripts/ ./scripts/

# Set up environment for GStreamer and libs
ENV LD_LIBRARY_PATH="/app:/usr/local/lib:/usr/local/lib/gstreamer-1.0:/usr/lib/gstreamer-1.0"
ENV GST_PLUGIN_PATH="/app:/usr/local/lib/gstreamer-1.0:/usr/lib/gstreamer-1.0"
ENV PATH="/usr/local/bin:/app/scripts:$PATH"

# Default entrypoint 
ENTRYPOINT ["./scripts/sender.sh"]