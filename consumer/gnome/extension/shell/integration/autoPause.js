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

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import UPower from 'gi://UPowerGlib';
import * as Config from 'resource:///org/gnome/shell/misc/config.js';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import * as Logger from '../logger.js';
import * as Wallpaper from '../ui/wallpaper.js';

const logger = new Logger.Logger('desktopFacts');
const moduleDir = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const commonDir = GLib.build_filenamev([moduleDir, '..', '..', 'common']);
if (!imports.searchPath.some(path => path === commonDir))
    imports.searchPath.unshift(commonDir);

const Mpris = imports.mpris;
const shellVersion = parseInt(Config.PACKAGE_VERSION.split('.')[0]);

const FACT_SCHEMA = 'display-window-state-v1';
const FACT_REFRESH_WARN_USEC = 8_000;
const FACT_REPORT_WARN_USEC = 10_000;

function formatUsec(usec) {
    return `${(Number(usec) / 1000).toFixed(2)}ms`;
}

function warnSlowFactWork(name, startedUsec, detail = '') {
    const elapsedUsec = GLib.get_monotonic_time() - startedUsec;
    if (elapsedUsec <= FACT_REFRESH_WARN_USEC)
        return;

    logger.warn(`desktop facts slow ${name} duration=${formatUsec(elapsedUsec)}${detail}`);
}

function normalizeIdentifier(value) {
    if (typeof value !== 'string')
        return '';
    return value.trim().toLowerCase();
}

function uniqueStrings(values) {
    return [...new Set(values.map(normalizeIdentifier).filter(Boolean))];
}

function safeGetString(target, methodName) {
    if (!target || typeof target[methodName] !== 'function')
        return '';

    try {
        return target[methodName]() ?? '';
    } catch (error) {
        logger.trace(error);
        return '';
    }
}

function readProcessIdentifiers(pid) {
    if (!Number.isInteger(pid) || pid <= 0)
        return [];

    const identifiers = [];
    const pushPathVariants = path => {
        if (!path)
            return;

        identifiers.push(path);
        identifiers.push(GLib.path_get_basename(path));
    };

    try {
        const cmdlinePath = GLib.build_filenamev(['/proc', `${pid}`, 'cmdline']);
        const cmdlineFile = Gio.File.new_for_path(cmdlinePath);
        const [binaryData] = cmdlineFile.load_bytes(null);
        const argv = new TextDecoder()
            .decode(binaryData.get_data())
            .split('\u0000')
            .filter(Boolean);
        pushPathVariants(argv[0] ?? '');
    } catch (error) {
        logger.trace(error);
    }

    try {
        const exePath = GLib.build_filenamev(['/proc', `${pid}`, 'exe']);
        const exeFile = Gio.File.new_for_path(exePath);
        const info = exeFile.query_info(
            'standard::symlink-target',
            Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS,
            null
        );
        pushPathVariants(info.get_symlink_target() ?? '');
    } catch (error) {
        logger.trace(error);
    }

    return uniqueStrings(identifiers);
}

function connectTracked(handles, object, signal, callback) {
    if (!object || typeof object.connect !== 'function')
        return 0;

    try {
        const id = object.connect(signal, callback);
        handles.push([object, id]);
        return id;
    } catch (error) {
        logger.trace(error);
        return 0;
    }
}

function disconnectTracked(handles) {
    for (const [object, id] of handles) {
        try {
            object.disconnect(id);
        } catch (error) {
            logger.trace(error);
        }
    }
    handles.length = 0;
}

function windowIsReportable(metaWindow) {
    /*
     * Helper windows are consumer implementation details. They must stay
     * visible to the wallpaper clone lookup, but they must never enter the
     * producer's desktop facts; otherwise pause/stop policy could react to the
     * display sink process itself instead of real user applications.
     */
    return !!metaWindow &&
        !metaWindow.skip_taskbar &&
        !metaWindow.title?.startsWith(Wallpaper.TITLE_PREFIX);
}

