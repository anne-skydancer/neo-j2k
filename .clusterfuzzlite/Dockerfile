# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# ClusterFuzzLite build image: OSS-Fuzz's base builder (Clang, libFuzzer, the sanitizers), a
# CMake recent enough for the project (3.25+) and Ninja from PyPI, and the repository.
FROM gcr.io/oss-fuzz-base/base-builder:v1
RUN apt-get update && apt-get install -y --no-install-recommends zip && \
    python3 -m pip install --no-cache-dir cmake==3.31.6 ninja==1.11.1.4
COPY . $SRC/neo-j2k
WORKDIR $SRC/neo-j2k
COPY .clusterfuzzlite/build.sh $SRC/
