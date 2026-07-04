(function() {
  try {
    fetch('/generate_204?berry_js_alive=1', {
      credentials: 'include',
      mode: 'no-cors'
    });
    console.log('WatchShim js-alive');
  } catch (e) {}

  var vid = '__BERRY_VIDEO_ID__';
  var innRetries = 0;
  var mediaRetries = 0;
  var savedTime = 0;

  function pickUrl(j) {
    var f = j && j.streamingData && j.streamingData.formats;
    if (!f) return null;
    var i;
    for (i = 0; i < f.length; i++)
      if (f[i].itag === 18 && f[i].url) return f[i].url;
    for (i = 0; i < f.length; i++)
      if (f[i].mimeType && f[i].mimeType.indexOf('video/mp4') >= 0 && f[i].url)
        return f[i].url;
    for (i = 0; i < f.length; i++)
      if (f[i].url) return f[i].url;
    return null;
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
    if (!pickUrl(j))
      return 'No progressive download available for this video.';
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
          innRetries = mediaRetries = 0;
          savedTime = 0;
          load(false);
        }
      : null;
  }

  function playerBody() {
    return {
      context: {
        client: {
          clientName: 'WEB',
          clientVersion: '2.20250101.01.00',
          hl: 'en',
          gl: 'US',
          timeZone: 'America/New_York',
          utcOffsetMinutes: -300,
          clientScreen: 'WATCH'
        }
      },
      videoId: vid,
      racyCheckOk: true,
      contentCheckOk: true,
      playbackContext: {
        contentPlaybackContext: { html5Preference: 'HTML5_PREF_WANTS' }
      }
    };
  }

  function fetchPlayer() {
    return fetch('/youtubei/v1/player?prettyPrint=false', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      credentials: 'include',
      body: JSON.stringify(playerBody())
    }).then(function(r) {
      return r.json();
    });
  }

  function bindVideo(v) {
    v.onerror = function() {
      if (mediaRetries++ < 1) {
        savedTime = v.currentTime || 0;
        showStatus('Refreshing stream URL...');
        load(true);
        return;
      }
      showStatus('Playback failed - tap to reload', true);
    };
  }

  function load(fromMediaErr) {
    if (!fromMediaErr) {
      if (innRetries > 1) {
        showStatus('Could not load video - tap to retry', true);
        return;
      }
      showStatus('Fetching stream...');
    }
    fetchPlayer()
      .then(function(j) {
        setTitle(j);
        var url = pickUrl(j);
        var msg = unplayableMsg(j);
        if (!url) {
          if (msg) {
            showStatus(msg, true);
            return;
          }
          if (innRetries++ < 1) {
            load(false);
            return;
          }
          showStatus(msg || 'Could not load video', true);
          return;
        }
        innRetries = 0;
        showStatus('Playing');
        var v = document.getElementById('v');
        bindVideo(v);
        v.src = url;
        if (savedTime > 0) {
          try {
            v.currentTime = savedTime;
          } catch (e) {}
        }
        savedTime = 0;
        if (v.play) v.play().catch(function() {});
      })
      .catch(function() {
        if (innRetries++ < 1) {
          load(false);
          return;
        }
        showStatus('Network error - tap to retry', true);
      });
  }

  load(false);
})();
