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

## Sun 4 Oct 2026 — Day 3
**Done:** PUT and GET in Agent and Controller, plus throughput extension.

**Decisions**
- PUT reads exactly <filesize> bytes. Bytes that arrived in the same recv() as the PUT line are already in the session buffer, so read_bytes() uses them first before calling recv().
- Upload goes to a temp file (mkstemp) and is renamed only when complete, so a client that disconnects mid-upload never leaves a half file.
- Max file size 10 MB (my assumption). Too large → bytes are read and discarded, then ERR 004. Otherwise those bytes would be read as commands.
- Filename must have no '/', no '\', not start with '.', printable chars only → stops ../ path traversal. Added ERR 010 INVALID_FILENAME and ERR 011 STORAGE_ERROR.
- GET sends "OK FILE_SEND <name> <size>" then streams in 64 KB chunks. Controller saves to ./downloads/ so the original isn't overwritten.
- Throughput (extension): clock_gettime(CLOCK_MONOTONIC); Controller prints bytes/s, Agent logs it. 5 MB: ~64 MB/s up, ~55 MB/s down on localhost.
- Small files took ~40 ms because of Nagle's algorithm + delayed ACK; set TCP_NODELAY on both sockets.

**Tested:** 39 B text, 5 MB binary, 11 MB (ERR 004), missing file (ERR 005), ../ name (ERR 010), SYSINFO after errors still works. md5sum identical for both transfers.

## Next
- Day 4: MONITOR START/STOP over UDP, disconnect tests, 5+ clients.

## Mon 5 Oct 2026 — Day 4
**Done:** MONITOR START/STOP over UDP; tested concurrency and disconnects.

**Decisions**
- One monitor thread per session, sending "SYSINFO <load> <mem> <uptime> SID:8600" to the client's TCP IP on the requested UDP port. Interval: 5 seconds.
- Thread waits with pthread_cond_timedwait instead of sleep(5), so MONITOR STOP signals the condition and the thread exits immediately.
- stop_monitor() is called on MONITOR STOP, QUIT and in client_thread cleanup, so a crashed client never leaves a stream running.
- Reuses get_sysinfo() from Day 2.
- Errors: 012 ALREADY_MONITORING, 013 NOT_MONITORING, 014 MONITOR_FAILED; bad port gives 006.
- Controller binds the UDP port before sending MONITOR START and prints datagrams from a background thread with a counter and timestamp, so commands can still be typed while monitoring.
- Observed: first UDP datagram can arrive before the TCP "OK MONITOR_STARTED" reply, because they are separate channels. Harmless.

**Tested:** datagrams exactly 5 s apart; SYSINFO over TCP works during monitoring; none after STOP or QUIT; kill -9 of controller -> agent logged "ungraceful: peer closed" and "stopped after 5 datagrams" in the same second, then served a new client; 5 controllers at once (ss shows 5 ESTABLISHED, log shows interleaved lines from 5 threads).

## Next
- Tue 6 Oct: code screenshots, implementation report, reflection.
- Wed 7 Oct: ZIP, make repo visible to examiner, submit early.
