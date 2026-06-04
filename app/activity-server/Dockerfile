FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        ninja-build \
        pkg-config \
        libssl-dev \
        liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF \
        -DBUILD_WEB=OFF \
        -DBUILD_PROXY=OFF \
        -DBUILD_GAME=OFF \
        -DBUILD_MEDIA=ON \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_ACTIVITY_SERVER=ON \
    && cmake --build build --target activity_server

FROM --platform=$BUILDPLATFORM node:20-alpine AS frontend-build
WORKDIR /frontend

COPY app/activity-server/frontend/package.json app/activity-server/frontend/package-lock.json ./
RUN npm ci

COPY app/activity-server/frontend/ ./
RUN npm run build

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ARG TARGETARCH
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        ffmpeg \
        libssl3 \
        liburing2 \
    && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    arch="${TARGETARCH:-$(dpkg --print-architecture)}"; \
    case "${arch}" in \
        amd64|x86_64) ytdlp_asset="yt-dlp_linux" ;; \
        arm64|aarch64) ytdlp_asset="yt-dlp_linux_aarch64" ;; \
        arm/v7|armhf) ytdlp_asset="yt-dlp_linux_armv7l" ;; \
        *) echo "unsupported yt-dlp release architecture: ${arch}" >&2; exit 1 ;; \
    esac; \
    curl -fL --retry 3 -o /usr/local/bin/yt-dlp \
        "https://github.com/yt-dlp/yt-dlp/releases/latest/download/${ytdlp_asset}"; \
    chmod 0755 /usr/local/bin/yt-dlp; \
    /usr/local/bin/yt-dlp --version

RUN useradd --system --uid 10002 --home-dir /nonexistent --shell /usr/sbin/nologin activity \
    && mkdir -p /var/lib/iouring-runtime/activity-server/downloads \
    && chown -R activity:activity /var/lib/iouring-runtime/activity-server

COPY --from=build /src/build/bin/activity_server /usr/local/bin/activity_server
COPY --from=frontend-build /frontend/dist /usr/share/activity-server/frontend

USER activity
ENV ACTIVITY_HOST=0.0.0.0 \
    ACTIVITY_PORT=8000 \
    ACTIVITY_DOWNLOAD_DIR=/var/lib/iouring-runtime/activity-server/downloads \
    ACTIVITY_STATIC_DIR=/usr/share/activity-server/frontend
EXPOSE 8000
ENTRYPOINT ["/usr/local/bin/activity_server"]
