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
w / a / s / d          Move instantly in a normal Linux terminal
c                      Enter chat mode
/                      Enter command mode
/chat message          Chat. In Room 3, private if seated at a table.
/chat                  Enter chat mode from command mode
/move                  Return from chat mode to move mode
/c message             Alias for /chat
/shout message         Public Room 3 chat, useful from a table seat.
/s message             Alias for /shout
/map                   Print current map
/help                  Print command list
/start                 Start quiz when allowed on Q in Room 2
O or X                 Answer current quiz question
/quit or /exit         Disconnect

Client UI
---------
The client uses ANSI escape sequences to redraw a simple dashboard:
- map/status area
- fixed quiz status panel in Room 2
- current player status
- recent chat/system/quiz log
- bottom input prompt

In a normal Linux terminal, the client uses raw input mode:
- MOVE mode: w/a/s/d moves immediately without Enter.
- During a Room 2 quiz, O/X submits an answer immediately without Enter.
- Press c to enter CHAT mode.
- CHAT mode sends each line as chat after Enter.
- Type /move or press ESC to return to MOVE mode.
- Press / in MOVE mode to type one command such as /map or /quit.

If the client is run from a pipe or non-interactive terminal, it falls back to
line input mode where Enter is required.

Maps are pushed by the server after movement, room transitions, joins, and
disconnects, so users in the same room see avatar position changes without
typing a new command.

Run the client in a normal Linux terminal. If output looks messy after resizing
the window, type /map to refresh the map area.

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
