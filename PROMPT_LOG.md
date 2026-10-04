# AI Prompt Log — RemoteOps (IT24100068)

| # | Date | Tool | Prompt (summary) | AI output | How I used / changed it |
|---|---|---|---|---|---|
| 1 | 2026-10-03 | Claude | Asked for a simple overview of the assignment brief and how to approach it | Plain-language explanation, marking breakdown, tricky parts (framing, byte-exact transfer, SIGPIPE), 5-day plan | Used as my study plan; chose threads as concurrency model |
| 2 | 2026-10-03 | Claude | Gave my reg. no. IT24100068 and asked to start Day 1 | Calculated personalised values; starter agent_068.c (listener, threads, framing, AUTH, logging), Makefile_068, README | Checked each value by hand against §2.4; built and tested it myself with ss and nc |
| 3 | 2026-10-03 | Claude | Files would not download to CentOS; asked for a way to create them | nano paste method and printf command for the Makefile (keeps Tab characters) | Used it; learned that Makefiles need real Tabs |
| 4 | 2026-10-03 | Claude | Asked to continue with Day 2 (SYSINFO, LISTPROC, EXEC, Controller) | Agent handlers reading /proc and ps, fixed EXEC whitelist table, interactive controller_068.c, updated Makefile | Tested every command on my machine, including rejected ones. AI first used thread-local buffers; changed to plain local arrays because they are simpler to explain |
| 5 | 2026-10-03 | Claude | Asked for fewer comments in the code | Same code with comments reduced to a few key ones | Logic unchanged; rebuilt with no warnings and re-tested all commands |
| 6 | 2026-10-04 | Claude | Asked for Day 3: PUT/GET file transfer and throughput | PUT/GET in agent and controller: exact byte counting, leftover-buffer handling, temp file + rename, filename checks, 10 MB limit, bytes/s reporting | Tested with 39 B text, 5 MB binary and 11 MB (rejected) files; md5sum matched. Small files took ~40 ms; added TCP_NODELAY (Lecture 05) which fixed it |

