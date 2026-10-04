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

```bash
make -f Makefile_068          # builds agent_068 and controller_068
make -f Makefile_068 clean
```

## Run

```bash
./agent_068                   # listens on TCP 9410
ss -tlnp | grep 9410          # verify the listening port
```

In a second terminal:

```bash
./controller_068              # connects to 127.0.0.1:9410
./controller_068 <agent_ip>   # or another machine
```

Example session:

```
remoteops> AUTH OPS-0068
<< OK AUTHENTICATED SID:8600
remoteops> SYSINFO
<< OK SYSINFO 0.33 1499 7228 SID:8600
remoteops> EXEC HOSTNAME
<< OK EXEC_RESULT desktop-qrr84pe SID:8600
remoteops> QUIT
<< OK BYE SID:8600
```

## EXEC whitelist

| Name | Fixed command run by the Agent |
|---|---|
| DATE | `date` |
| UPTIME | `uptime -p` |
| DISKFREE | `df -h /` |
| HOSTNAME | `uname -n` |
| WHOAMI | `whoami` |

The Controller only sends a name; the command string is fixed in the Agent, so user text never reaches the shell. Multi-line output is joined with ` | ` to keep one line per response.

## Status

- [x] TCP listener on port 9410, thread-per-client concurrency
- [x] Line framing (partial lines, multiple lines per `recv()`)
- [x] AUTH gate, QUIT, SID tag on every response
- [x] Timestamped, thread-safe logging; ungraceful disconnect handling
- [x] SYSINFO (/proc), LISTPROC (ps), EXEC (fixed whitelist)
- [x] PUT / GET (exact byte count, 10 MB limit, temp file + rename)
- [ ] MONITOR START / STOP (UDP)
- [x] Controller (interactive, same framing, SID check)
- [x] Optional extension: transfer throughput (bytes/s)

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
| 010 | INVALID_FILENAME |
| 011 | STORAGE_ERROR |
