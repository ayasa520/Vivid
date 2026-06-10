const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;

var MPRIS_DBUS_NAME = 'org.freedesktop.DBus';
var MPRIS_DBUS_PATH = '/org/freedesktop/DBus';
var MPRIS_PLAYER_PATH = '/org/mpris/MediaPlayer2';
var MPRIS_PLAYER_INTERFACE = 'org.mpris.MediaPlayer2.Player';
var DBUS_PROPERTIES_INTERFACE = 'org.freedesktop.DBus.Properties';
var MPRIS_SYNC_QUERY_TIMEOUT_MS = 250;

function isMprisPlayerName(name) {
    return typeof name === 'string' && name.startsWith('org.mpris.MediaPlayer2.');
}

function _callDbusAsync(connection, busName, objectPath, interfaceName, methodName, parameters, replyType) {
    return new Promise((resolve, reject) => {
        connection.call(
            busName,
            objectPath,
            interfaceName,
            methodName,
            parameters,
            replyType,
            Gio.DBusCallFlags.NONE,
            -1,
            null,
            (source, result) => {
                try {
                    resolve(source.call_finish(result));
                } catch (e) {
                    reject(e);
                }
            }
        );
    });
}

function _newPlayerProxyAsync(connection, name) {
    return new Promise((resolve, reject) => {
        Gio.DBusProxy.new(
            connection,
            Gio.DBusProxyFlags.NONE,
            null,
            name,
            MPRIS_PLAYER_PATH,
            MPRIS_PLAYER_INTERFACE,
            null,
            (_source, result) => {
                try {
                    resolve(Gio.DBusProxy.new_finish(result));
                } catch (e) {
                    reject(e);
                }
            }
        );
    });
}

async function listMprisPlayerNames(connection = Gio.DBus.session) {
    try {
        const result = await _callDbusAsync(
            connection,
            MPRIS_DBUS_NAME,
            MPRIS_DBUS_PATH,
            MPRIS_DBUS_NAME,
            'ListNames',
            null,
            new GLib.VariantType('(as)')
        );
        const [names] = result.deep_unpack();
        return names.filter(name => isMprisPlayerName(name));
    } catch (_e) {
        return [];
    }
}

function _callDbusSync(connection, busName, objectPath, interfaceName, methodName, parameters, replyType) {
    return connection.call_sync(
        busName,
        objectPath,
        interfaceName,
        methodName,
        parameters,
        replyType,
        Gio.DBusCallFlags.NONE,
        MPRIS_SYNC_QUERY_TIMEOUT_MS,
        null
    );
}

function listMprisPlayerNamesSync(connection = Gio.DBus.session) {
    try {
        const result = _callDbusSync(
            connection,
            MPRIS_DBUS_NAME,
            MPRIS_DBUS_PATH,
            MPRIS_DBUS_NAME,
            'ListNames',
            null,
            new GLib.VariantType('(as)')
        );
        const [names] = result.deep_unpack();
        return names.filter(name => isMprisPlayerName(name));
    } catch (_e) {
        return [];
    }
}

async function queryConnectionUnixProcessId(name, connection = Gio.DBus.session) {
    if (!isMprisPlayerName(name))
        return null;

    try {
        const result = await _callDbusAsync(
            connection,
            MPRIS_DBUS_NAME,
            MPRIS_DBUS_PATH,
            MPRIS_DBUS_NAME,
            'GetConnectionUnixProcessID',
            new GLib.Variant('(s)', [name]),
            new GLib.VariantType('(u)')
        );
        const [pid] = result.deep_unpack();
        return Number.isFinite(pid) ? pid : null;
    } catch (_e) {
        return null;
    }
}

function _deepUnpack(value) {
    return value?.deep_unpack?.() ?? value;
}

function _normalizeArtists(value) {
    const unpacked = _deepUnpack(value) ?? [];
    if (Array.isArray(unpacked))
        return unpacked.filter(Boolean).join(', ');
    return String(unpacked ?? '');
}

function _normalizeString(value) {
    const unpacked = _deepUnpack(value);
    return String(unpacked ?? '');
}

function buildMprisPlayerSnapshot(name, proxy) {
    const playbackStatus = _deepUnpack(proxy?.get_cached_property?.('PlaybackStatus')) ?? '';
    const metadata = _deepUnpack(proxy?.get_cached_property?.('Metadata')) ?? {};
    return buildMprisPlayerSnapshotFromProperties(name, {
        PlaybackStatus: playbackStatus,
        Metadata: metadata,
    });
}

function buildMprisPlayerSnapshotFromProperties(name, properties) {
    const playbackStatus = _deepUnpack(properties?.PlaybackStatus) ?? '';
    const metadata = _deepUnpack(properties?.Metadata) ?? {};
    const title = _normalizeString(metadata['xesam:title']);
    const artist = _normalizeArtists(metadata['xesam:artist']);
    const albumArtist = _normalizeArtists(metadata['xesam:albumArtist']);
    const artUrl = _normalizeString(metadata['mpris:artUrl']);
    const score =
        playbackStatus === 'Playing' ? 3 :
        playbackStatus === 'Paused' ? 2 :
        (title || artist || artUrl) ? 1 : 0;

    return {
        name,
        playbackStatus,
        title,
        artist,
        albumTitle: _normalizeString(metadata['xesam:album']),
        albumArtist,
        subTitle: _normalizeString(metadata['xesam:comment']),
        genres: _normalizeArtists(metadata['xesam:genre']),
        contentType: title || artist || albumArtist ? 'music' : '',
        artUrl,
        score,
    };
}

