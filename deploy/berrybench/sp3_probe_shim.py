"""Shim for a browser bug found 2026-11-20 (present in build 109 as well).

The load event for an iframe's initial about:blank document is delivered after
insertion returns rather than during it. Speedometer inserts the frame, then
assigns onload and src, so the late event starts a suite before its document
has loaded. Ignore a load event whose document is still about:blank while the
element already points at a real URL. Timing of the measured work is
unaffected.
"""
SHIM_JS = """(function(){
  var d = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'onload');
  if (!d || !d.set) return;
  Object.defineProperty(HTMLIFrameElement.prototype, 'onload', {
    configurable: true, enumerable: true,
    get: function(){ return this.__berryOnload || null; },
    set: function(h){
      this.__berryOnload = h;
      var self = this;
      d.set.call(this, typeof h !== 'function' ? h : function(ev){
        var src = self.getAttribute('src');
        var href = '';
        try { href = self.contentWindow.location.href; } catch (e) {}
        if (src && src !== 'about:blank' && href === 'about:blank') return;
        return h.call(self, ev);
      });
    }
  });
})();"""
