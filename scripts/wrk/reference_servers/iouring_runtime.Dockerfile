FROM debian:bookworm-slim AS build

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        liburing-dev \
        pkg-config && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_WEB=ON \
        -DBUILD_EXAMPLES=ON \
        -DBUILD_TESTS=OFF && \
    cmake --build build --target hello_http -j"$(nproc)"

FROM debian:bookworm-slim

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        libstdc++6 \
        liburing2 && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/bin/hello_http /usr/local/bin/hello_http
COPY --from=build /src/scripts/wrk/reference_servers/www /srv/www

EXPOSE 8080

ENV HELLO_HTTP_PORT=8080
ENV HELLO_HTTP_LOG_LEVEL=off
ENV HELLO_HTTP_STATIC_ROOT=/srv/www
WORKDIR /srv

ENTRYPOINT ["/usr/local/bin/hello_http"]
