# ZZ9000 app SDK: Arm GNU Toolchain 15.3.rel2, PIC newlib, and sdk/zzapp.
# Build from the sdk/zzapp directory:
#   docker build -f docker/Dockerfile -t zz9000-app-sdk .
#
# SPDX-License-Identifier: MIT

FROM debian:bookworm-slim AS toolchain

ARG ARM_TOOLCHAIN_VERSION=15.3.rel2
ARG ARM_TOOLCHAIN_SHA256=463df9665c246fa13bab2102115bbf83440661b6eb70b44f3dfd4712ca30037a
ARG ARM_TOOLCHAIN_URL=https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-arm-none-eabi.tar.xz

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates curl xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "${ARM_TOOLCHAIN_URL}" -o /tmp/toolchain.tar.xz \
    && echo "${ARM_TOOLCHAIN_SHA256}  /tmp/toolchain.tar.xz" | sha256sum -c - \
    && mkdir -p /opt/arm-gnu-toolchain \
    && tar -xJf /tmp/toolchain.tar.xz --strip-components=1 -C /opt/arm-gnu-toolchain \
    && rm /tmp/toolchain.tar.xz

ENV PATH=/opt/arm-gnu-toolchain/bin:${PATH}

# libgloss builds an info page; the firmware CI installs texinfo for the same script.
RUN apt-get update \
    && apt-get install -y --no-install-recommends texinfo \
    && rm -rf /var/lib/apt/lists/*
COPY build_pic_newlib.sh /tmp/build_pic_newlib.sh
RUN chmod 755 /tmp/build_pic_newlib.sh \
    && ZZAPP_PIC_SYSROOT=/opt/zzapp-pic-newlib \
       ZZAPP_NEWLIB_CACHE=/tmp/newlib-cache \
       ZZAPP_NEWLIB_BUILD=/tmp/newlib-build \
       JOBS="$(nproc)" \
       /tmp/build_pic_newlib.sh \
    && rm -rf /tmp/newlib-cache /tmp/newlib-build /tmp/build_pic_newlib.sh

FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates make cmake python3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=toolchain /opt/arm-gnu-toolchain /opt/arm-gnu-toolchain
COPY --from=toolchain /opt/zzapp-pic-newlib /opt/zzapp-pic-newlib
COPY . /opt/zzapp-sdk

ENV PATH=/opt/arm-gnu-toolchain/bin:${PATH}
ENV ZZAPP_SDK=/opt/zzapp-sdk
ENV ZZAPP_PIC_SYSROOT=/opt/zzapp-pic-newlib
WORKDIR /opt/zzapp-sdk
