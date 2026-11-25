#!/bin/sh
# End-to-end check of the C chat example: start the server, run two clients, compare the output.
# Usage: chat_example.sh <chat_server> <chat_client>
server=$1
client=$2
work=$(mktemp -d)
trap 'kill "$pid" 2>/dev/null; rm -rf "$work"' EXIT

# The server prints its port on the first line (it listens on port 0: no fixed port in tests).
$server > "$work/server.out" &
pid=$!
tries=0
until [ -s "$work/server.out" ] || [ $tries -ge 200 ]; do tries=$((tries + 1)); sleep 0.05; done
port=$(head -n 1 "$work/server.out")
[ -n "$port" ] || { echo "server did not start"; exit 1; }

# Alice sends two messages and must receive both broadcasts back, in order.
$client "$port" alice "hello room" "second message" > "$work/alice.out" || { echo "client failed"; exit 1; }
printf '[%s] hello room\n[%s] second message\n' "$(sed -n '1s/^\[\([0-9]*\)\].*/\1/p' "$work/alice.out")" \
    "$(sed -n '2s/^\[\([0-9]*\)\].*/\1/p' "$work/alice.out")" > "$work/expected.out"
diff "$work/alice.out" "$work/expected.out" || { echo "unexpected output"; exit 1; }

# A second client gets its own message and a different user id.
$client "$port" bob "hi from bob" > "$work/bob.out" || { echo "second client failed"; exit 1; }
grep -q "hi from bob" "$work/bob.out" || { echo "bob did not get his message"; exit 1; }
echo "chat example OK"
