# Kakadu builder image.
#
# Stages:
#   kakadu-builder  builds the Kakadu SDK (libraries + apps) and stages the
#                   results under /opt/staging, ready for downstream images to
#                   pick up with COPY --from
#   test            builds this vips plugin against kakadu-builder and runs
#                   the pytest suite plus a jpylyzer check
#
# Build the builder image (default target):
#   docker build -t kakadu-builder .
# Build and run the plugin tests:
#   docker build --target test .
#
# Expects the licensed SDK zip at kakadu/kakadu.zip (top-level dir "kakadu/").

FROM debian:trixie AS kakadu-builder

# set by buildx/BuildKit, eg. amd64 or arm64
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive
ENV JAVA_HOME=/usr/lib/jvm/java-21-openjdk-${TARGETARCH}

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates unzip make g++ libnuma-dev openjdk-21-jdk-headless \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /opt/staging/usr/lib /opt/staging/usr/local/bin

COPY kakadu/kakadu.zip /opt/

# KAKADU LIB + APPS
#
# KAKADU_ARCH is written to /opt/kakadu/KAKADU_ARCH so later stages and
# downstream images know which lib/ and bin/ subdirectory to use.
RUN case "${TARGETARCH}" in \
        amd64) KAKADU_ARCH=Linux-x86-64-gcc ;; \
        arm64) KAKADU_ARCH=Linux-arm-64-gcc ;; \
        *) echo "unsupported arch ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && cd /opt && unzip -q kakadu.zip && rm kakadu.zip \
    # same steps as make/Makefile-<arch> "all"; managed/ must be serial
    # since it generates kdu_aux.cpp with kdu_hyperdoc before compiling it
    && make -C /opt/kakadu/coresys/make -j"$(nproc)" \
        -f Makefile-${KAKADU_ARCH} all \
    && make -C /opt/kakadu/apps/make -j"$(nproc)" \
        -f Makefile-${KAKADU_ARCH} all_but_hyperdoc \
    && make -C /opt/kakadu/managed/make -f Makefile-${KAKADU_ARCH} all \
    && echo ${KAKADU_ARCH} > /opt/kakadu/KAKADU_ARCH \
    # shared libs (and JNI lib) for runtime images
    && cp -P /opt/kakadu/lib/${KAKADU_ARCH}/*.so* /opt/staging/usr/lib/ \
    # command-line apps: kdu_compress, kdu_expand, kdu_jp2info, ...
    && find /opt/kakadu/bin/${KAKADU_ARCH} -maxdepth 1 -type f -perm -u+x \
        -exec cp -P {} /opt/staging/usr/local/bin/ \; \
    # full SDK kit: headers, static + shared libs, apps -- laid out so that
    # KAKADUHOME=/opt/staging/opt/kakadu works for src/Makefile
    && mkdir -p /opt/staging/opt/kakadu/lib /opt/staging/opt/kakadu/bin \
        /opt/staging/opt/kakadu/managed \
    && cp -Pr /opt/kakadu/lib/${KAKADU_ARCH} /opt/staging/opt/kakadu/lib/ \
    && cp -Pr /opt/kakadu/bin/${KAKADU_ARCH} /opt/staging/opt/kakadu/bin/ \
    && cp -Lr /opt/kakadu/managed/all_includes \
        /opt/staging/opt/kakadu/managed/ \
    && cp /opt/kakadu/KAKADU_ARCH /opt/staging/opt/kakadu/ \
    && ls -l /opt/staging/usr/lib /opt/staging/usr/local/bin

ENV PATH=/opt/staging/usr/local/bin:${PATH}
ENV LD_LIBRARY_PATH=/opt/staging/usr/lib

# ---------------------------------------------------------------------------

FROM kakadu-builder AS test

RUN apt-get update && apt-get install -y --no-install-recommends \
        pkg-config libvips-dev libvips-tools python3-venv python3-dev \
    && rm -rf /var/lib/apt/lists/* \
    # pyvips and jpylyzer are not packaged for trixie
    && python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir pyvips pytest jpylyzer

ENV PATH=/opt/venv/bin:${PATH}

COPY src /build/src
COPY test /build/test

WORKDIR /build/src
RUN make KAKADUHOME=/opt/staging/opt/kakadu \
        KAKADU_ARCH="$(cat /opt/staging/opt/kakadu/KAKADU_ARCH)" \
    && make install KAKADUHOME=/opt/staging/opt/kakadu \
        KAKADU_ARCH="$(cat /opt/staging/opt/kakadu/KAKADU_ARCH)"

WORKDIR /build
RUN python3 -m pytest -v test \
    && vips copy test/images/sample_640×426.ppm /tmp/out.jph \
    && vips copy test/images/sample_640×426.ppm /tmp/out.jp2 \
    && kdu_jp2info -i /tmp/out.jph | grep brand \
    && kdu_jp2info -i /tmp/out.jp2 | grep brand \
    && jpylyzer --format jp2 /tmp/out.jp2 | grep -q "<isValid format=\"jp2\">True" \
    && echo "jpylyzer: out.jp2 valid"

# ---------------------------------------------------------------------------

# default target is the builder image
FROM kakadu-builder
