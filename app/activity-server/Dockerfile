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
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_ACTIVITY_SERVER=ON \
    && cmake --build build --target activity_server

FROM node:20-alpine AS frontend-build
WORKDIR /frontend

COPY app/activity-server/frontend/package.json app/activity-server/frontend/package-lock.json ./
RUN npm ci

COPY app/activity-server/frontend/ ./
RUN npm run build

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        ffmpeg \
        libssl3 \
        liburing2 \
        python3 \
        python3-pip \
    && python3 -m pip install --break-system-packages --no-cache-dir yt-dlp \
    && rm -rf /var/lib/apt/lists/*

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
