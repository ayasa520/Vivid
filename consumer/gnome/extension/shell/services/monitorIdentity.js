import Gdk from 'gi://Gdk?version=4.0';
import Meta from 'gi://Meta';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const monitorObjectString = (object, keys) => {
    for (const key of keys) {
        const value = object?.[key];
        if (typeof value === 'string' && value.trim())
            return value.trim();
    }
    return '';
};

const normalizeConnector = value =>
    typeof value === 'string' ? value.trim() : '';

const gListToArray = list => {
    if (!list)
        return [];
    if (Array.isArray(list))
        return list;

    const items = [];
    try {
        for (const item of list)
            items.push(item);
        if (items.length > 0)
            return items;
    } catch (_e) {
    }

    for (let node = list; node; node = node.next ?? null) {
        const data = node.data ?? null;
        if (data)
            items.push(data);
    }
    return items;
};

const shellMonitorManager = () => {
    try {
        const backend = global.backend;
        if (backend && typeof backend.get_monitor_manager === 'function')
            return backend.get_monitor_manager();
    } catch (_e) {
    }

    try {
        if (typeof Meta.MonitorManager?.get === 'function')
            return Meta.MonitorManager.get();
    } catch (_e) {
    }

    return null;
};

const shellMonitorGeometry = index => {
    const layout = Main.layoutManager?.monitors?.[index] ?? null;
    if (layout)
        return layout;

    try {
        return global.display?.get_monitor_geometry?.(index) ?? null;
    } catch (_e) {
        return null;
    }
};

const sameMonitorGeometry = (left, right) =>
    left &&
    right &&
    Math.round(Number(left.x) || 0) === Math.round(Number(right.x) || 0) &&
    Math.round(Number(left.y) || 0) === Math.round(Number(right.y) || 0) &&
    Math.round(Number(left.width) || 0) === Math.round(Number(right.width) || 0) &&
    Math.round(Number(left.height) || 0) === Math.round(Number(right.height) || 0);

const connectorFromMetaMonitorManager = monitorIndex => {
    const manager = shellMonitorManager();
    if (!manager)
        return '';

    const logicalMonitors = manager.get_logical_monitors?.();
    for (const logicalMonitor of gListToArray(logicalMonitors)) {
        if (logicalMonitor.get_number?.() !== monitorIndex)
            continue;

        for (const monitor of gListToArray(logicalMonitor.get_monitors?.())) {
            const connector = normalizeConnector(monitor?.get_connector?.());
            if (connector)
                return connector;
        }
    }

    return '';
};

const connectorFromGdkDisplay = monitorIndex => {
    const display = Gdk.Display.get_default?.() ?? null;
    const monitors = display?.get_monitors?.();
    const itemCount = monitors?.get_n_items?.() ?? 0;
    if (!monitors || itemCount <= 0)
        return '';

    const targetGeometry = shellMonitorGeometry(monitorIndex);
    if (targetGeometry) {
        const target = {
            x: Math.round(Number(targetGeometry.x) || 0),
            y: Math.round(Number(targetGeometry.y) || 0),
            width: Math.round(Number(targetGeometry.width) || 0),
            height: Math.round(Number(targetGeometry.height) || 0),
        };

        for (let i = 0; i < itemCount; i++) {
            const monitor = monitors.get_item(i);
            if (!monitor)
                continue;

            const geometry = monitor.get_geometry?.();
            if (!sameMonitorGeometry(target, geometry))
                continue;

            const connector = normalizeConnector(monitor.get_connector?.());
            if (connector)
                return connector;
        }
    }

    if (monitorIndex >= 0 && monitorIndex < itemCount) {
        const monitor = monitors.get_item(monitorIndex);
        const connector = normalizeConnector(monitor?.get_connector?.());
        if (connector)
            return connector;
    }

    return '';
};

export const shellMonitorLayout = monitorIndex => {
    const index = Number(monitorIndex);
    if (!Number.isInteger(index) || index < 0)
        return null;
    return Main.layoutManager?.monitors?.[index] ?? null;
};

export const shellMonitorConnector = (index, layout = null) => {
    const monitorIndex = Number(index);
    if (!Number.isInteger(monitorIndex) || monitorIndex < 0)
        return '';

    const fromMeta = connectorFromMetaMonitorManager(monitorIndex);
    if (fromMeta)
        return fromMeta;

    const fromGdk = connectorFromGdkDisplay(monitorIndex);
    if (fromGdk)
        return fromGdk;

    if (!layout)
        layout = shellMonitorLayout(monitorIndex);

    return monitorObjectString(layout, ['connector', 'output']).trim();
};
