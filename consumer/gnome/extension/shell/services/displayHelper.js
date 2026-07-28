import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import * as BuildConfig from '../../buildConfig.js';
import * as Logger from '../logger.js';
import {shellMonitorConnector as resolveShellMonitorConnector} from './monitorIdentity.js';

export {shellMonitorConnector} from './monitorIdentity.js';

const logger = new Logger.Logger('displayHelper');
const encoder = new TextEncoder();

const moduleFile = GLib.filename_from_uri(import.meta.url)[0];
const moduleDir = GLib.path_get_dirname(moduleFile);
const extensionDir = GLib.path_get_dirname(GLib.path_get_dirname(moduleDir));
const helperPath = GLib.build_filenamev([extensionDir, 'shell', 'helper', 'display-helper.js']);
const HELPER_WRITE_WARN_USEC = 50_000;
const HELPER_RESTART_DELAY_MS = 1000;

const resolveExtensionPath = path => {
    if (!path || GLib.path_is_absolute(path))
        return path;
    return GLib.build_filenamev([extensionDir, path]);
};

const prependEnv = (launcher, name, value) => {
    if (!value)
        return;

    const oldValue = GLib.getenv(name);
    launcher.setenv(name, oldValue && oldValue.length > 0 ? `${value}:${oldValue}` : value, true);
};

const defaultSocketPath = () => {
    const runtimeDir = GLib.getenv('XDG_RUNTIME_DIR') || GLib.get_tmp_dir();
    return GLib.build_filenamev([runtimeDir, 'vivid', 'display-v1.sock']);
};

const bytesToProcArgs = bytes => {
    const data = bytes.get_data();
    const args = [];
    let start = 0;
    for (let i = 0; i <= data.length; i++) {
        if (i < data.length && data[i] !== 0)
            continue;

        if (i > start) {
            let arg = '';
            for (let j = start; j < i; j++)
                arg += String.fromCharCode(data[j]);
            args.push(arg);
        }
        start = i + 1;
    }
    return args;
};

