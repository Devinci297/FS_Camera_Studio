/* Confirm committed saves. MSFS toolbar views use the simulator's supported UI sound route. */
(function (global) {
  'use strict';
  let audio = null, sequence = 0, soundReady = null;
  const heard = [];
  let status = { state: 'idle', message: 'No confirmation played yet.' };
  function isNative() { return typeof global.RegisterCommBusListener === 'function'; }
  function report(state, message) { status = { state: state, message: message }; return status; }
  function withTimeout(promise) {
    return new Promise(function (resolve, reject) {
      const timer = global.setTimeout(function () { reject(new Error('Simulator sound service timed out.')); }, 4000);
      Promise.resolve(promise).then(function (value) { global.clearTimeout(timer); resolve(value); }, function (error) {
        global.clearTimeout(timer); reject(error);
      });
    });
  }
  function nativeSoundListener() {
    if (!soundReady) {
      let listener;
      soundReady = new Promise(function (resolve, reject) {
        try {
          if (typeof global.RegisterViewListener !== 'function') throw new Error('Simulator sound service is unavailable.');
          listener = global.RegisterViewListener('JS_LISTENER_SOUND', resolve, true);
        } catch (error) { reject(error); }
      }).then(function () {
        if (!listener || typeof listener.call !== 'function') throw new Error('Simulator sound service is unavailable.');
        return listener;
      }).catch(function (error) { soundReady = null; throw error; });
    }
    return soundReady;
  }
  function play(requestId) {
    if (heard.indexOf(requestId) >= 0) return Promise.resolve(status);
    heard.push(requestId);
    if (heard.length > 32) heard.shift();
    if (isNative()) {
      // The current MSFS 2024 SoundManager uses JS_LISTENER_SOUND / PLAY_SOUND.
      // Wait for registration: the legacy ViewListener.call does not do that itself.
      return withTimeout(nativeSoundListener()).then(function (listener) {
        return withTimeout(listener.call('PLAY_SOUND', 'UI_VALID'));
      }).then(function () {
        return report('requested', 'Confirmation chime requested. Check the simulator UI sound volume if silent.');
      }, function () {
        return report('unavailable', 'Confirmation sound is unavailable. Your cameras are still saved.');
      });
    }
    try {
      if (!audio) { audio = new global.Audio('audio/cameras-saved.wav'); audio.preload = 'auto'; audio.volume = 0.8; }
      audio.pause(); audio.currentTime = 0;
      return Promise.resolve(audio.play()).then(function () {
        return report('playing', 'Playing Cameras saved.');
      }, function () {
        return report('unavailable', 'Voice playback was blocked. Your cameras are still saved.');
      });
    } catch (_) {
      return Promise.resolve(report('unavailable', 'Voice playback is unavailable. Your cameras are still saved.'));
    }
  }
  global.FSCSSaveFeedback = {
    play: play,
    test: function () { return play('feedback-test-' + (++sequence)); },
    isNative: isNative,
    getStatus: function () { return { state: status.state, message: status.message }; }
  };
})(window);
