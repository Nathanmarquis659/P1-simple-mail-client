# P1 Simple Mail Client

- Name: Nathan Marquis
- Email: nathanmarquis659@u.boisestate.edu
- Class: CS425-001

## Known Bugs or Issues

- **Unhandled malloc failure in command builders.** `cmd_helo`, `cmd_mail_from`, and `cmd_rcpt_to` allocate with `malloc` 
and return `NULL` on failure; `run_session` passes the result straight to `send_command`, which calls `strlen` on it. A 
malloc failure there would crash rather than fail cleanly. At these sizes malloc effectively never fails, so severity is 
low — but a larger version should check for NULL before use.
- **Message body is read entirely into memory.** When no `-b` file is given, `main` slurps all of stdin into a single 
buffer before building the payload. Fine for the message sizes in this project; not suitable for very large messages.
- **HELO only, by design.** Per the assignment, the client speaks plain HELO. There is no EHLO/ESMTP negotiation, TLS, 
or authentication.
- **Two cosmetic issues in error reporting.** (a) When the server's reply to `DATA` is not the expected code, the 
diagnostic prints "expected 250" but should say "expected 354"; (b) the "server hung up after DATA" notification goes to 
stdout via `printf` rather than stderr. Neither affects protocol correctness.

## Experience

This project was quite challenging for me. I read through the documentation, but my lack of high level C skills hindered
me the most and was an eye-opener. I had a lot of socket problems at layer 3 when trying to connect to the server because 
I was writing sloppy code and had been using the opening and closing for each socket properly. When we complied with
this standard (for the most part) was the most valuable lesson for me, seeing how these protocols are layered and connect
to each other with extreme care and precision. Faulty implementations are quite easy to make here, and could allow command
injection if dot stuffing was not properly addressed and considered. Overall this project was a great help in understanding
networking, although I needed quite a bit of help from AI to get it working 100% and tested. I used a smaller local AI model
(Qwen3.8-27B) which needed a fair amount of guidance and understanding on my part to get working well, but it turned out well!