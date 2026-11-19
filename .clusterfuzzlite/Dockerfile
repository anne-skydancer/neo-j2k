# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# ClusterFuzzLite build image: OSS-Fuzz's base builder (Clang, libFuzzer, the sanitizers), a
# CMake recent enough for the project (3.25+) and Ninja from PyPI, and the repository. The base
# image is pinned by digest and the Python packages by hash, so a moved tag cannot change what
# builds the private sources; move the pin on purpose (the digest of base-builder:v1 then).
FROM gcr.io/oss-fuzz-base/base-builder:v1@sha256:1f9cf74bc3d7029b528494542f9d9851b256dceed38d3f420989434dc5ae0760
COPY .clusterfuzzlite/requirements.txt /tmp/nj2k-requirements.txt
RUN apt-get update && apt-get install -y --no-install-recommends zip && \
    python3 -m pip install --no-cache-dir --require-hashes -r /tmp/nj2k-requirements.txt
COPY . $SRC/neo-j2k
WORKDIR $SRC/neo-j2k
COPY .clusterfuzzlite/build.sh $SRC/
