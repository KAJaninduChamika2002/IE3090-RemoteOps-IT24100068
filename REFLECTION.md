# Structured Reflection — RemoteOps (IT24100068)

## Which AI tools did I use, and at which stages?

The AI tool called Claude was used throughout Part 1. First, Claude was asked to provide the 
clarification of the brief and create an action plan to complete it in four days. During the 
development stage, Claude was used to create the code base for each of the features mentioned 
(threaded Agent and framing on the first day, system commands and Controller on the second 
day, PUT/GET on the third day, and UDP monitoring on the fourth day). 

## What did the AI do well, and where did it get things wrong?

The AI did very well in providing explanation on concepts that had just been covered in class, 
including the need for TCP to use explicit framing, the possibility for send() to send fewer 
characters than needed and for SIGPIPE to be ignored. However, the AI made mistakes. The 
AI provided instructions that required the files to download in my CentOS VM, which didn't 
work out as planned hence I had to write my own instructions in nano. It noted that the Agent 
file will contain 622 lines, but actually contained 621. The result provided by AI for the 
command EXEC DATE; rm -rf / is incorrect (it is actually ERR 006, not ERR 002). Also, the 
AI's test case for client crash was at first glance a bug, but it turned out to be a mistake in the 
AI's test file.

## What did I change, add or reject, and why?

The variables that were declared with the thread keyword in my first Day 2 code were changed 
to normal local arrays because I was uncertain about the syntax of thread. I asked for less 
comments and have added explanation to this report instead. With the knowledge gained on 
the Nagle’s algorithm from Lecture 05, I have observed the 40ms delay in transferring small 
files and added TCP_NODELAY to my code. I have chosen the constant values on my own 
like the 5 seconds and 10 MB. 

## What did I learn about my own understanding of network programming?

Prior to completing this assignment, I viewed the function recv() as "receiving a message". 
Now I understand that TCP transfers a stream of bytes, and that it's up to the application to 
determine where messages end – by newline characters for commands and byte size for files. 
I learned about how one client-per-thread and a protected-by-mutex log cooperate, how a 
condition variable is a more elegant solution for halting a thread than sleep(), the differences 
between UDP and TCP when it comes to ordering and reliability, and the influence that various 
socket options (like SO_REUSEADDR or TCP_NODELAY on socket 48) have on the actual 
behaviour. I also learned not to disclose any secrets, after accidentally pasting a GitHub token 
in a chat and needing to invalidate it. The most important lesson I learned was that I truly 
understood a piece of code only after testing it and explaining each line.
