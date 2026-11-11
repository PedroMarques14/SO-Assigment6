#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_valgrind_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_valgrind.sh"
        test_result=$?
        rm -rf "$temporary_root"
        exit "$test_result"
    fi
    rm -f "$fifo_probe"
fi

rm -f SENSOR_PIPE COMMAND_PIPE valgrind-*.log
valgrind_tool="${VALGRIND_TOOL:-memcheck}"
valgrind_options=(--tool="$valgrind_tool" --trace-children=yes -s)
if [[ "$valgrind_tool" == "memcheck" ]]; then
    valgrind_options+=(--leak-check=full --show-leak-kinds=all)
fi
valgrind "${valgrind_options[@]}" \
    --error-exitcode=42 --log-file=valgrind-%p.log ./home_iot &
manager_pid=$!
cleanup() {
    if kill -0 "$manager_pid" 2>/dev/null; then
        kill -TERM "$manager_pid" 2>/dev/null || true
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -f SENSOR_PIPE COMMAND_PIPE
}
trap cleanup EXIT

for attempt in {1..30}; do
    if [[ -p SENSOR_PIPE && -p COMMAND_PIPE ]]; then break; fi
    sleep 0.1
done
if [[ ! -p SENSOR_PIPE || ! -p COMMAND_PIPE ]]; then
    echo "Valgrind manager did not create FIFOs" >&2
    exit 1
fi

kill -INT "$manager_pid"
wait "$manager_pid" || true

for report in valgrind-*.log; do
    [[ -f "$report" ]] || continue
    if ! grep -q 'ERROR SUMMARY: 0 errors' "$report"; then
        echo "Valgrind errors in $report" >&2
        cat "$report" >&2
        exit 1
    fi
done

echo "valgrind smoke test: OK"
