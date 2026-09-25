FROM ubuntu:24.04

# build-essential stays in the image on purpose: workers need gcc/ar/ld to do
# their job, and the client needs the preprocessor
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config python3 \
      libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler \
      libpq-dev libssl-dev postgresql-client \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/distcompile
COPY CMakeLists.txt ./
COPY proto proto
COPY sql sql
COPY src src
COPY tests tests
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build \
    && cp build/dc-coordinator build/dc-worker build/dcc /usr/local/bin/

WORKDIR /work
