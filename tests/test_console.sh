#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_console_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_console.sh"
        test_result=$?
        rm -rf "$temporary_root"
        exit "$test_result"
    fi
    rm -f "$fifo_probe"
fi

manager_pid=""
cleanup() {
    if [[ -n "$manager_pid" ]] && kill -0 "$manager_pid" 2>/dev/null; then
        kill -TERM "$manager_pid" 2>/dev/null || true
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -f SENSOR_PIPE COMMAND_PIPE
}
trap cleanup EXIT

rm -f SENSOR_PIPE COMMAND_PIPE logs/home_iot_log.txt
./home_iot &
manager_pid=$!
for attempt in {1..20}; do
    if [[ -p SENSOR_PIPE && -p COMMAND_PIPE ]]; then break; fi
    sleep 0.1
done

if [[ ! -p SENSOR_PIPE || ! -p COMMAND_PIPE ]]; then
    echo "FIFOs were not created" >&2
    exit 1
fi

printf 'SALA_T1#TEMP_SALA#22\n' > SENSOR_PIPE
sleep 0.2
console_output=$(printf 'stats TEMP_SALA\nexit\n' | ./user_console 1)
printf '%s\n' "$console_output"

if ! grep -q 'TEMP_SALA min=22 max=22 avg=22.0 count=1' <<< "$console_output"; then
    echo "Console did not receive expected statistics" >&2
    exit 1
fi

kill -INT "$manager_pid"
wait "$manager_pid"
manager_pid=""
echo "console smoke test: OK"
