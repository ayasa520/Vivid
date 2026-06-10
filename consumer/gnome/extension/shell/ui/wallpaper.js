/**
 * Copyright (C) 2023 Jeff Shee (jeffshee8969@gmail.com)
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Graphene from 'gi://Graphene';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import * as Logger from '../logger.js';

const logger = new Logger.Logger();
// Ref: https://gitlab.gnome.org/GNOME/gnome-shell/-/blob/main/js/ui/layout.js
const BACKGROUND_FADE_ANIMATION_TIME = 1000;
const ATTACH_POLL_INTERVAL_MS = 1000;
export const APPLICATION_ID = 'dev.rikka.VividWallpaper.Helper';
export const TITLE_PREFIX = `@${APPLICATION_ID}!`;
// const CUSTOM_BACKGROUND_BOUNDS_PADDING = 2;

/**
 * The GNOME Shell desktop-side display surface for display-v1.
 */
export const LiveWallpaper = GObject.registerClass(
    {
        /*
         * GObject types are process-global and cannot be unregistered when a
         * GNOME Shell extension is disabled. Other live wallpaper extensions may
         * also register a class named `LiveWallpaper` from `shell/ui/wallpaper.js`,
         * which auto-generates the same GType name in the same Shell session.
         * Keep the consumer type name explicit and project-specific so users can
         * switch extensions without leaving either one stuck in ERROR.
         */
        GTypeName: 'VividWallpaperLiveWallpaper',
    },
    class LiveWallpaper extends St.Widget {
        constructor(backgroundActor, displayHelper = null) {
            super({
                layout_manager: new Clutter.BinLayout(),
                width: backgroundActor.width,
                height: backgroundActor.height,
                // Layout manager will allocate extra space for the actor, if possible.
                x_expand: true,
                y_expand: true,
                opacity: 0,
                reactive: true,
            });
            this._backgroundActor = backgroundActor;
            this._metaBackgroundGroup = backgroundActor.get_parent();
            this._monitorIndex = backgroundActor.monitor;
            this._displayHelper = displayHelper;
            this._lastMotionPos = null;
            this._cloneActor = null;
            this._cloneDestroyId = 0;
            this._sourceActor = null;
            this._sourceDestroyId = 0;
            this._attachPollId = 0;
            this._attachAttempts = 0;

            /**
             * _monitorScale is fractional scale factor
             * _monitorWidth and _monitorHeight are scaled resolution
             * e.g. if the physical reolution = (2240, 1400) and fractional scale factor = 1.25,
             * then the scaled resolution would be (2240/1.25, 1400/1.25) = (1792, 1120).
             */
            this._display = backgroundActor.meta_display;
            this._monitorScale = this._display.get_monitor_scale(
                this._monitorIndex
            );
            let {width, height} =
                Main.layoutManager.monitors[this._monitorIndex];
            this._monitorWidth = width;
            this._monitorHeight = height;

            backgroundActor.layout_manager = new Clutter.BinLayout();
            backgroundActor.add_child(this);

            this._tryAttachHelperWindow();

            /*
             * Rounded clipping is cosmetic and is deliberately disabled in the
             * split consumer. GNOME Shell 49 no longer exposes the old GLSL
             * SnippetHook API used by the legacy effect; constructing that
             * effect logs an asynchronous JS ERROR even when wrapped in
             * try/catch, which can interrupt the wallpaper clone path.
             */
            this._setupPointerCapture();
            this.connect('destroy', () => this._onDestroy());

            // FIXME: Bounds calculation is wrong if the layout isn't vanilla (with custom dock, panel, etc.), disabled for now.
            // this.connect('notify::allocation', () => {
            //     let heightOffset = this.height - this._metaBackgroundGroup.get_parent().height;
            //     this._roundedCornersEffect.setBounds(
            //         [
            //             CUSTOM_BACKGROUND_BOUNDS_PADDING,
            //             CUSTOM_BACKGROUND_BOUNDS_PADDING + heightOffset,
            //             this.width,
            //             this.height,
            //         ].map(e => e * this._monitorScale)
            //     );
            // });
        }

        setPixelStep(width, height) {
        }

        setRoundedClipRadius(radius) {
        }

        setRoundedClipBounds(x1, y1, x2, y2) {
        }

        _fade(visible = true) {
            this.ease({
                opacity: visible ? 255 : 0,
                duration: BACKGROUND_FADE_ANIMATION_TIME,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            });
        }

        _setupPointerCapture() {
            this.connect('motion-event', (_actor, event) => {
                this._capturePointerEvent('mousemove', event);
                return Clutter.EVENT_PROPAGATE;
            });

            this.connect('button-press-event', (_actor, event) => {
                this._capturePointerEvent('mousedown', event);
                return Clutter.EVENT_PROPAGATE;
            });

            this.connect('button-release-event', (_actor, event) => {
                this._capturePointerEvent('mouseup', event);
                return Clutter.EVENT_PROPAGATE;
            });

            this.connect('scroll-event', (_actor, event) => {
                this._capturePointerEvent('wheel', event);
                return Clutter.EVENT_PROPAGATE;
            });
        }

        _capturePointerEvent(type, event) {
            if (!this._isCaptureActive())
                return;

            const [stageX, stageY] = event.get_coords();
            const [actorX, actorY] = this.get_transformed_position();
            const x = stageX - actorX;
            const y = stageY - actorY;
            if (x < 0 || y < 0 || x > this.width || y > this.height)
                return;

            if (type === 'mousemove') {
                if (this._lastMotionPos) {
                    const dx = Math.abs(x - this._lastMotionPos.x);
                    const dy = Math.abs(y - this._lastMotionPos.y);
                    if (dx === 0 && dy === 0)
                        return;
                }
                this._lastMotionPos = {x, y};
            }

            let button = 0;
            if (type === 'mousedown' || type === 'mouseup')
                button = event.get_button();

            let deltaX = 0;
            let deltaY = 0;
            if (type === 'wheel') {
                const scrollDirection = event.get_scroll_direction();
                if (scrollDirection === Clutter.ScrollDirection.UP) {
                    deltaY = -120;
                } else if (scrollDirection === Clutter.ScrollDirection.DOWN) {
                    deltaY = 120;
                } else if (scrollDirection === Clutter.ScrollDirection.LEFT) {
                    deltaX = -120;
                } else if (scrollDirection === Clutter.ScrollDirection.RIGHT) {
                    deltaX = 120;
                } else {
                    [deltaX, deltaY] = event.get_scroll_delta();
                }
            }

            const wasSent = this._displayHelper?.sendPointerEvent?.({
                type,
                monitorIndex: this._monitorIndex,
                x,
                y,
                button,
                deltaX,
                deltaY,
                timeUsec: GLib.get_monotonic_time(),
            });
            if (!wasSent && type === 'mousemove')
                this._lastMotionPos = null;
        }

        _isCaptureActive() {
            return !!this._displayHelper;
        }

        _tryAttachHelperWindow() {
            const source = this._findHelperWindowActor();
            if (!source) {
                this._attachAttempts++;
                if (this._attachAttempts <= 5 || this._attachAttempts % 10 === 0) {
                    logger.warn(`helper clone source not found yet monitor=${this._monitorIndex} ` +
                        `attempt=${this._attachAttempts}`);
                }
                this._scheduleAttachPoll();
                return;
            }

            /*
             * The helper process owns GTK/GDK DMA-BUF import. GNOME Shell only
             * clones the helper's compositor actor into the desktop background,
             * which avoids linking the extension's display module to Mutter's
             * private Clutter/Cogl ABI.
             */
            this._sourceActor = source;
            this._cloneActor = new Clutter.Clone({
                source,
                pivot_point: new Graphene.Point({x: 0.5, y: 0.5}),
            });
            this._attachAttempts = 0;
            this._cloneDestroyId = this._cloneActor.connect('destroy', () => {
                this._cloneActor = null;
                this._cloneDestroyId = 0;
            });
            this._sourceDestroyId = source.connect('destroy', () => this._onSourceDestroyed());
            this.add_child(this._cloneActor);
            this._fade();
            const window = source.meta_window;
            logger.debug(`Attached helper clone monitor=${this._monitorIndex} ` +
                `source-monitor=${Number(window?.get_monitor?.() ?? -1)} ` +
                `source-visible=${!!source.visible} source-mapped=${!!source.mapped} ` +
                `source-size=${Math.round(source.width)}x${Math.round(source.height)} ` +
                `window-minimized=${!!window?.minimized}`);
        }

        _scheduleAttachPoll() {
            if (this._attachPollId !== 0)
                return;

            this._attachPollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ATTACH_POLL_INTERVAL_MS, () => {
                this._attachPollId = 0;
                if (!this._cloneActor)
                    this._tryAttachHelperWindow();
                return GLib.SOURCE_REMOVE;
            });
        }

        _getWindowActorsForCloneLookup() {
            /*
             * The legacy in-process renderer asked GNOME Shell for the unfiltered window actor
             * list with get_window_actors(false), because the renderer/helper
             * window is intentionally hidden from user-facing Shell lists while
             * still being a valid clone source. Some newer Shell versions expose
             * the method without that compatibility argument; keep the original
             * path when it exists and fall back only when introspection rejects
             * the argument.
             */
            try {
                return global.get_window_actors(false);
            } catch (_e) {
                return global.get_window_actors();
            }
        }

        _findHelperWindowActor() {
            const actors = this._getWindowActorsForCloneLookup();
            const helperActors = actors.filter(actor =>
                actor.meta_window?.title?.startsWith(TITLE_PREFIX)
            );

            const nMonitors = global.display.get_n_monitors();
            if (helperActors.length < nMonitors) {
                logger.debug(`helper windows (${helperActors.length}) < monitors (${nMonitors}), rejecting`);
                return null;
            }

            const monitorIndices = helperActors.map(actor =>
                Number(actor.meta_window?.get_monitor?.() ?? -1)
            );
            if (new Set(monitorIndices).size !== monitorIndices.length) {
                logger.debug(`helper monitor indices are not unique (${monitorIndices.join(',')}), rejecting`);
                return null;
            }

            return helperActors.find(actor =>
                Number(actor.meta_window?.get_monitor?.() ?? -1) === this._monitorIndex
            ) ?? null;
        }

        _onSourceDestroyed() {
            this._sourceDestroyId = 0;
            this._sourceActor = null;

            if (this._cloneActor) {
                const clone = this._cloneActor;
                this._cloneActor = null;
                if (this._cloneDestroyId) {
                    try {
                        clone.disconnect(this._cloneDestroyId);
                    } catch (_e) {
                    }
                    this._cloneDestroyId = 0;
                }
                try {
                    clone.destroy();
                } catch (_e) {
                }
            }

            this._scheduleAttachPoll();
        }

        _onDestroy() {
            if (this._attachPollId) {
                GLib.source_remove(this._attachPollId);
                this._attachPollId = 0;
            }

            if (this._sourceActor && this._sourceDestroyId) {
                try {
                    this._sourceActor.disconnect(this._sourceDestroyId);
                } catch (_e) {
                }
            }
            this._sourceActor = null;
            this._sourceDestroyId = 0;

            if (this._cloneActor) {
                try {
                    this._cloneActor.destroy();
                } catch (_e) {
                }
            }
            this._cloneActor = null;
            this._displayHelper = null;
        }
    }
);