function windowIsVisible(metaWindow) {
    return windowIsReportable(metaWindow) && !metaWindow.minimized;
}

function windowIsMaximizedOrFullscreen(metaWindow) {
    if (!windowIsVisible(metaWindow))
        return false;

    if (shellVersion < 49)
        return metaWindow.get_maximized() === Meta.MaximizeFlags.BOTH || metaWindow.fullscreen;

    return metaWindow.is_maximized() || metaWindow.fullscreen;
}

function buildWindowInfo(metaWindow, windowTracker, processIdentifiers = []) {
    const app = windowTracker?.get_window_app(metaWindow) ?? null;
    const originalIdentifiers = [
        app?.get_id?.() ?? '',
        app?.get_name?.() ?? '',
        safeGetString(metaWindow, 'get_gtk_application_id'),
        safeGetString(metaWindow, 'get_wm_class'),
        safeGetString(metaWindow, 'get_wm_class_instance'),
        ...processIdentifiers,
    ].filter(Boolean);

    return {
        title: metaWindow.title ?? '',
        pid: Number(metaWindow.get_pid?.() ?? 0),
        monitorIndex: Number(metaWindow.get_monitor?.() ?? -1),
        focused: !!metaWindow.appears_focused,
        fullscreen: !!metaWindow.fullscreen,
        maximized: windowIsMaximizedOrFullscreen(metaWindow),
        minimized: !!metaWindow.minimized,
        identifiers: uniqueStrings(originalIdentifiers),
        originalIdentifiers,
    };
}

class FactModule {
    constructor(name, onUpdated) {
        this.name = name;
        this._onUpdated = onUpdated;
        this._signalHandles = [];
        this._facts = {};
    }

    enable() {}

    snapshot() {
        return this._facts;
    }

    _updated() {
        this._onUpdated?.(this.name);
    }

    _connect(object, signal, callback) {
        return connectTracked(this._signalHandles, object, signal, callback);
    }

    disable() {
        disconnectTracked(this._signalHandles);
        this._facts = {};
    }
}

class CoveringWindowFactModule extends FactModule {
    constructor(onUpdated) {
        super('coveringWindows', onUpdated);
        this._workspaceManager = null;
        this._activeWorkspace = null;
        this._workspaceSignalHandles = [];
        this._trackedWindows = new Map();
    }

    enable() {
        this._workspaceManager = global.workspace_manager;
        this._connect(this._workspaceManager, 'active-workspace-changed', () => {
            this._trackActiveWorkspace();
            this._refresh();
        });
        this._trackActiveWorkspace();
        this._refresh();
    }

    _trackActiveWorkspace() {
        disconnectTracked(this._workspaceSignalHandles);
        this._untrackAllWindows();

        this._activeWorkspace = this._workspaceManager?.get_active_workspace?.() ?? null;
        if (!this._activeWorkspace)
            return;

        for (const metaWindow of this._activeWorkspace.list_windows())
            this._trackWindow(metaWindow, false);

        connectTracked(this._workspaceSignalHandles, this._activeWorkspace, 'window-added',
            (_workspace, metaWindow) => {
                this._trackWindow(metaWindow);
                this._refresh();
            });
        connectTracked(this._workspaceSignalHandles, this._activeWorkspace, 'window-removed',
            (_workspace, metaWindow) => {
                this._untrackWindow(metaWindow);
                this._refresh();
            });
    }

    _trackWindow(metaWindow, emitUpdate = true) {
        if (!windowIsReportable(metaWindow) || this._trackedWindows.has(metaWindow))
            return;

        const signals = [];
        for (const propertyName of [
            'maximized-horizontally',
            'maximized-vertically',
            'fullscreen',
            'minimized',
        ]) {
            connectTracked(signals, metaWindow, `notify::${propertyName}`, () => {
                this._refresh();
                this._updated();
            });
        }

        this._trackedWindows.set(metaWindow, signals);
        if (emitUpdate)
            this._updated();
    }

