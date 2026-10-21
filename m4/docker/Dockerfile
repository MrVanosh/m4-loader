# Build container for the M4 loader (rev 11 .. rev 13, and the Linux-side builds that follow).
#
# Reproduces the toolchain of the container `m4-r41-build` (image m4-r41-build-keep:2026-10-15,
# sha256:997b5310..., Fedora 44 aarch64) that built rev 12 (7bf9b881...) and rev 13 (c8068164...).
#
# From the repo root (plain docker on aarch64 Linux; no OrbStack, no special mounts; `sudo` if your user is not in the docker group):
#   docker build -t m4-loader-build -f m4/docker/Dockerfile m4/docker
#   docker run --rm -v "$PWD":/src -v "$HOME/m4-inputs":/inputs:ro -v "$HOME/m4-private":/private:ro m4-loader-build \
#       bash -c 'cd /src && INPUTS=/inputs PRIVATE=/private WORK=/tmp/w m4/check.sh'
# (the container needs no mount at all: copying the checkout and the two input directories in with `docker cp` works the same.)
#
# What decides the bytes of m1n1.bin: gcc (+cpp/libgcc), binutils (ld, objcopy, as) and the prebuilt
# librust.a. They are pinned by exact NVR below. Everything else (python, dtc, qemu, e2fsprogs, glibc)
# only runs the tests; its NVRs are recorded in rpm-qa-original.txt for reference but are not pinned,
# because Fedora's updates repository drops superseded builds (glibc-devel, git, rust, python3 were
# already replaced when this file was written, 2026-10-21).
#
# Base: Docker Hub fedora:44 as of 2026-10-21. The original image was built about 2026-09-29 on a
# fedora:44 layer (diff_id sha256:50cb43dc...) that no tag serves any more, so the exact original base
# digest could not be recovered. The base only supplies glibc/coreutils/rpm; the pinned toolchain
# below is what counts.
FROM fedora:44@sha256:43b29f65a41eb9c35e1cd5323e3bdf3b655c2357a9f4f1ff2f9c2798e5045d80

# Toolchain that determines the output bytes (exact NVRs, available in the fc44 repos on 2026-10-21).
ARG GCC=16.2.1-2.fc44
ARG BINUTILS=2.46.1-1.fc44
RUN dnf -y install --setopt=install_weak_deps=False \
        gcc-${GCC} cpp-${GCC} libgcc-${GCC} libasan-${GCC} libubsan-${GCC} \
        binutils-${BINUTILS} \
        make-4.4.1-12.fc44 \
    && dnf clean all

# Test tools. dtc 1.8.1-6, e2fsprogs 1.47.3-4 and qemu 10.2.2-1 are pinned (they were current).
RUN dnf -y install --setopt=install_weak_deps=False \
        glibc-devel python3 \
        dtc-1.8.1-6.fc44 e2fsprogs-1.47.3-4.fc44 qemu-system-aarch64-core-10.2.2-1.fc44 \
        git patch tar gzip xz diffutils findutils which file hostname \
    && dnf clean all

# Only needed to rebuild librust.a (the Makefile never rebuilds it: `-o build/librust.a`).
# The original used rustc/cargo 1.98.1; Fedora 44 now serves 1.99.0 -- a rebuilt librust.a is not
# expected to be byte-identical (see inputs.md of the repo).
RUN dnf -y install --setopt=install_weak_deps=False rust cargo rust-src && dnf clean all

# The Makefile on aarch64 uses plain gcc/ld; loader-layout.py and the test scripts call the
# aarch64-linux-gnu- prefixed names, as the original container did.
RUN for tool in gcc cc as ld objcopy objdump readelf nm ar size; do \
        ln -s /usr/bin/$tool /usr/local/bin/aarch64-linux-gnu-$tool; done

ENV CARGO_HOME=/work/.cargo LANG=C.UTF-8
WORKDIR /work
CMD ["sleep", "infinity"]
