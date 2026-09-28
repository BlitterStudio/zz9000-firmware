# AmiSSL build environment: ubuntu + the adtools m68k toolchain AmiSSL's own
# CI uses (gcc 2.95.3 cross-compiler is a 32-bit binary, hence the i386 libc).
# Building inside this image — and cloning the sources onto its own ext4
# filesystem (see build-amissl-provider.yml) — is the supported way to build
# AmiSSL + OpenSSL 3 for m68k; a bare runner trips over sfdc/stub generation.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture i386 \
    && apt-get update -qq \
    && apt-get install -y -qq \
        libc6:i386 ca-certificates curl git make perl bzip2 libmpfr6 \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf /usr/lib/x86_64-linux-gnu/libmpfr.so.6 \
              /usr/lib/x86_64-linux-gnu/libmpfr.so.4
RUN curl -sL https://github.com/adtools/adtools/releases/download/20220416/adtools-utils.tar.bz2 | tar xj -C / \
    && curl -sL https://github.com/adtools/adtools/releases/download/20170213/adtools-m68k-amigaos.tar.bz2 | tar xj -C /
ENV PATH=/usr/local/amiga/bin:/opt/m68k-amigaos/bin:$PATH