    _untrackWindow(metaWindow) {
        const signals = this._trackedWindows.get(metaWindow);
        if (!signals)
            return;

        disconnectTracked(signals);
        this._trackedWindows.delete(metaWindow);
    }

    _untrackAllWindows() {
        for (const metaWindow of [...this._trackedWindows.keys()])
            this._untrackWindow(metaWindow);
    }

    _refresh() {
        const visibleWindows = [...this._trackedWindows.keys()].filter(windowIsVisible);
        const coveringWindows = visibleWindows.filter(windowIsMaximizedOrFullscreen);
        const monitors = Main.layoutManager.monitors ?? [];
        const coveredMonitors = new Set(coveringWindows.map(metaWindow => metaWindow.get_monitor()));

        this._facts = {
            maximizedOrFullscreenOnAnyMonitor: coveringWindows.length > 0,
            maximizedOrFullscreenOnAllMonitors:
                monitors.length > 0 && monitors.every(monitor => coveredMonitors.has(monitor.index)),
            coveredMonitorIndices: [...coveredMonitors],
            visibleWindowCount: visibleWindows.length,
        };
    }

    disable() {
        disconnectTracked(this._workspaceSignalHandles);
        this._untrackAllWindows();
        this._workspaceManager = null;
        this._activeWorkspace = null;
        super.disable();
    }
}

class FocusWindowFactModule extends FactModule {
    constructor(onUpdated) {
        super('focusWindow', onUpdated);
        this._display = null;
        this._trackedFocusWindow = null;
        this._focusWindowSignalHandles = [];
        this._windowTracker = Shell.WindowTracker.get_default();
    }

    enable() {
        this._display = global.display;
        this._connect(this._display, 'notify::focus-window', () => {
            this._trackFocusWindow(this._display.focus_window);
            this._refresh();
            this._updated();
        });
        this._trackFocusWindow(this._display.focus_window);
        this._refresh();
    }

    _trackFocusWindow(metaWindow) {
        disconnectTracked(this._focusWindowSignalHandles);
        this._trackedFocusWindow = metaWindow;
        if (!metaWindow)
            return;

        for (const propertyName of ['appears-focused', 'minimized']) {
            connectTracked(this._focusWindowSignalHandles, metaWindow, `notify::${propertyName}`, () => {
                this._refresh();
                this._updated();
            });
        }
        connectTracked(this._focusWindowSignalHandles, metaWindow, 'unmanaged', () => {
            this._trackFocusWindow(this._display?.focus_window ?? null);
            this._refresh();
            this._updated();
        });
    }

    _refresh() {
        const focusWindow = this._display?.focus_window ?? null;
        const pausableFocus = windowIsVisible(focusWindow) && !!focusWindow.appears_focused;

        this._facts = {
            windowFocused: pausableFocus,
            focusWindow: focusWindow
                ? buildWindowInfo(focusWindow, this._windowTracker)
                : null,
        };
    }

    disable() {
        disconnectTracked(this._focusWindowSignalHandles);
        this._display = null;
        this._trackedFocusWindow = null;
        this._windowTracker = null;
        super.disable();
    }
}

class PowerFactModule extends FactModule {
    constructor(onUpdated) {
        super('power', onUpdated);
        this._client = null;
        this._displayDevice = null;
        this._deviceSignalHandles = [];
    }

    enable() {
        try {
            this._client = UPower.Client.new();
        } catch (error) {
            logger.warn(`UPower facts unavailable: ${error}`);
            this._refresh();
            return;
        }

        this._connect(this._client, 'notify::on-battery', () => {
            this._refresh();
            this._updated();
        });
        this._connect(this._client, 'device-added', () => this._refreshDisplayDevice());
        this._connect(this._client, 'device-removed', () => this._refreshDisplayDevice());
        this._refreshDisplayDevice();
    }