function queryMprisPlayerSnapshotSync(name, connection = Gio.DBus.session) {
    if (!isMprisPlayerName(name))
        return null;

    try {
        const result = _callDbusSync(
            connection,
            name,
            MPRIS_PLAYER_PATH,
            DBUS_PROPERTIES_INTERFACE,
            'GetAll',
            new GLib.Variant('(s)', [MPRIS_PLAYER_INTERFACE]),
            new GLib.VariantType('(a{sv})')
        );
        const [properties] = result.deep_unpack();
        return buildMprisPlayerSnapshotFromProperties(name, properties);
    } catch (_e) {
        return null;
    }
}

function queryMprisSnapshotsSync(connection = Gio.DBus.session) {
    /*
     * This synchronous path is intentionally short-timeout and side-effect free.
     * The regular monitor still handles DBus signals, while display-helper poll
     * uses this as a deterministic recovery path when async callbacks are delayed
     * inside a busy GTK render process.
     */
    return listMprisPlayerNamesSync(connection)
        .map(name => queryMprisPlayerSnapshotSync(name, connection))
        .filter(snapshot => snapshot !== null)
        .sort((a, b) => b.score - a.score || a.name.localeCompare(b.name));
}

var MprisMonitor = class {
    constructor(params = {}) {
        this._connection = params.connection ?? Gio.DBus.session;
        this._onChanged = typeof params.onChanged === 'function' ? params.onChanged : null;
        this._warn = typeof params.warn === 'function' ? params.warn : null;
        this._players = new Map();
        this._trackedPlayers = new Set();
        this._snapshots = [];
        this._activePlayerName = null;
        this._nameOwnerChangedId = 0;
        this._destroyed = false;

        this._loadInitialPlayers().catch(e => {
            if (!this._destroyed)
                this._warn?.(`failed to enumerate MPRIS players: ${e}`);
        });
        this._nameOwnerChangedId = this._connection.signal_subscribe(
            MPRIS_DBUS_NAME,
            MPRIS_DBUS_NAME,
            'NameOwnerChanged',
            MPRIS_DBUS_PATH,
            null,
            Gio.DBusSignalFlags.NONE,
            (_connection, _senderName, _objectPath, _interfaceName, _signalName, parameters) => {
                const [name, _oldOwner, newOwner] = parameters.deep_unpack();
                if (!isMprisPlayerName(name))
                    return;

                if (newOwner)
                    void this._addPlayer(name);
                else
                    this._removePlayer(name);
            }
        );
        this._recompute();
    }

    destroy() {
        this._destroyed = true;
        if (this._nameOwnerChangedId) {
            this._connection.signal_unsubscribe(this._nameOwnerChangedId);
            this._nameOwnerChangedId = 0;
        }

        for (const {proxy, signalId} of this._players.values()) {
            try {
                proxy.disconnect(signalId);
            } catch (_e) {
            }
        }

        this._players.clear();
        this._trackedPlayers.clear();
        this._snapshots = [];
    }

    getSnapshots() {
        return this._snapshots.map(snapshot => ({...snapshot}));
    }

    getActiveSnapshot() {
        const active = this._findSnapshot(this._activePlayerName) ?? this._snapshots[0] ?? null;
        return active ? {...active} : null;
    }

    _findSnapshot(name, snapshots = this._snapshots) {
        if (!name)
            return null;

        return snapshots.find(snapshot => snapshot.name === name) ?? null;
    }

    async _loadInitialPlayers() {
        const names = await listMprisPlayerNames(this._connection);
        names.forEach(name => {
            void this._addPlayer(name);
        });
    }

    async _addPlayer(name) {
        if (this._trackedPlayers.has(name))
            return;

        this._trackedPlayers.add(name);

        try {
            const proxy = await _newPlayerProxyAsync(this._connection, name);
            if (this._destroyed || !this._trackedPlayers.has(name)) {
                try {
                    proxy.run_dispose?.();
                } catch (_e) {
                }
                return;
            }

            const signalId = proxy.connect('g-properties-changed', () => this._recompute());
            this._players.set(name, {proxy, signalId});
            this._recompute();
        } catch (e) {
            this._trackedPlayers.delete(name);
            this._warn?.(`failed to watch ${name}: ${e}`);
        }
    }

    _removePlayer(name) {
        this._trackedPlayers.delete(name);
        const entry = this._players.get(name);
        if (!entry)
            return;

        try {
            entry.proxy.disconnect(entry.signalId);
        } catch (_e) {
        }

        this._players.delete(name);
        this._recompute();
    }

    _recompute() {
        const previousActive = this._findSnapshot(this._activePlayerName);
        const nextSnapshots = [...this._players.entries()]
            .map(([name, {proxy}]) => buildMprisPlayerSnapshot(name, proxy))
            .sort((a, b) => b.score - a.score || a.name.localeCompare(b.name));
        const bestSnapshot = nextSnapshots[0] ?? null;
        const stickySnapshot = this._findSnapshot(this._activePlayerName, nextSnapshots);
        const nextActive =
            stickySnapshot && stickySnapshot.score > 0 &&
            (!bestSnapshot || stickySnapshot.score >= bestSnapshot.score)
                ? stickySnapshot
                : bestSnapshot;

        this._snapshots = nextSnapshots;
        this._activePlayerName = nextActive?.name ?? null;

        if ((previousActive?.name ?? null) !== (nextActive?.name ?? null)) {
            this._warn?.(
                `active player switched: ${previousActive?.name ?? '(none)'} ` +
                `(${previousActive?.playbackStatus ?? '-'}:${previousActive?.score ?? 0}) -> ` +
                `${nextActive?.name ?? '(none)'} ` +
                `(${nextActive?.playbackStatus ?? '-'}:${nextActive?.score ?? 0})`
            );
        }

        this._onChanged?.({
            active: this.getActiveSnapshot(),
            snapshots: this.getSnapshots(),
        });
    }
};
