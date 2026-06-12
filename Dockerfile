FROM vitasdk/vitasdk:latest

RUN apk add --no-cache \
    cmake \
    make \
    git \
    ca-certificates

# vdpm is pre-installed at /usr/local/vitasdk/bin/vdpm
# -f = force install (also works for first-time installs)
RUN vdpm -f \
    zlib \
    libpng \
    libjpeg-turbo \
    freetype \
    openssl \
    curl \
    zstd \
    vita2d

WORKDIR /src
COPY . .

RUN cmake -B /build -DCMAKE_BUILD_TYPE=Release . && \
    cmake --build /build --parallel "$(nproc)"

# Mount a host directory to /output to retrieve the VPK
CMD ["sh", "-c", "cp /build/vitaImmich.vpk /output/vitaImmich.vpk && echo 'Done: /output/vitaImmich.vpk'"]
