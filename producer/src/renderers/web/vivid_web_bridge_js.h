/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * JavaScript bridge injected by vivid-web-helper into every page context
 * before any project script runs (CefRenderProcessHandler::OnContextCreated).
 *
 * It emulates the Wallpaper Engine web API surface (wallpaperPropertyListener,
 * wallpaperRegisterAudioListener, wallpaperRegisterMedia*Listener) on top of
 * push-style __vivid* entry points that the browser process drives with
 * ExecuteJavaScript. There is no HTTP polling and no URL rewriting: pages run
 * directly from their file:// project directory.
 *
 * GameMaker HTML5 runtimes used by some Wallpaper Engine projects walk
 * function.caller while formatting fatal errors, which throws on strict,
 * async, or arrow-function frames - every path that can call project code
 * sticks to classic functions on purpose (ported from the old gstcefsrc
 * bootstrap).
 */
#pragma once

static const char kVividWebBridgeJs[] = R"VIVID_JS(
(function() {
    if (window.__vividWallpaperBridgeInstalled)
        return;
    window.__vividWallpaperBridgeInstalled = true;

    /* ---- wallpapers are not touch surfaces ---- */
    try {
        delete window.ontouchstart;
    } catch (_e) {
    }
    try {
        var proto = Object.getPrototypeOf(window);
        if (proto)
            delete proto.ontouchstart;
    } catch (_e) {
    }
    try {
        Object.defineProperty(navigator, 'maxTouchPoints', {
            configurable: true,
            get: function() {
                return 0;
            },
        });
    } catch (_e) {
    }

    /* ---- track AudioContexts so the pause script can suspend them ---- */
    try {
        window.__vividAudioContexts = [];
        var patchAudioContext = function(name) {
            var Original = window[name];
            if (typeof Original !== 'function')
                return;
            var Wrapped = function() {
                var context = new (Function.prototype.bind.apply(
                    Original, [null].concat(Array.prototype.slice.call(arguments))))();
                try {
                    window.__vividAudioContexts.push(context);
                } catch (_e) {
                }
                return context;
            };
            Wrapped.prototype = Original.prototype;
            try {
                Object.defineProperty(Wrapped, 'name', {value: name});
            } catch (_e) {
            }
            window[name] = Wrapped;
        };
        patchAudioContext('AudioContext');
        patchAudioContext('webkitAudioContext');
    } catch (_e) {
    }

    function logBridgeError(methodName, error) {
        var message = error && (error.stack || error.message || String(error));
        console.warn('[Vivid] wallpaper bridge callback failed in ' + methodName + ': ' + message);
    }

    /* ---- audio visualization bridge (wallpaperRegisterAudioListener) ---- */
    var wallpaperAudioListenerValue = null;
    var latestAudioFrame = new Array(128);
    for (var sampleIndex = 0; sampleIndex < 128; sampleIndex++)
        latestAudioFrame[sampleIndex] = 0;

    function flushAudioFrame() {
        if (typeof wallpaperAudioListenerValue !== 'function')
            return;
        try {
            wallpaperAudioListenerValue(latestAudioFrame.slice());
        } catch (error) {
            logBridgeError('wallpaperRegisterAudioListener', error);
        }
    }

    Object.defineProperty(window, 'wallpaperRegisterAudioListener', {
        configurable: true,
        enumerable: true,
        writable: true,
        value: function(callback) {
            wallpaperAudioListenerValue = typeof callback === 'function' ? callback : null;
            window.setTimeout(function() {
                flushAudioFrame();
            }, 0);
        },
    });

    window.__vividApplyAudioFrame = function(payload) {
        latestAudioFrame = Array.isArray(payload) ? payload.slice(0, 128) : [];
        while (latestAudioFrame.length < 128)
            latestAudioFrame.push(0);
        flushAudioFrame();
    };

    /* ---- wallpaperPropertyListener bridge ---- */
    var bridgeState = {
        userProperties: {},
        generalProperties: {},
        paused: false,
    };
    var wallpaperPropertyListenerValue = window.wallpaperPropertyListener;
    var bridgeCallbacksReady = document.readyState === 'complete';
    var bridgeReplayPending = false;
    var bridgeReplayScheduled = false;

    function getPropertyListener() {
        var listener = wallpaperPropertyListenerValue;
        if (!listener || typeof listener !== 'object')
            return null;
        return listener;
    }

    function safeInvokeListener(listener, methodName, args) {
        if (!listener || typeof listener[methodName] !== 'function')
            return;
        try {
            listener[methodName].apply(listener, args || []);
        } catch (error) {
            logBridgeError(methodName, error);
        }
    }

    function flushUserProperties() {
        safeInvokeListener(getPropertyListener(), 'applyUserProperties', [bridgeState.userProperties]);
    }

    function flushGeneralProperties() {
        safeInvokeListener(getPropertyListener(), 'applyGeneralProperties', [bridgeState.generalProperties]);
    }

    function flushPausedState() {
        safeInvokeListener(getPropertyListener(), 'setPaused', [bridgeState.paused]);
    }

    function applyPlaybackState() {
        var playing = !bridgeState.paused;
        var mediaElements = document.querySelectorAll('audio, video');
        for (var i = 0; i < mediaElements.length; i++) {
            var media = mediaElements[i];
            if (playing) {
                if (typeof media.play === 'function') {
                    var playPromise = media.play();
                    if (playPromise && typeof playPromise.catch === 'function')
                        playPromise.catch(function(_e) {});
                }
            } else if (typeof media.pause === 'function') {
                media.pause();
            }
        }
        try {
            window.dispatchEvent(new CustomEvent('vivid-playback-change', {
                detail: {playing: playing},
            }));
        } catch (error) {
            logBridgeError('vivid-playback-change', error);
        }
    }

    function replayBridgeState() {
        flushUserProperties();
        flushGeneralProperties();
        flushPausedState();
        applyPlaybackState();
    }

    /*
     * Some wallpapers register wallpaperPropertyListener before their own
     * window.load initializers create WebGL canvases or media elements. Defer
     * replays until load handlers have run so property callbacks see fully
     * initialized project globals.
     */
    function scheduleBridgeReplay() {
        bridgeReplayPending = true;
        if (!bridgeCallbacksReady || bridgeReplayScheduled)
            return;
        bridgeReplayScheduled = true;
        window.setTimeout(function() {
            bridgeReplayScheduled = false;
            if (!bridgeReplayPending || !bridgeCallbacksReady)
                return;
            bridgeReplayPending = false;
            replayBridgeState();
        }, 0);
    }

    Object.defineProperty(window, 'wallpaperPropertyListener', {
        configurable: true,
        enumerable: true,
        get: function() {
            return wallpaperPropertyListenerValue;
        },
        set: function(value) {
            wallpaperPropertyListenerValue = value;
            scheduleBridgeReplay();
        },
    });

    window.__vividApplyUserProperties = function(payload) {
        bridgeState.userProperties = payload && typeof payload === 'object' ? payload : {};
        if (!bridgeCallbacksReady) {
            scheduleBridgeReplay();
            return;
        }
        flushUserProperties();
    };

    window.__vividApplyGeneralProperties = function(payload) {
        bridgeState.generalProperties = payload && typeof payload === 'object' ? payload : {};
        if (!bridgeCallbacksReady) {
            scheduleBridgeReplay();
            return;
        }
        flushGeneralProperties();
    };

    window.__vividSetPaused = function(isPaused) {
        var nextPaused = Boolean(isPaused);
        if (bridgeState.paused === nextPaused && bridgeCallbacksReady)
            return;
        bridgeState.paused = nextPaused;
        if (!bridgeCallbacksReady) {
            scheduleBridgeReplay();
            return;
        }
        flushPausedState();
        applyPlaybackState();
    };

    /* ---- Wallpaper Engine media integration API ---- */
    var PLAYBACK_STOPPED = 0;
    var PLAYBACK_PLAYING = 1;
    var PLAYBACK_PAUSED = 2;
    window.wallpaperMediaIntegration = {
        PLAYBACK_STOPPED: PLAYBACK_STOPPED,
        PLAYBACK_PLAYING: PLAYBACK_PLAYING,
        PLAYBACK_PAUSED: PLAYBACK_PAUSED,
    };

    var mediaListeners = {
        status: null,
        properties: null,
        playback: null,
        thumbnail: null,
        timeline: null,
    };
    var latestMediaState = null;

    function colorChannel(value) {
        var scaled = Math.round(Number(value || 0) * 255);
        if (scaled < 0)
            scaled = 0;
        if (scaled > 255)
            scaled = 255;
        return scaled;
    }

    function cssColor(rgb) {
        if (!Array.isArray(rgb) || rgb.length < 3)
            return 'rgb(0, 0, 0)';
        return 'rgb(' + colorChannel(rgb[0]) + ', ' + colorChannel(rgb[1]) + ', ' +
            colorChannel(rgb[2]) + ')';
    }

    function thumbnailUrl(path) {
        if (typeof path !== 'string' || path === '')
            return '';
        if (path.indexOf('://') >= 0 || path.indexOf('data:') === 0)
            return path;
        return 'file://' + path;
    }

    function dispatchMediaStatus(state) {
        safeInvokeListener(mediaListeners, 'status', [{
            enabled: Number(state.playbackState || 0) !== PLAYBACK_STOPPED,
        }]);
    }

    function dispatchMediaProperties(state) {
        safeInvokeListener(mediaListeners, 'properties', [{
            title: String(state.title || ''),
            artist: String(state.artist || ''),
            subTitle: String(state.subTitle || ''),
            albumTitle: String(state.albumTitle || ''),
            albumArtist: String(state.albumArtist || ''),
            genres: String(state.genres || ''),
            contentType: String(state.contentType || ''),
        }]);
    }

    function dispatchMediaPlayback(state) {
        var playbackState = Number(state.playbackState || 0);
        if (playbackState !== PLAYBACK_PLAYING && playbackState !== PLAYBACK_PAUSED)
            playbackState = playbackState === PLAYBACK_STOPPED ? PLAYBACK_STOPPED : PLAYBACK_PLAYING;
        safeInvokeListener(mediaListeners, 'playback', [{state: playbackState}]);
    }

    function dispatchMediaThumbnail(state) {
        if (!state.hasThumbnail)
            return;
        safeInvokeListener(mediaListeners, 'thumbnail', [{
            thumbnail: thumbnailUrl(state.thumbnailPath),
            primaryColor: cssColor(state.primaryColor),
            secondaryColor: cssColor(state.secondaryColor),
            tertiaryColor: cssColor(state.tertiaryColor),
            textColor: cssColor(state.textColor),
            highContrastColor: cssColor(state.highContrastColor),
        }]);
    }

    function dispatchMediaState(state) {
        if (!state || typeof state !== 'object')
            return;
        dispatchMediaStatus(state);
        dispatchMediaProperties(state);
        dispatchMediaPlayback(state);
        dispatchMediaThumbnail(state);
    }

    function defineMediaRegister(globalName, listenerKey) {
        Object.defineProperty(window, globalName, {
            configurable: true,
            enumerable: true,
            writable: true,
            value: function(callback) {
                mediaListeners[listenerKey] =
                    typeof callback === 'function' ? callback : null;
                if (latestMediaState) {
                    window.setTimeout(function() {
                        if (latestMediaState)
                            dispatchMediaState(latestMediaState);
                    }, 0);
                }
            },
        });
    }

    defineMediaRegister('wallpaperRegisterMediaStatusListener', 'status');
    defineMediaRegister('wallpaperRegisterMediaPropertiesListener', 'properties');
    defineMediaRegister('wallpaperRegisterMediaPlaybackListener', 'playback');
    defineMediaRegister('wallpaperRegisterMediaThumbnailListener', 'thumbnail');
    defineMediaRegister('wallpaperRegisterMediaTimelineListener', 'timeline');

    window.__vividApplyMediaState = function(state) {
        latestMediaState = state && typeof state === 'object' ? state : null;
        if (latestMediaState)
            dispatchMediaState(latestMediaState);
    };

    function markBridgeCallbacksReady() {
        window.setTimeout(function() {
            bridgeCallbacksReady = true;
            scheduleBridgeReplay();
        }, 0);
    }

    if (bridgeCallbacksReady)
        markBridgeCallbacksReady();
    else
        window.addEventListener('load', markBridgeCallbacksReady, {once: true});
})();
)VIVID_JS";
