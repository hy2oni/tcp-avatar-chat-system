TCP Multi-room Avatar Chat System
=================================

Environment
-----------
- Linux
- C
- TCP socket
- pthread

Build
-----
make

Or manually:
gcc server.c -o server -pthread
gcc client.c -o client -pthread

Run
---
Terminal 1:
./server 5555

Terminal 2+:
./client 127.0.0.1 5555

Login
-----
Each client chooses:
- unique nickname
- unique one-character avatar

Duplicate nicknames and duplicate avatars are rejected by the server.

Commands
--------
w / a / s / d          Move
/chat message          Chat. In Room 3, private if seated at a table.
/c message             Alias for /chat
/shout message         Public Room 3 chat, useful from a table seat.
/s message             Alias for /shout
/map                   Print current map
/help                  Print command list
/start                 Start quiz when allowed on Q in Room 2
O or X                 Answer current quiz question
/quit or /exit         Disconnect

Arrow Keys
----------
WASD movement is the stable supported input.
Arrow key escape sequences are translated when the terminal sends them as a full line.
If this is unreliable in a terminal, use WASD for the demo.

Demo Flow
---------
1. Start server.
2. Start three clients, for example kim/K, park/P, lee/L.
3. Show unique login checks, random Lobby spawn, movement, map updates, and collision blocking.
4. Use /chat in Lobby.
5. Move to the Room 2 portal labeled 2/@.
6. Step on Q, then the first Q visitor runs /start.
7. Answer O/X questions and show the scoreboard.
8. Move to the Room 3 portal labeled 3/@.
9. Sit on table seats. Use /chat for private table chat and /shout for public Room 3 chat.
10. Leave the table and check the server log for table log clearing.

AI Assistance Notice
--------------------
Major functions and complex logic include comments marked "AI-assisted implementation" as required by the assignment.
