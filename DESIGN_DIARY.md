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