    _refreshDisplayDevice() {
        disconnectTracked(this._deviceSignalHandles);
        this._displayDevice = null;

        try {
            this._displayDevice = this._client?.get_display_device?.() ?? null;
        } catch (error) {
            logger.trace(error);
        }

        if (this._displayDevice) {
            for (const propertyName of ['percentage', 'state']) {
                connectTracked(this._deviceSignalHandles, this._displayDevice, `notify::${propertyName}`, () => {
                    this._refresh();
                    this._updated();
                });
            }
        }

        this._refresh();
        this._updated();
    }

    _refresh() {
        let onBattery = false;
        try {
            onBattery = !!this._client?.get_on_battery?.();
        } catch (error) {
            logger.trace(error);
        }

        const percentage = Number(this._displayDevice?.percentage ?? 100);
        const state = Number(this._displayDevice?.state ?? 0);
        this._facts = {
            onBattery,
            batteryPercentage: Number.isFinite(percentage) ? percentage : 100,
            batteryState: Number.isFinite(state) ? state : 0,
        };
    }

    disable() {
        disconnectTracked(this._deviceSignalHandles);
        this._displayDevice = null;
        this._client = null;
        super.disable();
    }
}

class MprisFactModule extends FactModule {
    constructor(onUpdated) {
        super('mpris', onUpdated);
        this._monitor = null;
    }

    enable() {
        this._monitor = new Mpris.MprisMonitor({
            warn: message => logger.debug(message),
            onChanged: ({snapshots}) => {
                this._refresh(snapshots);
                this._updated();
            },
        });
        this._refresh();
    }

    _refresh(snapshots = this._monitor?.getSnapshots?.() ?? []) {
        this._facts = {
            mprisPlaying: snapshots.some(player => player.playbackStatus === 'Playing'),
            mprisPlayers: snapshots.map(({name, playbackStatus}) => ({name, playbackStatus})),
        };
    }

    disable() {
        this._monitor?.destroy?.();
        this._monitor = null;
        super.disable();
    }
}

class WindowInventoryFactModule extends FactModule {
    constructor(onUpdated) {
        super('windowInventory', onUpdated);
        this._display = null;
        this._windowTracker = Shell.WindowTracker.get_default();
        this._trackedWindows = new Map();
    }

    enable() {
        this._display = global.display;
        for (const windowActor of global.get_window_actors())
            this._trackWindow(windowActor?.meta_window, false);

        this._connect(this._display, 'window-created', (_display, metaWindow) => {
            this._trackWindow(metaWindow);
            this._refresh();
            this._updated();
        });

        this._refresh();
    }

    _trackWindow(metaWindow, emitUpdate = true) {
        if (!windowIsReportable(metaWindow) || this._trackedWindows.has(metaWindow))
            return;

        const tracked = {
            signals: [],
            processIdentifiers: readProcessIdentifiers(metaWindow.get_pid?.() ?? 0),
        };

        connectTracked(tracked.signals, metaWindow, 'unmanaged', () => {
            this._untrackWindow(metaWindow);
            this._refresh();
            this._updated();
        });

        for (const propertyName of [
            'title',
            'wm-class',
            'wm-class-instance',
            'gtk-application-id',
            'minimized',
            'appears-focused',
        ]) {
            connectTracked(tracked.signals, metaWindow, `notify::${propertyName}`, () => {
                this._refresh();
                this._updated();
            });
        }

        this._trackedWindows.set(metaWindow, tracked);
        if (emitUpdate)
            this._updated();
    }

    _untrackWindow(metaWindow) {
        const tracked = this._trackedWindows.get(metaWindow);
        if (!tracked)
            return;

        disconnectTracked(tracked.signals);
        this._trackedWindows.delete(metaWindow);
    }

