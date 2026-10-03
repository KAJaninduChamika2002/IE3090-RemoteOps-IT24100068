# RemoteOps — Remote System Monitoring & Management Tool (IE3090)

**Student registration number:** IT24100068

## Personalised values

| Item | Calculation | Value |
|---|---|---|
| Agent TCP port | 7000 + 2410 (first four digits of 24100068) | **9410** |
| Source files | last three digits = 068 | `agent_068.c`, `controller_068.c`, `Makefile_068` |
| Session ID tag | last four digits 0068, reversed | **SID:8600** |
| Auth token | "OPS-" + last four digits | **OPS-0068** |
| Log file | remoteops_ + full reg. no. | `remoteops_IT24100068.log` |
| Storage path | ./agentfiles/ + full reg. no. | `./agentfiles/IT24100068/` |
| Submission archive | IE3090_ + full reg. no. | `IE3090_IT24100068.zip` |

## Build (CentOS / any Linux with gcc)

    make -f Makefile_068
    make -f Makefile_068 clean

## Run

    ./agent_068                   # listens on TCP 9410
    ss -tlnp | grep 9410          # verify the listening port

Quick manual test (until the Controller is ready):

    nc localhost 9410
    AUTH OPS-0068
    QUIT

## Status

- [x] TCP listener on port 9410, thread-per-client concurrency
- [x] Line framing (partial lines, multiple lines per recv())
- [x] AUTH gate, QUIT, SID tag on every response
- [x] Timestamped, thread-safe logging; ungraceful disconnect handling
- [ ] SYSINFO, LISTPROC, EXEC
- [ ] PUT / GET
- [ ] MONITOR START / STOP (UDP)
- [ ] Controller
- [ ] Optional extension: transfer throughput

## Error codes

| Code | Reason |
|---|---|
| 001 | AUTH_FAILED |
| 002 | COMMAND_NOT_ALLOWED |
| 003 | NOT_AUTHENTICATED |
| 004 | FILE_TOO_LARGE |
| 005 | FILE_NOT_FOUND |
| 006 | BAD_SYNTAX |
| 007 | ALREADY_AUTHENTICATED |
| 008 | UNKNOWN_COMMAND |
| 009 | LINE_TOO_LONG |
