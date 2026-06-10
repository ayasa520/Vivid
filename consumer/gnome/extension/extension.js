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

import GLib from 'gi://GLib';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import * as AutoPause from './shell/integration/autoPause.js';
import * as GnomeShellOverride from './shell/integration/gnomeShellOverride.js';
import * as WindowManager from './shell/integration/windowManager.js';
import * as DisplayHelper from './shell/services/displayHelper.js';

export default class VividWallpaperExtension extends Extension {
    constructor(metadata) {
        super(metadata);
        this.isEnabled = false;
        this.startupDelayId = 0;
        this.startupCompleteId = 0;
        this.startupOverviewRestoreId = 0;
    }

    enable() {
        this.settings = this.getSettings();
        const socketPath = this.settings.get_string('display-socket-path');
        this.displayHelper = new DisplayHelper.DisplayHelperService({
            socketPath: socketPath && socketPath.length > 0 ? socketPath : null,
        });
        this.windowManager = new WindowManager.WindowManager();

        /**
         * Disable startup animation (Workaround for issue #65)
         */
        this.old_hasOverview = Main.sessionMode.hasOverview;

        if (Main.layoutManager._startingUp) {
            Main.sessionMode.hasOverview = false;
            this.startupOverviewRestoreId = Main.layoutManager.connect('startup-complete', () => {
                Main.sessionMode.hasOverview = this.old_hasOverview;
                Main.layoutManager.disconnect(this.startupOverviewRestoreId);
                this.startupOverviewRestoreId = 0;
            });
            // Handle Ubuntu's method
            if (Main.layoutManager.startInOverview)
                Main.layoutManager.startInOverview = false;
        }

        /**
         * Other overrides
         */
        this.override = new GnomeShellOverride.GnomeShellOverride(this);

        // If the desktop is still starting up, wait until it is ready
        if (Main.layoutManager._startingUp) {
            this.startupCompleteId = Main.layoutManager.connect(
                'startup-complete',
                () => {
                    this.startupDelayId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, this.settings.get_int('startup-delay'), () => {
                        this.startupDelayId = 0;
                        Main.layoutManager.disconnect(this.startupCompleteId);
                        this.startupCompleteId = 0;
                        if (this.settings)
                            this.innerEnable();
                        return false;
                    });
                }
            );
        } else {
            this.startupDelayId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, this.settings.get_int('startup-delay'), () => {
                this.startupDelayId = 0;
                if (this.settings)
                    this.innerEnable();
                return false;
            });
        }
    }

    innerEnable() {
        this.displayHelper?.start();
        this.windowManager?.enable();
        /*
         * The GNOME extension is now a desktop fact producer, not a playback
         * policy owner. These observations are sent over display-v1 so the
         * producer can evaluate pause/stop policy from its own persisted
         * configuration.
         */
        this.autoPauseFacts = new AutoPause.AutoPause(this.displayHelper);
        this.autoPauseFacts.enable();
        this.override.enable();

        this.isEnabled = true;
    }

    disable() {
        if (this.startupDelayId) {
            GLib.source_remove(this.startupDelayId);
            this.startupDelayId = 0;
        }

        if (this.startupCompleteId) {
            Main.layoutManager.disconnect(this.startupCompleteId);
            this.startupCompleteId = 0;
        }

        if (this.startupOverviewRestoreId) {
            Main.layoutManager.disconnect(this.startupOverviewRestoreId);
            this.startupOverviewRestoreId = 0;
        }

        Main.sessionMode.hasOverview = this.old_hasOverview;
        this.autoPauseFacts?.disable();
        this.displayHelper?.stop();
        this.override?.disable();
        this.windowManager?.disable();

        this.isEnabled = false;

        this.settings = null;
        this.autoPauseFacts = null;
        this.override = null;
        this.windowManager = null;
        this.displayHelper = null;
    }
}
