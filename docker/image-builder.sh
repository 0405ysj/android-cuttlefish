#!/usr/bin/env bash

# This shell script exists for building docker image.
# Docker image includes HO(Host Orchestrator) inside,
# so it could execute CF instance with API in HO.

script_location=`realpath -s $(dirname ${BASH_SOURCE[0]})`
android_cuttlefish_root_dir=$(realpath -s $script_location/..)

usage() {
  echo "usage: $0 [-t <tag>] [-b <build-option>]"
  echo "  -t: name or name:tag of docker image (default cuttlefish-orchestration)"
  echo "  -b: build option of docker image (default local-source)"
  echo "      local-source: build host packages from the local source"
  echo "      apt-install : download and install host packages from apt"
}

name=cuttlefish-orchestration
target=runner-with-local-source
while getopts ":hb:t:" opt; do
  case "${opt}" in
    h)
      usage
      exit 0
      ;;
    b)
      target="runner-with-${OPTARG}"
      ;;
    t)
      name="${OPTARG}"
      ;;
    \?)
      echo "Invalid option: ${OPTARG}" >&2
      usage
      exit 1
      ;;
    :)
      echo "Invalid option: ${OPTARG} requires an argument" >&2
      usage
      exit 1
      ;;
  esac
done

# Build docker image
pushd $android_cuttlefish_root_dir
DOCKER_BUILDKIT=1 docker build \
    --force-rm \
    --no-cache \
    -f docker/Dockerfile \
    -t $name \
    --target $target \
    .
popd
