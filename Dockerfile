FROM ubuntu:26.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make cmake libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DZESTDB_BUILD_TESTS=OFF \
    && cmake --build build --target zestdb --parallel

FROM ubuntu:26.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends openssl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 zest \
    && mkdir -p /data/db /data/archive \
    && chown -R zest /data

COPY --from=build /src/bin/zestdb /usr/local/bin/zestdb
COPY --chown=zest docker/config.yaml /data/config.yaml

WORKDIR /data
USER zest
VOLUME /data/db
EXPOSE 7321 1237
ENTRYPOINT ["zestdb"]
