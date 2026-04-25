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
        liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_WEB=ON \
        -DBUILD_EXAMPLES=ON \
        -DBUILD_TESTS=OFF \
    && cmake --build build --target dropapp

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates liburing2 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --system --uid 10001 --home-dir /nonexistent --shell /usr/sbin/nologin dropapp \
    && mkdir -p /data \
    && chown -R dropapp:dropapp /data

COPY --from=build /src/build/bin/dropapp /usr/local/bin/dropapp
COPY --from=build /src/examples/web/dropapp/static /usr/share/dropapp/static

USER dropapp
ENV DROPAPP_HOST=0.0.0.0 \
    DROPAPP_PORT=3000 \
    DROPAPP_ROOT=/data \
    DROPAPP_STATIC_ROOT=/usr/share/dropapp/static
EXPOSE 3000
ENTRYPOINT ["/usr/local/bin/dropapp"]
