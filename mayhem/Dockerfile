# syntax=docker/dockerfile:1.7
#
# scs commit-image (small form): FROM the org base, a DEV image that retains full source
# + toolchain (fixes the slim-final "nodata" failure). Builds via mayhem/build.sh.
FROM ghcr.io/mayhemheroes/base:20260621

ARG COMMIT
ARG REPO_URL
# SOURCE_DATE_EPOCH stays an ARG (available to build.sh); do NOT bake it as an ENV — an
# empty value breaks clang both here and for any in-image rebuild.
ARG SOURCE_DATE_EPOCH
# Sanitizer override (default = base ASan+UBSan): --build-arg SANITIZER_FLAGS="-fsanitize=address ..."
ARG SANITIZER_FLAGS
# Build parallelism (optional; build.sh falls back to nproc): --build-arg MAYHEM_JOBS=8
ARG MAYHEM_JOBS=

USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

# Seed corpus: seeds are NOT baked into the image. Mayhem resolves the Mayhemfile `testsuite:`
# file:// entries against the *checkout on the CI runner* (package dir = `.`), and accumulates the
# server-side corpus (testsuite.tar) on top. The git-tracked seeds under mayhem/testsuite/ are all
# that's needed; an image /testsuite is never read. See issue #158 and the seed-corpus writeup.
RUN bash mayhem/build.sh

# Run the project's functional tests at build time IF the repo ships them (mayhem/test.sh is
# optional — omit it only when the repo has no tests meaningful for RL). A failing suite fails the
# image build, so the commit image is guaranteed to build AND pass its own tests.
RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh — repo ships no functional tests"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
