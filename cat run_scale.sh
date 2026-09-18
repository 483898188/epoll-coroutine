

#!/bin/bash
# One scale point: establish N connections, capture ss/nstat, profile the
# request rounds with perf (window tightened to exactly the rounds).
# Usage: run_scale.sh <N> [batch]
set -u
N=$1
BATCH=${2:-10}
TAG="n$N"
cd /home/hxl/epoll-coroutine || exit 1

echo "########## scale point N=$N (batch=$BATCH) ##########"
pkill -x test_server 2>/dev/null; sleep 0.5
rm -f /tmp/scale_ready_$TAG /tmp/scale_go_$TAG /tmp/scale_done_$TAG
rm -f /tmp/server_$TAG.log /tmp/perf_$TAG.txt /tmp/ss_idle_$TAG.txt \
      /tmp/client_$TAG.txt /tmp/nstat_before_$TAG.txt /tmp/nstat_after_$TAG.txt \
      /tmp/scale_result_$TAG.txt

nohup bash -c "ulimit -n 1048576; exec ./test_server" > /tmp/server_$TAG.log 2>&1 &
sleep 1.5
SPID=$(pgrep -x test_server)
[ -z "$SPID" ] && { echo "SERVER FAILED TO START"; exit 1; }
echo "server pid=$SPID"

nstat -az > /tmp/nstat_before_$TAG.txt 2>/dev/null
NSTAT_EST_BEFORE=$(nstat -az 2>/dev/null | awk '/TcpExtListenOverflows/{print $2}')

ulimit -n 1048576
python3 -u /tmp/scale_client.py "$N" 8080 "$BATCH" "$TAG" > /tmp/client_$TAG.txt 2>&1 &
CPID=$!

for _ in $(seq 1 900); do [ -f /tmp/scale_ready_$TAG ] && break; sleep 0.2; done
if [ ! -f /tmp/scale_ready_$TAG ]; then echo "CLIENT NEVER READY"; cat /tmp/client_$TAG.txt; kill $CPID; exit 1; fi

# --- steady-state snapshot ---
{
  echo "### ss -lnt '( sport = :8080 )'   [Send-Q = listen backlog, Recv-Q = accept queue]"
  ss -lnt '( sport = :8080 )'
  echo
  echo "### tcp states on :8080"
  ss -tan '( sport = :8080 )' | awk 'NR>1 {print $1}' | sort | uniq -c
  echo
  echo "### ss -s"
  ss -s | head -3
  echo
  echo "### fds / rss / threads"
  echo "fds=$(ls /proc/$SPID/fd 2>/dev/null | wc -l)"
  echo "threads=$(ls /proc/$SPID/task 2>/dev/null | wc -l)"
  grep -E "VmRSS" /proc/$SPID/status
} > /tmp/ss_idle_$TAG.txt 2>&1

# --- perf stat: fixed window covering the request rounds ---
# SIGINT does not make this perf version flush its report, so instead run it
# for a bounded window and let it exit on its own (that does write the report).
# The client holds its connections open for ~25s after the rounds, so this
# window ends during idle time and teardown syscalls are excluded.
sudo -n perf stat -p "$SPID" \
  -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  -e syscalls:sys_enter_epoll_pwait,syscalls:sys_enter_epoll_ctl,syscalls:sys_enter_accept,syscalls:sys_enter_recvfrom,syscalls:sys_enter_sendto \
  -o /tmp/perf_$TAG.txt -- sleep 12 &
PERFPID=$!
sleep 2.0
echo "perf attached (pid $PERFPID), window 12s"

touch /tmp/scale_go_$TAG
for _ in $(seq 1 1500); do [ -f /tmp/scale_done_$TAG ] && break; sleep 0.2; done
wait "$PERFPID" 2>/dev/null
sleep 0.3

NSTAT_EST_AFTER=$(nstat -az 2>/dev/null | awk '/TcpExtListenOverflows/{print $2}')
nstat -az > /tmp/nstat_after_$TAG.txt 2>/dev/null
wait "$CPID" 2>/dev/null

echo "--- client ---"
cat /tmp/client_$TAG.txt
echo "--- ss (steady state) ---"
cat /tmp/ss_idle_$TAG.txt
echo "--- perf ---"
cat /tmp/perf_$TAG.txt
echo "--- ListenOverflows delta: $((NSTAT_EST_AFTER - NSTAT_EST_BEFORE)) ---"
pkill -x test_server 2>/dev/null
echo



