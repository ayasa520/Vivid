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

import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk';
import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

const webuiUrl = 'http://127.0.0.1:8765/';

export default class VividWallpaperPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const page = new Adw.PreferencesPage();
        const group = new Adw.PreferencesGroup({
            title: 'WebUI',
        });
        const row = new Adw.ActionRow({
            title: 'WebUI',
            subtitle: webuiUrl,
        });
        const button = new Gtk.Button({
            label: 'Open WebUI',
            valign: Gtk.Align.CENTER,
        });

        button.add_css_class('suggested-action');
        button.connect('clicked', () => {
            Gio.AppInfo.launch_default_for_uri(webuiUrl, null);
        });

        row.add_suffix(button);
        row.activatable_widget = button;
        group.add(row);
        page.add(group);
        window.add(page);
    }
}
