(function() {
  try {
    fetch('/generate_204?berry_js_alive=1', {
      credentials: 'include',
      mode: 'no-cors'
    });
    console.log('WatchShim js-alive');
  } catch (e) {}

  var vid = '__BERRY_VIDEO_ID__';
  var clients = ['android', 'vr'];
  var clientIdx = 0;
  var innRetries = 0;
  var savedTime = 0;
  var urls = [];
  var urlIndex = 0;

  function unescapeUrl(u) {
    return String(u)
      .replace(/\\u0026/g, '&')
      .replace(/\\u003d/g, '=')
      .replace(/\\u003f/g, '?')
      .replace(/\\\//g, '/');
  }

  function collectUrls(j, raw) {
    var out = [];
    var seen = {};
    function add(u) {
      if (!u) return;
      u = unescapeUrl(u);
      if (u.indexOf('googlevideo.com/videoplayback') < 0) return;
      if (seen[u]) return;
      seen[u] = 1;
      out.push(u);
    }
    var f = j && j.streamingData && j.streamingData.formats;
    if (f) {
      var prefer = [22, 18, 59, 78];
      var p, i;
      for (p = 0; p < prefer.length; p++)
        for (i = 0; i < f.length; i++)
          if (f[i].itag === prefer[p] && f[i].url) add(f[i].url);
      for (i = 0; i < f.length; i++)
        if (f[i].mimeType && f[i].mimeType.indexOf('video/mp4') >= 0 && f[i].url)
          add(f[i].url);
      for (i = 0; i < f.length; i++)
        if (f[i].url) add(f[i].url);
    }
    if (!out.length && raw) {
      var re = /"url":"(https:[^"]+googlevideo\.com\/videoplayback[^"]+)"/g;
      var m;
      while ((m = re.exec(raw))) add(m[1]);
    }
    return out;
  }

  function unplayableMsg(j) {
    if (!j) return 'Could not load video';
    var vd = j.videoDetails;
    if (vd && (vd.isLiveContent || vd.isLive))
      return 'Live streams are not supported in BerryBrowser.';
    var ps = j.playabilityStatus;
    if (ps) {
      if (ps.status === 'LOGIN_REQUIRED')
        return ps.reason || 'Sign in required.';
      if (ps.status === 'UNPLAYABLE' || ps.status === 'ERROR')
        return ps.reason || ps.status;
      if (ps.reason) return ps.reason;
    }
    return null;
  }

  function setTitle(j) {
    var t = j && j.videoDetails && j.videoDetails.title;
    if (!t) return;
    document.title = t;
    document.getElementById('title').textContent = t;
  }

  function showStatus(msg, tap) {
    var el = document.getElementById('status');
    el.textContent = msg;
    el.className = tap ? 'tap' : '';
    el.onclick = tap
      ? function() {
          innRetries = 0;
          clientIdx = 0;
          savedTime = 0;
          urls = [];
          urlIndex = 0;
          load(false);
        }
      : null;
  }

  function clientName() {
    return clients[clientIdx] || 'android';
  }

  function playerBody() {
    var c = clientName();
    var client = {
      hl: 'en',
      gl: 'US',
      timeZone: 'America/New_York',
      utcOffsetMinutes: -300
    };
    if (c === 'vr') {
      client.clientName = 'ANDROID_VR';
      client.clientVersion = '1.65.10';
      client.deviceMake = 'Oculus';
      client.deviceModel = 'Quest 3';
      client.androidSdkVersion = 32;
      client.osName = 'Android';
      client.osVersion = '12L';
    } else {
      client.clientName = 'ANDROID';
      client.clientVersion = '21.26.364';
      client.androidSdkVersion = 30;
      client.osName = 'Android';
      client.osVersion = '11';
    }
    return {
      context: { client: client },
      videoId: vid,
      racyCheckOk: true,
      contentCheckOk: true,
      playbackContext: {
        contentPlaybackContext: { html5Preference: 'HTML5_PREF_WANTS' }
      }
    };
  }

  function fetchPlayer() {
    var c = clientName();
    console.log('WatchShim player client=' + c);
    return fetch('/youtubei/v1/player?prettyPrint=false&berry_client=' + c, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      credentials: 'include',
      body: JSON.stringify(playerBody())
    }).then(function(r) {
      return r.text();
    }).then(function(raw) {
      var j = null;
      try {
        j = JSON.parse(raw);
      } catch (e) {
        console.log('WatchShim json parse fail client=' + c + ' bytes=' +
                    raw.length + ' err=' + e);
      }
      return { j: j, raw: raw };
    });
  }

  function nextClient(reason) {
    console.log('WatchShim next client after ' + clientName() + ' (' + reason + ')');
    clientIdx++;
    if (clientIdx < clients.length) {
      innRetries = 0;
      urls = [];
      urlIndex = 0;
      showStatus('Trying another player...');
      load(true);
      return true;
    }
    return false;
  }

  function playCurrent(v) {
    if (urlIndex >= urls.length) return false;
    var url = urls[urlIndex];
    console.log('WatchShim play idx=' + urlIndex + '/' + urls.length +
                ' url=' + url.substring(0, 80));
    showStatus('Playing');
    v.src = url;
    if (savedTime > 0) {
      try {
        v.currentTime = savedTime;
      } catch (e) {}
    }
    if (v.play) v.play().catch(function() {});
    return true;
  }

  function bindVideo(v) {
    v.onerror = function() {
      var code = v.error ? v.error.code : 0;
      console.log('WatchShim media error code=' + code + ' idx=' + urlIndex +
                  ' client=' + clientName());
      savedTime = v.currentTime || savedTime || 0;
      urlIndex++;
      if (playCurrent(v)) {
        showStatus('Trying another stream...');
        return;
      }
      if (nextClient('media ' + code))
        return;
      showStatus('Playback failed - tap to reload', true);
    };
  }

  function load() {
    showStatus('Fetching stream...');
    fetchPlayer()
      .then(function(got) {
        var j = got.j;
        setTitle(j);
        urls = collectUrls(j, got.raw);
        urlIndex = 0;
        var msg = unplayableMsg(j);
        console.log('WatchShim formats=' + urls.length + ' client=' +
                    clientName() + ' status=' +
                    (j && j.playabilityStatus && j.playabilityStatus.status) +
                    ' bytes=' + (got.raw ? got.raw.length : 0));
        if (!urls.length) {
          if (nextClient(msg || 'no url'))
            return;
          showStatus(msg || 'Could not load video', true);
          return;
        }
        showStatus('Playing');
        var v = document.getElementById('v');
        bindVideo(v);
        playCurrent(v);
      })
      .catch(function(e) {
        console.log('WatchShim fetch fail ' + e);
        if (innRetries++ < 1) {
          load();
          return;
        }
        if (nextClient('network'))
          return;
        showStatus('Network error - tap to retry', true);
      });
  }

  load();
})();
