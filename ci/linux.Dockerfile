# Mirrors the GitHub Actions runner closely enough to test tools/setup +
# tools/selftest on Linux locally:
#   docker build -t agk-ci -f ci/linux.Dockerfile ci
#   docker run --rm -v /var/run/docker.sock:/var/run/docker.sock \
#     -v "$PWD:$PWD" -w "$PWD" agk-ci sh -c 'tools/setup && tools/selftest a500-aros'
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake git python3 ca-certificates docker.io \
    && rm -rf /var/lib/apt/lists/*
RUN git config --global --add safe.directory '*'
