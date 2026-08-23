#!/usr/bin/env bash
set -euo pipefail

app_home="${OPENISAC_HOME:-/opt/openisac}"
run_dir="${OPENISAC_RUN_DIR:-/work/build}"

usage() {
    cat <<'EOF'
Usage:
  openisac-run ChannelSimulator [sim|sim_duplex|sim_ertm|sim_resourcemap]
  openisac-run BS [sim|sim_duplex|sim_ertm|sim_resourcemap] [extra args...]
  openisac-run UE [sim|sim_duplex|sim_ertm|sim_resourcemap] [extra args...]
  openisac-run bash

Environment:
  OPENISAC_RUN_DIR=/work/build       Directory used as the runtime cwd.
  OPENISAC_CONFIG=/path/to/file.yaml Copy this config instead of a preset.
  OPENISAC_REFRESH_CONFIG=1          Overwrite existing BS.yaml/UE.yaml.
EOF
}

if [[ $# -eq 0 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

target="$1"
shift || true

case "$target" in
    simulator|ChannelSimulator)
        binary="${app_home}/bin/ChannelSimulator"
        runtime_yaml="BS.yaml"
        preset_prefix="BS"
        ;;
    bs|BS)
        binary="${app_home}/bin/BS"
        runtime_yaml="BS.yaml"
        preset_prefix="BS"
        ;;
    ue|UE)
        binary="${app_home}/bin/UE"
        runtime_yaml="UE.yaml"
        preset_prefix="UE"
        ;;
    *)
        exec "$target" "$@"
        ;;
esac

profile="${1:-${OPENISAC_PROFILE:-sim}}"
consume_profile=0
case "$(printf '%s' "$profile" | tr '[:upper:]' '[:lower:]')" in
    sim) profile_suffix="Sim"; consume_profile=1 ;;
    sim_duplex) profile_suffix="Sim_Duplex"; consume_profile=1 ;;
    sim_ertm) profile_suffix="Sim_eRTM"; consume_profile=1 ;;
    sim_resourcemap) profile_suffix="Sim_ResourceMap"; consume_profile=1 ;;
    *) profile="${OPENISAC_PROFILE:-sim}"; profile_suffix="Sim" ;;
esac
if [[ $consume_profile -eq 1 && $# -gt 0 ]]; then
    shift || true
fi

default_config="${app_home}/config/${preset_prefix}_${profile_suffix}.yaml"
source_config="${OPENISAC_CONFIG:-$default_config}"

if [[ ! -x "$binary" ]]; then
    echo "OpenISAC binary not found or not executable: $binary" >&2
    exit 127
fi

if [[ ! -f "$source_config" ]]; then
    echo "Config file not found: $source_config" >&2
    exit 2
fi

mkdir -p "$run_dir"
cd "$run_dir"

if [[ ! -f "$runtime_yaml" || "${OPENISAC_REFRESH_CONFIG:-0}" == "1" ]]; then
    cp "$source_config" "$runtime_yaml"
fi

echo "OpenISAC: running $(basename "$binary") with $PWD/$runtime_yaml"
exec "$binary" "$@"
