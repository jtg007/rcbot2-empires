# Sourced by the dev scripts: run builds and test servers on a few CPUs with low
# priority so a game client on the other cores is not slowed down. Children
# (g++, srcds) inherit the affinity.
#   RCBOT_CPUS="4,5,10,11"  CPUs for builds/tests (default: the last two cores and
#                           their SMT siblings on a 6-core/12-thread CPU); empty = all
#   RCBOT_NICE=10           nice level
if [[ -z "${RCBOT_CPU_PINNED:-}" ]]; then
  export RCBOT_CPU_PINNED=1
  _cpus="${RCBOT_CPUS-4,5,10,11}"
  if [[ -n "$_cpus" ]] && command -v taskset >/dev/null && taskset -c "$_cpus" true 2>/dev/null; then
    taskset -cp "$_cpus" $$ >/dev/null
  fi
  renice -n "${RCBOT_NICE:-10}" -p $$ >/dev/null 2>&1 || true
  unset _cpus
fi
