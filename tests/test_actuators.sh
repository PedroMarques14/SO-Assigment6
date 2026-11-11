#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_actuator_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_actuators.sh"
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

first_output=$(printf 'actuator ILUMINACAO ON duration: 1\nexit\n' | ./user_console 1)
if ! grep -q 'OK ILUMINACAO ON' <<< "$first_output"; then
    echo "Low-power activation failed" >&2
    exit 1
fi
sleep 2.5
second_output=$(printf 'status\nexit\n' | ./user_console 1)
if ! grep -q 'ILUMINACAO status=0' <<< "$second_output"; then
    echo "Actuator did not leave cooldown" >&2
    exit 1
fi

energy_output=$(printf 'actuator AQUECEDOR ON duration: 2\nactuator AR_CONDICIONADO ON duration: 2\nactuator FORNO ON duration: 2\nexit\n' | ./user_console 1)
if ! grep -q 'OK AQUECEDOR ON' <<< "$energy_output" ||
   ! grep -q 'OK AR_CONDICIONADO ON' <<< "$energy_output" ||
   ! grep -q 'energy budget exhausted' <<< "$energy_output"; then
    echo "Energy budget behavior failed" >&2
    exit 1
fi

kill -INT "$manager_pid"
wait "$manager_pid"
manager_pid=""
echo "actuator smoke test: OK"
