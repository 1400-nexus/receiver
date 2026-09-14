# Uniflow receiver — Linux build and run environment.
#
# WHY THIS EXISTS
# ---------------
# The receiver needs two things macOS does not have:
#
#   AF_UNIX SOCK_SEQPACKET   Darwin supports only STREAM and DGRAM for AF_UNIX;
#                            socket() fails at runtime. The IPC contract in the
#                            design brief cannot be implemented natively on a Mac.
#   POSIX shm + ISA-L        Reed-Solomon decode via Intel ISA-L and the
#                            lock-free SHM segment the three receivers share.
#
# It is also where protobuf gets pinned. Everyone who builds in this image has
# the same protoc and the same libprotobuf, so the version-mismatch class of
# problem cannot happen again.
#
# NOTE ON PROTOBUF VERSIONS: this image ships Ubuntu's 3.21.12. That is fine,
# and not a compromise — the protobuf WIRE FORMAT is stable across versions,
# so a receiver built here interoperates perfectly with a sender built
# anywhere else. Only the generated C++ is version-specific, and CMake now
# generates that locally from the .proto sources.
#
# Mirrors sender/Dockerfile: same base, same packages, so either image can
# build either C++ service.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        git \
        gdb \
        protobuf-compiler \
        libprotobuf-dev \
        libisal-dev \
        iproute2 \
        tcpdump \
        iputils-ping \
        python3 \
        python3-protobuf \
        python3-numpy \
        python3-pip \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# blake3 has no Debian package. proto_hash is BLAKE3 over the schema files, and
# CMake has no BLAKE3 of its own, so the build shells out to Python for it.
RUN pip install --no-cache-dir --break-system-packages blake3

WORKDIR /workspace

CMD ["/bin/bash"]
