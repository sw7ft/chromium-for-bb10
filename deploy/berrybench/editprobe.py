#!/usr/bin/env python3
"""Baseline probe for touch text selection: editprobe.py <engine>

Long-presses a word through the real Aura gesture path (CDP
Input.synthesizeTapGesture goes through SyntheticGestureTargetAura, i.e. the
same touch -> gesture recognizer -> RenderWidgetHostViewAura route a finger
takes) and reports what the page sees.
"""
import json, sys, urllib.parse
from engine import Engine, stop_all

HTML = """<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<style>body{font:20px sans-serif;margin:20px} p{margin:30px 0} input,textarea{font:20px sans-serif;width:300px}</style>
<p id=p>Hello <span id=w>BlackBerry</span> world, this is text.</p>
<input id=i value="first second third">
<p><textarea id=t rows=2></textarea></p>
<p><a id=a href="https://example.com/x">a link here</a></p>
"""
PAGE = "data:text/html," + urllib.parse.quote(HTML)

def center(c, sel):
    return json.loads(c.evaluate(
        "(function(){var r=document.querySelector(%s).getBoundingClientRect();"
        "return JSON.stringify([Math.round(r.left+r.width/2),Math.round(r.top+r.height/2)])})()" % json.dumps(sel)))

eng = sys.argv[1]
stop_all()
with Engine(eng, log_path="logs/editprobe-%s.log" % eng.replace("/", "_"),
            extra="--content-shell-host-window-size=540x540") as e:
    e.start()
    c = e.cdp
    c.call("Page.navigate", {"url": PAGE})
    c.pump(3)
    x, y = center(c, "#w")
    print("word at", x, y)
    r = c.call("Input.synthesizeTapGesture",
               {"x": x, "y": y, "duration": 1200, "tapCount": 1, "gestureSourceType": "touch"}, timeout=30)
    c.pump(1.5)
    print("selection after long-press on word: %r" % c.evaluate("getSelection().toString()"))
    c.evaluate("document.execCommand('copy')")
    x, y = center(c, "#t")
    c.call("Input.synthesizeTapGesture", {"x": x, "y": y, "duration": 50, "tapCount": 1, "gestureSourceType": "touch"}, timeout=30)
    c.pump(1)
    print("focused:", c.evaluate("document.activeElement.id"))
    print("execCommand paste ->", c.evaluate("document.execCommand('paste')"),
          "textarea=%r" % c.evaluate("document.getElementById('t').value"))
    x, y = center(c, "#i")
    c.call("Input.synthesizeTapGesture", {"x": x - 100, "y": y, "duration": 1200, "tapCount": 1, "gestureSourceType": "touch"}, timeout=30)
    c.pump(1.5)
    print("input selection after long-press: %r" % c.evaluate(
        "(function(){var i=document.getElementById('i');return i.value.substring(i.selectionStart,i.selectionEnd)})()"))
