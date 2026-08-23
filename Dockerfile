FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG AFF3CT_REF=v4.1.2
ARG BUILD_JOBS=2

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libboost-dev \
        libfftw3-dev \
        libuhd-dev \
        libyaml-cpp-dev \
        nlohmann-json3-dev \
        pkg-config \
        uhd-host \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --branch "${AFF3CT_REF}" --depth 1 https://github.com/aff3ct/aff3ct.git /tmp/aff3ct \
    && git -C /tmp/aff3ct submodule update --init --recursive \
    && cmake -S /tmp/aff3ct -B /tmp/aff3ct/build -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_CXX_FLAGS="-funroll-loops -faligned-new" \
        -DAFF3CT_COMPILE_EXE=OFF \
        -DAFF3CT_COMPILE_SHARED_LIB=ON \
        -DSPU_STACKTRACE=OFF \
        -DSPU_STACKTRACE_SEGFAULT=OFF \
    && cmake --build /tmp/aff3ct/build --parallel "${BUILD_JOBS}" \
    && cmake --install /tmp/aff3ct/build \
    && rm -rf /tmp/aff3ct

WORKDIR /src
COPY . /src

RUN cmake -S /src -B /src/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /src/build --parallel "${BUILD_JOBS}" \
    && cmake --install /src/build --prefix /opt/openisac

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libboost-chrono1.83.0t64 \
        libboost-filesystem1.83.0 \
        libboost-serialization1.83.0 \
        libboost-thread1.83.0 \
        libfftw3-single3 \
        libgomp1 \
        libpython3.12t64 \
        libuhd4.6.0t64 \
        libusb-1.0-0 \
        libyaml-cpp0.8 \
        uhd-host \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local /usr/local
COPY --from=builder /opt/openisac /opt/openisac
COPY config /opt/openisac/config
COPY LDPC_504_1008.alist /opt/openisac/
COPY LDPC_504_1008G.alist /opt/openisac/
COPY PEGReg504x1008_Gen.alist /opt/openisac/
COPY docker-entrypoint.sh /usr/local/bin/openisac-run

RUN ldconfig \
    && uhd_images_downloader \
    && chmod +x /usr/local/bin/openisac-run \
    && mkdir -p /work/build

WORKDIR /work/build
ENV OPENISAC_HOME=/opt/openisac
ENV OPENISAC_RUN_DIR=/work/build

ENTRYPOINT ["openisac-run"]
CMD ["modulator", "x310"]
