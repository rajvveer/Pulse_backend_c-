# syntax=docker/dockerfile:1.7
# ──────────────────────────────────────────────────────────────────────────────
# Pulse C++ backend — production image (Linux, multi-stage).
#
# Stage 1 (build): a full toolchain + vcpkg compiles the backend and all native
#   deps (Drogon, mongo-cxx-driver, redis-plus-plus, jwt-cpp, …) from the pinned
#   vcpkg manifest. HEAVY on the first build (deps compile from source, ~20–40
#   min) but reproducible (builtin-baseline pins versions) and cached afterwards
#   via the BuildKit cache mounts.
#
# Stage 2 (runtime): a slim Ubuntu image with ONLY the binary + the shared libs
#   it links + CA certs. No compiler, no vcpkg, no source.
#
# DNS / c-ares note: on WINDOWS we had to rebuild trantor without c-ares (its
# bundled async resolver couldn't read the machine's DNS servers, so every
# outbound HTTPS call failed with BadServerAddress). Inside a Linux container
# c-ares reads /etc/resolv.conf normally, so the stock vcpkg Drogon works here.
# If you ever see BadServerAddress in container logs, see DEPLOY.md for the
# trantor -DBUILD_C-ARES=OFF overlay-port workaround.
# ──────────────────────────────────────────────────────────────────────────────

# =============================================================================
# Stage 1 — build
# =============================================================================
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain + libs vcpkg needs to bootstrap and to build the native deps from
# source (compiler, cmake, ninja, git, archivers, pkg-config, autotools, etc.).
RUN apt-get update && apt-get install -y --no-install-recommends \
      git curl zip unzip tar pkg-config \
      build-essential cmake ninja-build \
      autoconf automake autoconf-archive libtool \
      python3 zlib1g-dev linux-libc-dev \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ── vcpkg ──
ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
RUN git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /app

# Copy ONLY the manifest first so the (very expensive) dependency layer caches
# independently of source changes — editing a .cc won't recompile Drogon/mongo.
COPY vcpkg.json ./

# Build the manifest deps. The vcpkg binary cache + downloads are mounted as
# BuildKit caches so re-builds reuse already-compiled packages.
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    --mount=type=cache,target=/opt/vcpkg/downloads \
    "$VCPKG_ROOT/vcpkg" install --triplet x64-linux --x-manifest-root=/app

# Now the source. third_party/ is REQUIRED — the CMake `pulse_bcrypt` target
# compiles third_party/bcrypt/*.c; omitting it fails the build.
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY third_party ./third_party

RUN --mount=type=cache,target=/root/.cache/vcpkg \
    cmake -B build -S . -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
      -DVCPKG_TARGET_TRIPLET=x64-linux \
    && cmake --build build --config Release -j "$(nproc)"

# Collect the binary + any shared libs vcpkg produced (x64-linux is mostly
# static, but mongo/bsoncxx and a few others may be shared) into /out so the
# runtime stage gets a self-contained bundle.
RUN mkdir -p /out/lib \
    && cp build/pulse_backend /out/pulse_backend \
    && (cp -a build/vcpkg_installed/x64-linux/lib/*.so* /out/lib/ 2>/dev/null || true)

# =============================================================================
# Stage 2 — runtime (slim)
# =============================================================================
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Runtime libs: TLS (libssl3), zlib, stdc++, curl for the healthcheck, and CA
# certs so outbound HTTPS to LiveKit / FCM / Brevo / Cloudinary validates.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl libssl3 zlib1g libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && update-ca-certificates \
    && useradd -m -u 10001 pulse

WORKDIR /app

# Binary + its shared-lib bundle.
COPY --from=build /out/pulse_backend /usr/local/bin/pulse_backend
COPY --from=build /out/lib/ /usr/local/lib/
RUN ldconfig

# Drogon's config.json + setDocumentRoot("./public"). config.json is committed;
# create an empty public/ so the document root exists.
COPY config.json ./config.json
RUN mkdir -p /app/public && chown -R pulse:pulse /app

ENV LD_LIBRARY_PATH=/usr/local/lib
# Production defaults — override at run/compose time. NODE_ENV=production makes
# the server FAIL FAST if Mongo or Redis is unreachable (intended).
ENV NODE_ENV=production \
    PORT=3000 \
    THREAD_NUM=0

USER pulse
EXPOSE 3000

# Readiness check (verifies Mongo + Redis), not just liveness.
HEALTHCHECK --interval=30s --timeout=5s --start-period=40s --retries=3 \
  CMD curl -fsS "http://127.0.0.1:${PORT:-3000}/health/ready" || exit 1

CMD ["pulse_backend"]
