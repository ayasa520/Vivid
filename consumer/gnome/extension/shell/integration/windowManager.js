import GLib from 'gi://GLib';
import GObject from 'gi://GObject';

import * as Config from 'resource:///org/gnome/shell/misc/config.js';

import * as Logger from '../logger.js';
import * as Wallpaper from '../ui/wallpaper.js';

const logger = new Logger.Logger('helperWindowManager');
const shellVersion = parseInt(Config.PACKAGE_VERSION.split('.')[0]);

class ManagedHelperWindow {
    constructor(window) {
        this._window = window;
        this._signals = [];
        this._lowerIdleId = 0;
        this._notifyPositionId = 0;
        this._surfaceContainer = null;
        this._states = {
            keepAtBottom: false,
            keepMinimized: false,
            keepPosition: false,
            position: [0, 0],
        };

        this._signals.push(window.connect('notify::title', () => this._parseTitle()));
        this._signals.push(window.connect_after('shown', () => {
            if (this._states.keepMinimized)
                this._window.minimize();
        }));
        this._signals.push(window.connect_after('raised', () => {
            if (this._states.keepAtBottom)
                this._window.lower();
        }));
        this._signals.push(window.connect('notify::above', () => {
            if (this._states.keepAtBottom && this._window.above)
                this._window.unmake_above();
        }));
        this._signals.push(window.connect('notify::minimized', () => {
            if (this._states.keepMinimized && !this._window.minimized)
                this._window.minimize();
        }));
        this._signals.push(window.connect('position-changed', () => {
            if (this._states.keepPosition) {
                const [x, y] = this._states.position;
                this._window.move_frame(true, x, y);
            }
        }));

        /*
         * This is the same minimized-source workaround used by the legacy
         * wallpaper window manager. On affected Mutter versions, minimizing a
         * Wayland window can offset the MetaSurfaceContainerActorWayland inside
         * the window actor. The wallpaper clone samples that actor directly, so
         * keeping the surface container pinned at 0,0 preserves the expected
         * monitor-aligned source geometry while the real helper window remains
         * hidden from the user.
         */
        if (shellVersion === 45) {
            const windowActor = window.get_compositor_private?.();
            const surfaceContainer = windowActor?.get_children?.().find(
                child => GObject.type_name(child) === 'MetaSurfaceContainerActorWayland'
            );
            if (surfaceContainer) {
                this._surfaceContainer = surfaceContainer;
                this._notifyPositionId = surfaceContainer.connect('notify::position', () => {
                    surfaceContainer.set_position(0, 0);
                });
            }
        }

        this._parseTitle();
    }

    disconnect() {
        if (this._lowerIdleId) {
            GLib.source_remove(this._lowerIdleId);
            this._lowerIdleId = 0;
        }

        if (this._notifyPositionId && this._surfaceContainer) {
            try {
                this._surfaceContainer.disconnect(this._notifyPositionId);
            } catch (_e) {
            }
        }

        for (const signalId of this._signals) {
            try {
                this._window.disconnect(signalId);
            } catch (_e) {
            }
        }
        this._signals = [];
        this._notifyPositionId = 0;
        this._surfaceContainer = null;
        this._window = null;
    }

    _parseTitle() {
        const title = this._window?.title;
        if (!title?.startsWith(Wallpaper.TITLE_PREFIX))
            return;

        const afterPrefix = title.slice(Wallpaper.TITLE_PREFIX.length);
        const pipeIndex = afterPrefix.indexOf('|');
        const stateText = pipeIndex >= 0 ? afterPrefix.slice(0, pipeIndex) : afterPrefix;
        try {
            this._states = {
                ...this._states,
                ...JSON.parse(stateText),
            };
        } catch (error) {
            logger.warn(`failed to parse helper window title state: ${error}`);
        }
        this._refresh();
    }

    _refresh() {
        if (!this._window)
            return;

        if (this._states.keepAtBottom && this._window.above)
            this._window.unmake_above();

        if (this._states.keepMinimized && !this._window.minimized) {
            this._window.minimize();
        } else if (this._states.keepAtBottom && !this._window.minimized && !this._lowerIdleId) {
            /*
             * Mutter may not assign a stack position until after map. Lowering
             * from idle keeps the helper out of the user's window stack without
             * tripping compositor assertions during the map sequence.
             */
            this._lowerIdleId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                this._lowerIdleId = 0;
                try {
                    this._window?.lower();
                } catch (_e) {
                }
                return GLib.SOURCE_REMOVE;
            });
        }

        if (this._states.keepPosition) {
            const [x, y] = this._states.position;
            this._window.move_frame(true, x, y);
        }
    }
}

export class WindowManager {
    constructor() {
        this._mapId = 0;
        this._managed = new Map();
    }

    enable() {
        if (this._mapId)
            return;

        this._mapId = global.window_manager.connect_after('map', (_wm, actor) => {
            const window = actor.get_meta_window?.();
            if (window)
                this._maybeManage(window);
        });

        for (const actor of global.get_window_actors()) {
            const window = actor.meta_window;
            if (window)
                this._maybeManage(window);
        }
    }

    disable() {
        if (this._mapId) {
            global.window_manager.disconnect(this._mapId);
            this._mapId = 0;
        }

        for (const managed of this._managed.values())
            managed.disconnect();
        this._managed.clear();
    }

    _maybeManage(window) {
        if (this._managed.has(window))
            return;
        if (!window.title?.startsWith(Wallpaper.TITLE_PREFIX))
            return;

        const managed = new ManagedHelperWindow(window);
        this._managed.set(window, managed);
        const unmanagedId = window.connect('unmanaged', unmanagedWindow => {
            try {
                unmanagedWindow.disconnect(unmanagedId);
            } catch (_e) {
            }
            managed.disconnect();
            this._managed.delete(unmanagedWindow);
        });
    }
}
