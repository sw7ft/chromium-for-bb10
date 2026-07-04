(function() {
  try {
    fetch('/generate_204?berry_js_alive=1', {
      credentials: 'include',
      mode: 'no-cors'
    });
    console.log('SearchShim js-alive');
  } catch (e) {}

  var query = '__BERRY_SEARCH_QUERY__';
  var retries = 0;

  function escHtml(s) {
    if (!s) return '';
    return String(s)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  function searchBody() {
    return {
      context: {
        client: {
          clientName: 'WEB',
          clientVersion: '2.20250101.01.00',
          hl: 'en',
          gl: 'US',
          timeZone: 'America/New_York',
          utcOffsetMinutes: -300
        }
      },
      query: query
    };
  }

  function collectVideoRenderers(obj, out) {
    if (!obj || typeof obj !== 'object') return;
    if (obj.videoId && obj.title) {
      out.push(obj);
      return;
    }
    if (Object.prototype.toString.call(obj) === '[object Array]') {
      var i;
      for (i = 0; i < obj.length; i++) collectVideoRenderers(obj[i], out);
      return;
    }
    for (var k in obj) {
      if (Object.prototype.hasOwnProperty.call(obj, k))
        collectVideoRenderers(obj[k], out);
    }
  }

  function showStatus(msg, tap) {
    var el = document.getElementById('status');
    el.textContent = msg;
    el.className = tap ? 'tap' : '';
    el.onclick = tap
      ? function() {
          retries = 0;
          load();
        }
      : null;
  }

  function renderResults(videos) {
    var list = document.getElementById('list');
    list.innerHTML = '';
    if (!videos.length) {
      showStatus('No videos found.');
      return;
    }
    showStatus(videos.length + ' video' + (videos.length === 1 ? '' : 's'));
    var i;
    for (i = 0; i < videos.length; i++) {
      var v = videos[i];
      var id = v.videoId;
      var title =
        v.title && v.title.runs && v.title.runs[0]
          ? v.title.runs[0].text
          : 'Untitled';
      var channel =
        v.ownerText && v.ownerText.runs && v.ownerText.runs[0]
          ? v.ownerText.runs[0].text
          : '';
      var dur =
        v.lengthText && v.lengthText.simpleText ? v.lengthText.simpleText : '';
      var thumb = '';
      if (v.thumbnail && v.thumbnail.thumbnails && v.thumbnail.thumbnails.length) {
        var thumbs = v.thumbnail.thumbnails;
        thumb = thumbs[thumbs.length - 1].url;
      }
      var a = document.createElement('a');
      a.className = 'row';
      a.href = '/watch?v=' + encodeURIComponent(id);
      a.innerHTML =
        '<img src="' +
        escHtml(thumb) +
        '" alt="">' +
        '<div class="meta"><div class="t">' +
        escHtml(title) +
        '</div><div class="c">' +
        escHtml(channel) +
        '</div></div>' +
        '<div class="d">' +
        escHtml(dur) +
        '</div>';
      list.appendChild(a);
    }
  }

  function load() {
    showStatus('Searching...');
    fetch('/youtubei/v1/search?prettyPrint=false', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      credentials: 'include',
      body: JSON.stringify(searchBody())
    })
      .then(function(r) {
        return r.json();
      })
      .then(function(j) {
        var videos = [];
        collectVideoRenderers(j, videos);
        var seen = {};
        var deduped = [];
        var i;
        for (i = 0; i < videos.length; i++) {
          if (seen[videos[i].videoId]) continue;
          seen[videos[i].videoId] = 1;
          deduped.push(videos[i]);
        }
        renderResults(deduped);
        console.log('SearchShim results=' + deduped.length);
      })
      .catch(function() {
        if (retries++ < 1) {
          load();
          return;
        }
        showStatus('Search failed — tap to retry', true);
      });
  }

  document.getElementById('searchForm').onsubmit = function(e) {
    e.preventDefault();
    var q = document.getElementById('q').value.trim();
    if (!q) return false;
    window.location.href =
      '/results?search_query=' + encodeURIComponent(q);
    return false;
  };

  load();
})();
