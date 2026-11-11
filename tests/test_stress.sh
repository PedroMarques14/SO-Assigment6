#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_stress_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_stress.sh"
        test_result=$?
        rm -rf "$temporary_root"
        exit "$test_result"
    fi
    rm -f "$fifo_probe"
fi

manager_pid=""
sensor_pids=()
cleanup() {
    for sensor_pid in "${sensor_pids[@]}"; do
        kill -TERM "$sensor_pid" 2>/dev/null || true
        wait "$sensor_pid" 2>/dev/null || true
    done
    if [[ -n "$manager_pid" ]] && kill -0 "$manager_pid" 2>/dev/null; then
        kill -TERM "$manager_pid" 2>/dev/null || true
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -f SENSOR_PIPE COMMAND_PIPE
}
trap cleanup EXIT

rm -f SENSOR_PIPE COMMAND_PIPE
./home_iot &
manager_pid=$!
for attempt in {1..20}; do
    if [[ -p SENSOR_PIPE && -p COMMAND_PIPE ]]; then break; fi
    sleep 0.1
done

for sensor_index in 1 2 3 4 5; do
    ./sensor "SENSOR_$sensor_index" 20 TEMP_STRESS 1 100 &
    sensor_pids+=("$!")
done
sleep 1
for sensor_pid in "${sensor_pids[@]}"; do
    kill -INT "$sensor_pid" 2>/dev/null || true
done
for sensor_pid in "${sensor_pids[@]}"; do
    wait "$sensor_pid" 2>/dev/null || true
done
sensor_pids=()

output=$(printf 'stats TEMP_STRESS\nexit\n' | ./user_console 1)
if ! grep -Eq 'TEMP_STRESS .* count=[1-9][0-9]*' <<< "$output"; then
    printf '%s\n' "$output" >&2
    echo "Concurrent sensor readings were not aggregated" >&2
    exit 1
fi

kill -INT "$manager_pid"
wait "$manager_pid"
manager_pid=""
echo "stress smoke test: OK"
