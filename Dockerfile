FROM node:22-bookworm AS dashboard-build

WORKDIR /workspace/dashboard
COPY dashboard/package.json dashboard/package-lock.json ./
RUN npm ci
COPY dashboard/ ./
RUN npm run build

FROM debian:bookworm-slim AS cpp-build

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      cmake \
      g++ \
      libabsl-dev \
      libgrpc++-dev \
      libprotobuf-dev \
      libsqlite3-dev \
      make \
      protobuf-compiler \
      protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY CMakeLists.txt common.cmake measure.proto ./
COPY src/ ./src/
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel 2

FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      libgrpc++1.51 \
      libprotobuf32 \
      libsqlite3-0 \
      openssl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home measure \
    && mkdir -p /var/lib/measure /run/measure/certs \
    && chown -R measure:measure /var/lib/measure /run/measure

RUN if dpkg-query -W -f='${binary:Package}\n' | grep -i brotli; then \
      echo "Brotli packages must not be installed in the runtime image" >&2; \
      exit 1; \
    fi

COPY --from=cpp-build /workspace/build/measure_server /usr/local/bin/
COPY --from=cpp-build /workspace/build/measure_client /usr/local/bin/
COPY --from=cpp-build /workspace/build/measure_healthcheck /usr/local/bin/
COPY --from=dashboard-build /workspace/dashboard/dist /opt/measure/dashboard/dist
COPY deployment/demo/generate-certs.sh /usr/local/bin/generate-demo-certs
RUN chmod 0755 /usr/local/bin/generate-demo-certs

USER 10001:10001
EXPOSE 50051 8080
ENTRYPOINT ["/usr/local/bin/measure_server"]
