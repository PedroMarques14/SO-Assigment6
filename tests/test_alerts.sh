#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_alert_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_alerts.sh"
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

rule_output=$(printf 'add_rule R1 key: SMOKE_COZ min: 0 max: 10 severity: 1 action: ALARME:ON:10\nexit\n' | ./user_console 1)
if ! grep -q 'OK rule added' <<< "$rule_output"; then
    echo "Rule creation failed" >&2
    exit 1
fi
printf 'COZ_F1#SMOKE_COZ#80\n' > SENSOR_PIPE
sleep 0.5
alert_output=$(printf 'alerts\nstatus\nexit\n' | ./user_console 1)
if ! grep -q 'AL_001 rule=R1 value=80 state=0' <<< "$alert_output"; then
    echo "Alert was not recorded" >&2
    exit 1
fi
if ! grep -q 'ALARME status=1' <<< "$alert_output"; then
    echo "Automatic actuator action was not executed" >&2
    exit 1
fi

kill -INT "$manager_pid"
wait "$manager_pid"
manager_pid=""
echo "alerts smoke test: OK"
