FROM vitasdk/vitasdk:latest

# The image is Ubuntu-based (not Alpine) -- use apt, not apk.
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    make \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# vdpm is pre-installed at /usr/local/vitasdk/bin/vdpm
#
# The image ships with (almost) the whole package set preinstalled,
# including openssl-1.1.1. Our prebuilt curl package is linked against
# the legacy openssl 1.0.2 ABI (SSL_library_init, sk_*, ...), which
# openssl-1.1.1 does not provide, so the 1.1.1 package has to be
# removed (ignoring its reverse deps -- we don't build any of them)
# before installing the legacy `openssl` package curl actually needs.
RUN vdpm pacman -- -Rdd --noconfirm openssl-1.1.1

# -f = force install (also works for first-time installs); vita2d was
# renamed to libvita2d.
RUN VDPM_NONINTERACTIVE=1 vdpm -f \
    zlib \
    libpng \
    libjpeg-turbo \
    freetype \
    openssl \
    curl \
    zstd \
    libvita2d

WORKDIR /src
COPY . .

RUN cmake -B /build -DCMAKE_BUILD_TYPE=Release . && \
    cmake --build /build --parallel "$(nproc)"

# Mount a host directory to /output to retrieve the VPK
CMD ["sh", "-c", "cp /build/vitaImmich.vpk /output/vitaImmich.vpk && echo 'Done: /output/vitaImmich.vpk'"]