    _refresh() {
        const startedUsec = GLib.get_monotonic_time();
        const windows = [...this._trackedWindows.entries()]
            .filter(([metaWindow]) => windowIsVisible(metaWindow))
            .map(([metaWindow, tracked]) =>
                buildWindowInfo(metaWindow, this._windowTracker, tracked.processIdentifiers));

        this._facts = {
            windows,
            applicationIdentifiers: uniqueStrings(windows.flatMap(window => window.identifiers)),
        };
        warnSlowFactWork('windowInventory.refresh', startedUsec, ` windows=${windows.length}`);
    }

    disable() {
        for (const metaWindow of [...this._trackedWindows.keys()])
            this._untrackWindow(metaWindow);
        this._display = null;
        this._windowTracker = null;
        super.disable();
    }
}

export class AutoPause {
    constructor(consumerOrExtension) {
        this._consumer = consumerOrExtension?.sendWindowState
            ? consumerOrExtension
            : consumerOrExtension?.consumerSocket ?? null;
        this._modules = [];
        this._enabled = false;
        this._reportSourceId = 0;
        this._pendingReasons = new Set();
        this._unsubscribeDisplay = null;
    }

    enable() {
        if (this._enabled)
            return;

        this._enabled = true;
        const schedule = reason => this._scheduleReport(reason);
        this._modules = [
            new CoveringWindowFactModule(schedule),
            new FocusWindowFactModule(schedule),
            new PowerFactModule(schedule),
            new MprisFactModule(schedule),
            new WindowInventoryFactModule(schedule),
        ];

        for (const module of this._modules)
            module.enable();

        this._unsubscribeDisplay = this._consumer?.addDisplayListener?.(event => {
            if (event?.type === 'connected' || event?.type === 'output-accepted')
                this._scheduleReport(event.type);
        }) ?? null;

        this._scheduleReport('enabled');
    }

    disable() {
        this._enabled = false;
        if (this._reportSourceId) {
            GLib.source_remove(this._reportSourceId);
            this._reportSourceId = 0;
        }

        this._unsubscribeDisplay?.();
        this._unsubscribeDisplay = null;

        for (const module of this._modules)
            module.disable();
        this._modules = [];
        this._pendingReasons.clear();
        this._consumer = null;
    }

    _scheduleReport(reason) {
        if (!this._enabled)
            return;

        if (reason)
            this._pendingReasons.add(reason);

        if (this._reportSourceId)
            return;

        this._reportSourceId = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._reportSourceId = 0;
            this._reportNow();
            return GLib.SOURCE_REMOVE;
        });
    }

    _reportNow() {
        const startedUsec = GLib.get_monotonic_time();
        const facts = {};
        for (const module of this._modules)
            Object.assign(facts, module.snapshot());

        const reasons = [...this._pendingReasons];
        this._pendingReasons.clear();

        /*
         * This object deliberately contains observations, not policy decisions.
         * The producer owns configuration and decides whether these facts imply
         * pause, resume, or stop. Keeping the fact schema explicit makes KDE or
         * another desktop consumer implement the same producer contract without
         * copying GNOME-specific policy code.
         */
        const payload = {
            schema: FACT_SCHEMA,
            source: 'gnome-shell',
            timeUsec: GLib.get_monotonic_time(),
            reasons,
            facts,
        };

        const sent = this._consumer?.sendWindowState?.(payload) ?? false;
        /*
         * Auto-pause runs inside GNOME Shell because only the desktop consumer can
         * observe Shell windows, focus, power and MPRIS state. Slow fact reports
         * are logged as performance evidence: if they line up with visible frame
         * hitches, the producer can be ruled out without guessing.
         */
        const elapsedUsec = GLib.get_monotonic_time() - startedUsec;
        if (elapsedUsec > FACT_REPORT_WARN_USEC) {
            logger.warn(`desktop facts report slow duration=${formatUsec(elapsedUsec)} ` +
                `sent=${sent} reasons=${reasons.join(',')} windows=${facts.windows?.length ?? 0}`);
        }
        logger.debug(`desktop facts ${sent ? 'sent' : 'queued until connected'} reasons=${reasons.join(',')}`);
    }
}
