# Design Diary — RemoteOps (IT24100068)
## Sat 3 Oct 2026
**Done:** personalised values, TCP listener, thread-per-client, line framing, AUTH, QUIT, logging.

**Decisions**
- **Concurrency: one POSIX thread per client** (pthread_create + pthread_detach). Each session's code reads top-to-bottom like a simple blocking client, which is easy to reason about. select() would need a state machine per client, especially for PUT/GET mid-transfer. Cost: one thread per client, fine for the required 5+ connections.
- **Framing:** each session has its own 8 KB buffer. read_line() returns a line only once '\n' arrives and keeps leftover bytes, because TCP is a byte stream. Leftover bytes will matter for PUT (file bytes may already be in the buffer).
- **SID tag:** all replies go through send_response(), which appends " SID:8600" — impossible to forget the tag.
- **Logging:** one mutex guards the log file so lines from different threads never mix. AUTH tokens are masked in the log.
- **SIGPIPE ignored + MSG_NOSIGNAL:** sending to a crashed client must not kill the Agent.
- Error codes 003, 006–009 chosen for cases the brief leaves open.

**Tested:** command before AUTH, bad token, correct token, QUIT, port verified with ss.

**Obstacles:** files downloaded from the browser did not reach CentOS; recreated them with nano. First paste attempt into the terminal broke because the heredoc line was lost; switched to nano.

## Next
- Sun 4 Oct: SYSINFO (/proc), LISTPROC, EXEC whitelist, Controller.

## Sat 3 Oct 2026 (evening) — Day 2
**Done:** SYSINFO, LISTPROC, EXEC whitelist in the Agent; interactive Controller.

**Decisions**
- SYSINFO reads /proc: load = 1-min load average (/proc/loadavg), memory used = MemTotal − MemAvailable (/proc/meminfo), uptime from /proc/uptime. Real values, not simulated. get_sysinfo() is separate so the UDP monitor can reuse it.
- LISTPROC uses popen("ps -eo pid=,comm=") and sends pid:name pairs separated by commas. Spaces/commas in names become '_'. Reply buffer is 32 KB because my desktop has 270+ processes.
- EXEC: the client sends only a NAME; the Agent looks it up in a fixed table of commands. User text never reaches the shell. Exact, case-sensitive match ("EXEC date" is rejected). Extra words give ERR 006 BAD_SYNTAX.
- One-line rule: multi-line output (df -h) is joined with " | " so framing isn't broken.
- HOSTNAME uses uname -n because the hostname package isn't always installed on minimal CentOS.
- Controller reuses the same read_line() framing, prints the raw reply, adds a readable line for SYSINFO, shows the first 30 processes as a table, and warns if SID:8600 is missing.
- Long replies are shortened to 200 chars in the log so LISTPROC doesn't flood it.
- Reduced code comments to the key ones; I explain the rest in the report.

**Tested:** all 5 EXEC names; EXEC rm and EXEC date rejected; LISTPROC returned 273 processes on my desktop; agent log shows every RX/TX.

## Next
- Day 3: PUT / GET with exact byte counts, md5sum check, throughput extension.