const isImportantHelperLine = line =>
    /\b(register|bound|connected|accepted|first frame|slow|failed|error|media|thumbnail|MPRIS|DMA-BUF|FRAME_READY|UNBIND|closed|monitor layout|monitor count|monitor\[)\b/i
        .test(line);

const formatUsec = usec => `${(Number(usec) / 1000).toFixed(2)}ms`;

const monitorObjectString = (object, keys) => {
    for (const key of keys) {
        const value = object?.[key];
        if (typeof value === 'string' && value.trim())
            return value.trim();
    }
    return '';
};

const shellMonitorConnector = (index, layout) =>
    resolveShellMonitorConnector(index, layout);

const shellMonitorDisplayName = (index, layout) => {
    const parts = [];
    const append = value => {
        const text = typeof value === 'string' ? value.trim() : '';
        if (text && !parts.includes(text))
            parts.push(text);
    };

    /*
     * Mutter has exposed monitor identity through different API shapes across
     * GNOME releases. The helper process cannot safely call Shell-only APIs, so
     * collect the best display name here and pass it over the helper command
     * line together with the authoritative Shell monitor geometry.
     */
    for (const getter of [
        'get_monitor_connector',
        'get_monitor_vendor',
        'get_monitor_product',
        'get_monitor_product_serial',
    ]) {
        try {
            if (typeof global.display?.[getter] === 'function')
                append(global.display[getter](index));
        } catch (_e) {
        }
    }

    append(monitorObjectString(layout, ['connector', 'output', 'name']));
    append(monitorObjectString(layout, ['manufacturer', 'vendor']));
    append(monitorObjectString(layout, ['model', 'product']));
    return parts.join(' ').trim();
};

export class DisplayHelperService {
    constructor({socketPath = null} = {}) {
        this.socketPath = socketPath || defaultSocketPath();
        this._subprocess = null;
        this._stdin = null;
        this._stdout = null;
        this._stdoutStream = null;
        this._cancellable = new Gio.Cancellable();
        this._started = false;
        this._writeQueue = [];
        this._writePending = false;
        this._restartSourceId = 0;
        this._runSerial = 0;
        this._helperPid = null;
        this._monitorsChangedId = 0;
        this._outputLayoutJson = '[]';
    }

    start() {
        if (this._started)
            return;

        this._started = true;
        this._runSerial++;
        if (this._cancellable.is_cancelled())
            this._cancellable = new Gio.Cancellable();
        this._connectMonitorSignals();
        this._reapStaleHelpers('startup');
        this._spawn();
    }

    stop() {
        this._started = false;
        this._runSerial++;
        this._disconnectMonitorSignals();
        this._clearRestartSource();
        try {
            this._cancellable.cancel();
        } catch (_e) {
        }

        const subprocess = this._subprocess;
        const helperPid = this._helperPid;
        this._writeQueue = [];
        this._writePending = false;
        this._stdin = null;
        this._stdout = null;
        this._stdoutStream = null;
        this._subprocess = null;
        this._helperPid = null;

        this._terminateSubprocess(subprocess, helperPid, 'service-stop');
        this._reapStaleHelpers('service-stop');
    }

    sendPointerEvent(event) {
        if (!event)
            return false;

        const type = String(event.type ?? '');
        const monitorIndex = Number(event.monitorIndex);
        if (!Number.isFinite(monitorIndex))
            return false;

        const queueKey = type === 'mousemove' ? `pointer-motion:${monitorIndex}` : null;
        return this._queueMessage({type: 'pointer', event}, queueKey);
    }

    sendWindowState(payload) {
        return this._queueMessage({type: 'window-state', payload: payload ?? {}});
    }

    _spawn() {
        const gjs = GLib.find_program_in_path('gjs') ?? 'gjs';
        const outputsJson = this._buildOutputLayoutJson();
        this._outputLayoutJson = outputsJson;
        const launcher = new Gio.SubprocessLauncher({
            flags: Gio.SubprocessFlags.STDIN_PIPE |
                Gio.SubprocessFlags.STDOUT_PIPE |
                Gio.SubprocessFlags.STDERR_MERGE,
        });

        launcher.set_cwd(extensionDir);
        prependEnv(launcher,
            'GI_TYPELIB_PATH',
            resolveExtensionPath(BuildConfig.displayConsumerTypelibDir));
        prependEnv(launcher,
            'LD_LIBRARY_PATH',
            resolveExtensionPath(BuildConfig.displayConsumerLibDir));

        const argv = [
            gjs,
            '-m',
            helperPath,
            '--socket',
            this.socketPath,
            '--outputs-json',
            outputsJson,
        ];

        const runSerial = this._runSerial;

        try {
            const subprocess = launcher.spawnv(argv);
            this._subprocess = subprocess;
            this._helperPid = subprocess.get_identifier?.() ?? null;
            this._stdin = subprocess.get_stdin_pipe();
            this._stdout = subprocess.get_stdout_pipe();
            this._stdoutStream = Gio.DataInputStream.new(this._stdout);
            this._readOutput();
            subprocess.wait_async(this._cancellable, (process, result) => {
                try {
                    process.wait_finish(result);
                } catch (_e) {
                }

                if (this._subprocess === process) {
                    this._stdin = null;
                    this._stdout = null;
                    this._stdoutStream = null;
                    this._subprocess = null;
                    this._helperPid = null;
                }

                /*
                 * GNOME Shell can call disable while an async wait callback from
                 * the previous helper is still queued. Keep the callback tied to
                 * the start generation that created it, otherwise an old helper
                 * exit can schedule a new process after the extension has been
                 * disabled or immediately re-enabled.
                 */
                if (this._started && runSerial === this._runSerial) {
                    logger.warn('display helper exited; restarting');
                    this._scheduleSpawnRetry('helper-exited', runSerial);
                }
            });
            logger.warn(`display helper started pid=${this._helperPid ?? 'unknown'} ` +
                `socket=${this.socketPath} outputs=${this._outputLayoutCount(outputsJson)}`);
        } catch (error) {
            logger.warn(`failed to start display helper: ${error}`);
            this._scheduleSpawnRetry('spawn-failed', runSerial);
        }
    }

    _connectMonitorSignals() {
        if (this._monitorsChangedId)
            return;

        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed', () => {
            const outputsJson = this._buildOutputLayoutJson();
            if (outputsJson === this._outputLayoutJson)
                return;

            logger.warn(`display helper monitor layout changed ` +
                `old=${this._outputLayoutCount(this._outputLayoutJson)} ` +
                `new=${this._outputLayoutCount(outputsJson)}`);
            this._outputLayoutJson = outputsJson;
            this._restartHelper('monitors-changed');
        });
    }

    _disconnectMonitorSignals() {
        if (!this._monitorsChangedId)
            return;

        try {
            Main.layoutManager.disconnect(this._monitorsChangedId);
        } catch (_e) {
        }
        this._monitorsChangedId = 0;
    }

    _buildOutputLayoutJson() {
        const outputs = [];
        const shellMonitorCount = Number(global.display?.get_n_monitors?.() ?? 0);
        const layoutMonitors = Main.layoutManager?.monitors ?? [];
        const count = Math.max(shellMonitorCount, layoutMonitors.length);
        for (let index = 0; index < count; index++) {
            const layout = layoutMonitors[index];
            let geometry = layout;
            if (!geometry) {
                try {
                    geometry = global.display.get_monitor_geometry(index);
                } catch (_e) {
                    geometry = null;
                }
            }
            if (!geometry)
                continue;

            let scale = 1;
            try {
                scale = Number(global.display.get_monitor_scale(index));
            } catch (_e) {
                scale = Number(layout?.scale ?? 1);
            }
            if (!Number.isFinite(scale) || scale <= 0)
                scale = 1;

            const width = Math.max(1, Math.round(Number(geometry.width) || 1));
            const height = Math.max(1, Math.round(Number(geometry.height) || 1));
            outputs.push({
                consumerOutputId: index + 1,
                monitorIndex: index,
                layoutOutputCount: count,
                displayKey: shellMonitorConnector(index, layout),
                x: Math.round(Number(geometry.x) || 0),
                y: Math.round(Number(geometry.y) || 0),
                width,
                height,
                scale,
                physicalWidth: Math.max(1, Math.round(width * scale)),
                physicalHeight: Math.max(1, Math.round(height * scale)),
                refreshRateMhz: 0,
                desktop: 'gnome-shell-helper',
                displayName: shellMonitorDisplayName(index, layout),
            });
        }
        return JSON.stringify(outputs);
    }

    _outputLayoutCount(json) {
        try {
            const outputs = JSON.parse(json || '[]');
            return Array.isArray(outputs) ? outputs.length : 0;
        } catch (_e) {
            return 0;
        }
    }

    _scheduleSpawnRetry(reason, runSerial = this._runSerial) {
        if (!this._started || this._restartSourceId)
            return;

        this._restartSourceId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT,
            HELPER_RESTART_DELAY_MS,
            () => {
                this._restartSourceId = 0;
                if (this._started && runSerial === this._runSerial)
                    this._spawn();
                return GLib.SOURCE_REMOVE;
            }
        );
        logger.warn(`display helper spawn retry scheduled reason=${reason} delay=${HELPER_RESTART_DELAY_MS}ms`);
    }

    _restartHelper(reason) {
        if (!this._started)
            return;

        const subprocess = this._subprocess;
        const helperPid = this._helperPid;
        this._runSerial++;
        this._clearRestartSource();
        this._writeQueue = [];
        this._writePending = false;
        this._stdin = null;
        this._stdout = null;
        this._stdoutStream = null;
        this._subprocess = null;
        this._helperPid = null;

        this._terminateSubprocess(subprocess, helperPid, reason);
        this._reapStaleHelpers(reason);
        this._spawn();
    }

    _queueMessage(message, queueKey = null) {
        if (!this._started || !this._stdin)
            return false;

        const bytes = encoder.encode(`${JSON.stringify(message)}\n`);
        if (queueKey) {
            for (let i = this._writeQueue.length - 1; i >= 0; i--) {
                if (this._writeQueue[i]?.queueKey !== queueKey)
                    continue;
                this._writeQueue[i] = {
                    bytes,
                    queueKey,
                    queuedAtUsec: GLib.get_monotonic_time(),
                };
                this._flushWriteQueue();
                return true;
            }
        }

        this._writeQueue.push({
            bytes,
            queueKey,
            queuedAtUsec: GLib.get_monotonic_time(),
        });
        this._flushWriteQueue();
        return true;
    }

    _flushWriteQueue() {
        if (!this._stdin || this._writePending || this._writeQueue.length === 0)
            return;

        const item = this._writeQueue[0];
        const writeStartUsec = GLib.get_monotonic_time();
        this._writePending = true;
        this._stdin.write_all_async(
            item.bytes,
            GLib.PRIORITY_DEFAULT,
            this._cancellable,
            (stream, result) => {
                if (stream !== this._stdin)
                    return;

                try {
                    stream.write_all_finish(result);
                    this._writeQueue.shift();
                } catch (error) {
                    if (!error.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        logger.warn(`display helper stdin write failed: ${error}`);
                    this._writeQueue = [];
                }

                const nowUsec = GLib.get_monotonic_time();
                const writeUsec = nowUsec - writeStartUsec;
                const queuedUsec = nowUsec - (item.queuedAtUsec ?? writeStartUsec);
                /*
                 * Shell owns pointer capture and desktop fact collection, while
                 * the helper owns socket IO to the producer. A slow stdin write is
                 * useful evidence that Shell is spending time feeding the helper
                 * instead of repainting the desktop clone. Only warn on threshold
                 * breaches so normal coalesced pointer traffic stays quiet.
                 */
                if (queuedUsec > HELPER_WRITE_WARN_USEC) {
                    logger.warn(`display helper stdin write slow queued=${formatUsec(queuedUsec)} ` +
                        `write=${formatUsec(writeUsec)} remaining=${this._writeQueue.length}`);
                }

                this._writePending = false;
                this._flushWriteQueue();
            }
        );
    }

    _readOutput() {
        const stream = this._stdoutStream;
        if (!stream)
            return;

        stream.read_line_async(GLib.PRIORITY_DEFAULT, this._cancellable, (source, result) => {
            if (source !== this._stdoutStream)
                return;

            try {
                const [line, length] = source.read_line_finish_utf8(result);
                if (length > 0) {
                    if (isImportantHelperLine(line))
                        logger.warn(`[helper] ${line}`);
                    else
                        logger.debug(`[helper] ${line}`);
                }
            } catch (error) {
                if (!error.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                    logger.warn(`display helper stdout read failed: ${error}`);
                return;
            }

            this._readOutput();
        });
    }

    _clearRestartSource() {
        if (!this._restartSourceId)
            return;

        try {
            GLib.source_remove(this._restartSourceId);
        } catch (_e) {
        }
        this._restartSourceId = 0;
    }

    _terminateSubprocess(subprocess, pid, reason) {
        if (!subprocess)
            return;

        try {
            logger.warn(`display helper stop requested pid=${pid ?? 'unknown'} reason=${reason}`);
            subprocess.force_exit();
        } catch (error) {
            logger.warn(`display helper stop failed pid=${pid ?? 'unknown'} reason=${reason}: ${error}`);
        }
    }

    _reapStaleHelpers(reason) {
        const procDir = Gio.File.new_for_path('/proc');
        if (!procDir.query_exists(null))
            return;

        let enumerator = null;
        try {
            enumerator = procDir.enumerate_children(
                'standard::name',
                Gio.FileQueryInfoFlags.NONE,
                null
            );
        } catch (error) {
            logger.warn(`display helper stale scan failed reason=${reason}: ${error}`);
            return;
        }

        /*
         * A helper can outlive the service object if GNOME Shell reloads the
         * extension while async subprocess callbacks are still pending. The
         * match is intentionally narrow: only a gjs command line containing this
         * exact installed helper path is reaped, so unrelated gjs services and
         * producer-side processes are left alone.
         */
        let info;
        while ((info = enumerator.next_file(null))) {
            const pid = info.get_name();
            if (!/^\d+$/.test(pid))
                continue;

            const cmdlineFile = Gio.File.new_for_path(GLib.build_filenamev(['/proc', pid, 'cmdline']));
            if (!cmdlineFile.query_exists(null))
                continue;

            let args;
            try {
                const [bytes] = cmdlineFile.load_bytes(null);
                args = bytesToProcArgs(bytes);
            } catch (_e) {
                continue;
            }

            if (!args.includes(helperPath))
                continue;

            try {
                const proc = new Gio.Subprocess({
                    argv: ['/bin/kill', '-KILL', pid],
                    flags: Gio.SubprocessFlags.NONE,
                });
                proc.init(null);
                proc.wait(null);
                logger.warn(`reaped stale display helper pid=${pid} reason=${reason}`);
            } catch (error) {
                logger.warn(`failed to reap stale display helper pid=${pid} reason=${reason}: ${error}`);
            }
        }

        try {
            enumerator.close(null);
        } catch (_e) {
        }
    }
}
