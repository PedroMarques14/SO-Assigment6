#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_admin_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_admin.sh"
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

rm -f SENSOR_PIPE COMMAND_PIPE
./home_iot &
manager_pid=$!
for attempt in {1..20}; do
    if [[ -p SENSOR_PIPE && -p COMMAND_PIPE ]]; then break; fi
    sleep 0.1
done

output=$(printf 'add_rule R1 key: TEMP_SALA min: 0 max: 25 severity: 2\nlist_rules\nremove_rule R1\nreset\nexit\n' | ./user_console 1)
if ! grep -q 'OK rule added' <<< "$output" ||
   ! grep -q 'R1 key=TEMP_SALA min=0 max=25 severity=2' <<< "$output" ||
   ! grep -q 'OK rule removed' <<< "$output" ||
   ! grep -q 'OK metrics reset' <<< "$output"; then
    printf '%s\n' "$output" >&2
    echo "Administrative commands failed" >&2
    exit 1
fi

kill -INT "$manager_pid"
wait "$manager_pid"
manager_pid=""
echo "admin smoke test: OK"
