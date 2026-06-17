#!/bin/sh
# Test berry-daemon TCP command channel (127.0.0.1:8767).
# Usage: ./test-daemon.sh [URL]

DIR="$(cd "$(dirname "$0")" && pwd)"
URL="${1:-https://example.com}"
PORT="${QNX_BERRY_CMD_PORT:-8767}"
OUT="/tmp/berry-daemon-test.out"
ERR="/tmp/berry-daemon-test.err"

killall content_shell 2>/dev/null
sleep 1
rm -f "$OUT" "$ERR"

echo "Starting daemon..."
sh "$DIR/run-daemon.sh" >"$OUT" 2>"$ERR" &
DPID=$!

wait_for_port() {
  if [ ! -x "$DIR/node/node" ]; then
    return 1
  fi
  sh "$DIR/node/node" -e "
    var net=require('net');
    var port=$PORT;
    var n=0;
    function tryOnce(){
      n++;
      var s=net.connect(port,'127.0.0.1',function(){ s.end(); process.exit(0); });
      s.on('error',function(){
        if(n>=360) process.exit(1);
        setTimeout(tryOnce,1000);
      });
    }
    tryOnce();
  "
}

echo "Waiting for TCP port $PORT..."
if ! wait_for_port; then
  echo "FAIL: daemon did not open port $PORT"
  tail -10 "$ERR"
  kill "$DPID" 2>/dev/null
  exit 1
fi

echo "Sending LOAD $URL ..."
sh "$DIR/node/node" -e "
  var net=require('net');
  var s=net.connect($PORT,'127.0.0.1',function(){
    s.write('LOAD $URL\n');
    s.end();
  });
  s.on('error',function(e){console.error(e);process.exit(1);});
"

echo "Waiting for render..."
sleep 60

if grep -q '@BERRY DOM' "$OUT" 2>/dev/null; then
  bytes=$(wc -c <"$OUT")
  echo "PASS: @BERRY DOM received ($bytes bytes)"
  grep -E 'cmd:|LoadURL|@BERRY' "$ERR" "$OUT" 2>/dev/null
  sh "$DIR/node/node" -e "var n=require('net'),s=n.connect($PORT,'127.0.0.1',function(){s.write('QUIT\n');s.end();});" 2>/dev/null
  kill "$DPID" 2>/dev/null
  exit 0
fi

echo "FAIL: no @BERRY output ($(wc -c <"$OUT") bytes stdout)"
grep -E 'cmd:|LoadURL|berry-daemon' "$ERR" 2>/dev/null | tail -10
kill "$DPID" 2>/dev/null
exit 1
