#!/usr/bin/env bash

set -e -x

usage="Usage: $0 [-c CREDENTIAL_SOURCE] ENVIRONMENT_SPECIFICATION_FILE"

ENV_FILE=""
CREDENTIAL_SOURCE="${CREDENTIAL_SOURCE:-}"
while getopts "c:e:" opt; do
  case "${opt}" in
    c)
      CREDENTIAL_SOURCE="${OPTARG}"
      ;;
    e)
      ENV_FILE="${OPTARG}"
      ;;
    *)
      echo "${usage}"
      exit 1
  esac
done

if [[ -z "${ENV_FILE}" ]]; then
  echo "${usage}"
  exit 1
fi

CMD_OUT="cvd_load_stdout.txt"
CMD_ERR="cvd_load_stderr.txt"

function collect_logs_and_cleanup() {
  # Don't immediately exit on failure anymore
  set +e

  if [[ -n "${TEST_UNDECLARED_OUTPUTS_DIR}" ]] && [[ -d "${TEST_UNDECLARED_OUTPUTS_DIR}" ]]; then
    cp "${ENV_FILE}" "${TEST_UNDECLARED_OUTPUTS_DIR}/environment.json"
    cp "${CMD_OUT}" "${TEST_UNDECLARED_OUTPUTS_DIR}"
    cp "${CMD_ERR}" "${TEST_UNDECLARED_OUTPUTS_DIR}"

    # TODO(b/324650975): cvd doesn't print very useful information yet so file locations must be extracted this way
    home_dir="$(grep -o -E -m1 HOME="/tmp/cvd/[0-9a-zA-Z/_]+" "${CMD_ERR}" | cut -d= -f2)"
    artifacts_dir="$(grep -o -E '\-\-target_directory=/tmp/cvd/[0-9a-zA-Z/_]+' "${CMD_ERR}" | cut -d= -f2 | grep -o -E '[^"]*')"

    cp "${artifacts_dir}/fetch.log" "${TEST_UNDECLARED_OUTPUTS_DIR}"

    for instance_dir in "${home_dir}"/cuttlefish/instances/*; do
      if [[ -d "${instance_dir}" ]]; then
        instance="$(basename "${instance_dir}")"
        for log_file in "${instance_dir}"/logs/*; do
          if [[ -f "${log_file}" ]]; then
            base_name="$(basename "${log_file}")"
            cp "${log_file}" "${TEST_UNDECLARED_OUTPUTS_DIR}/${instance}_${base_name}"
          fi
        done
        if [[ -f "${instance_dir}/cuttlefish_config.json" ]]; then
          cp "${instance_dir}"/cuttlefish_config.json "${TEST_UNDECLARED_OUTPUTS_DIR}/${instance}_cuttlefish_config.json"
        else
          echo "No cuttlefish_config.json found for instance ${instance}"
        fi
      fi
    done
  fi

  # Be nice, don't leave devices behind.
  cvd reset -y
}

trap collect_logs_and_cleanup EXIT

# Make sure to run in a clean environment, without any devices running
cvd reset -y

credential_arg=""
if [[ -n "$CREDENTIAL_SOURCE" ]]; then
    credential_arg="--override=fetch.credential_source:${CREDENTIAL_SOURCE}"
fi
cvd load ${credential_arg} "${ENV_FILE}" >"${CMD_OUT}" 2>"${CMD_ERR}"
