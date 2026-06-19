const dom = {
  statusText: document.querySelector('#statusText'),
  refreshButton: document.querySelector('#refreshButton'),
  themeButton: document.querySelector('#themeButton'),
  languageSelect: document.querySelector('#languageSelect'),
  resetDefaultsButton: document.querySelector('#resetDefaultsButton'),
  views: [...document.querySelectorAll('.view')],
  settingsButton: document.querySelector('#settingsButton'),
  settingsBackButton: document.querySelector('#settingsBackButton'),
  browseLayout: document.querySelector('.browse-layout'),
  inspectorBackButton: document.querySelector('#inspectorBackButton'),
  projectSearch: document.querySelector('#projectSearch'),
  projectSort: document.querySelector('#projectSort'),
  typeFilters: [...document.querySelectorAll('.typeFilter')],
  dynamicFilters: document.querySelector('#dynamicFilters'),
  filterDropdownButton: document.querySelector('#filterDropdownButton'),
  filterDropdown: document.querySelector('#filterDropdown'),
  projectCount: document.querySelector('#projectCount'),
  projectColumn: document.querySelector('.project-column'),
  projectGrid: document.querySelector('#projectGrid'),
  projectEmpty: document.querySelector('#projectEmpty'),
  inspectorTitle: document.querySelector('#inspectorTitle'),
  inspector: document.querySelector('.inspector'),
  inspectorSubtitle: document.querySelector('#inspectorSubtitle'),
  inspectorType: document.querySelector('#inspectorType'),
  inspectorPreview: document.querySelector('#inspectorPreview'),
  inspectorProperties: document.querySelector('#inspectorProperties'),
  settingsForm: document.querySelector('#settingsForm'),
  stateDetails: document.querySelector('.state-details'),
  stateOutput: document.querySelector('#stateOutput'),
};

const app = {
  state: {},
  global: {},
  projects: [],
  projectByPath: new Map(),
  browserFilterState: null,
  selectedProject: null,
  selectedOverrides: {},
  propertyRows: new Map(),
  propertySections: [],
  settingControls: new Map(),
  configTimers: new Map(),
  propertyTimer: 0,
  projectRenderToken: 0,
  projectRenderHandle: null,
  stateOutputDirty: true,
  stateOutputRenderHandle: null,
  populatingSettings: false,
  inspectorOpen: false,
  locale: 'en',
  themeMode: 'system',
  statusLocalization: null,
};

const wideBrowseQuery = window.matchMedia('(min-width: 1081px)');
const systemThemeQuery = window.matchMedia('(prefers-color-scheme: dark)');

const localeStorageKey = 'vivid-webui-locale';
const themeStorageKey = 'vivid-webui-theme-mode';
const projectRenderBatchSize = 48;
const projectRenderIdleTimeoutMs = 80;
const slowWorkLogThresholdMs = 50;

/*
 * Keep WebUI chrome translations centralized so language support stays an
 * additive concern. Wallpaper project metadata comes from user/workshop files
 * and is intentionally rendered as authored instead of being translated here.
 */
const TRANSLATIONS = {
  en: {
    'action.backToBrowse': 'Back to browse',
    'action.hideProperties': 'Hide properties',
    'action.openWallpaperSettings': 'Configure wallpaper',
    'action.refresh': 'Refresh',
    'action.restoreDefaults': 'Restore Defaults',
    'action.showProperties': 'Show properties',
    'aria.browseWallpapers': 'Browse wallpapers',
    'aria.mainViews': 'Main views',
    'aria.metadataFilters': 'Wallpaper metadata filters',
    'aria.producerSettings': 'Producer settings',
    'aria.wallpaperFilters': 'Wallpaper filters',
    'aria.wallpaperGrid': 'Wallpaper grid',
    'aria.wallpaperProperties': 'Wallpaper properties',
    'count.wallpapers': '{count} wallpaper{plural}',
    'confirm.restoreDefaults': 'Restore all global settings and wallpaper-specific settings to defaults?',
    'empty.noConfigurableProperties': 'This wallpaper has no configurable properties.',
    'empty.noMatches': 'No wallpapers match the current filters.',
    'empty.setLibrary': 'Set Steam Library in Settings to browse wallpapers.',
    'filter.contentRating': 'Content Rating',
    'filter.tags': 'Tags',
    'filter.type': 'Type',
    'inspector.noneSelected': 'No wallpaper selected',
    'inspector.wallpaper': 'Wallpaper',
    'label.filter': 'Filter',
    'label.language': 'Language',
    'label.search': 'Search',
    'label.sort': 'Sort',
    'label.theme': 'Theme',
    'option.auto': 'Auto',
    'option.unavailable': '{value} (unavailable)',
    'placeholder.search': 'Search wallpapers, tags, or project names',
    'project.subtitle': '{type} - {detail}',
    'project.untitled': 'Untitled',
    'rating.Everyone': 'Everyone',
    'rating.Mature': 'Mature',
    'rating.Questionable': 'Questionable',
    'reset.defaultsDescription': 'Restore all global settings and wallpaper-specific settings to their defaults.',
    'section.autopause': 'Auto Pause',
    'section.developer': 'Developer',
    'section.experimental': 'Experimental',
    'section.general': 'General Settings',
    'section.producerState': 'Producer State',
    'section.reset': 'Reset',
    'section.scene': 'Scene Settings',
    'section.web': 'Web Settings',
    'setting.change-wallpaper-directory-path.name': 'Steam Library',
    'setting.change-wallpaper-directory-path.note': 'Wallpaper Engine library root',
    'setting.change-wallpaper-interval.name': 'Change Wallpaper Interval',
    'setting.change-wallpaper-interval.note': 'Minutes',
    'setting.change-wallpaper-mode.name': 'Change Wallpaper Mode',
    'setting.change-wallpaper.name': 'Change Wallpaper Automatically',
    'setting.content-fit.name': 'Fit Mode',
    'setting.content-fit.note': 'Wallpaper scaling on the monitor',
    'setting.debug-mode.name': 'Debug Mode',
    'setting.low-battery-threshold.name': 'Low Battery Threshold',
    'setting.mute.name': 'Mute Audio',
    'setting.pause-on-battery.name': 'Pause on Battery',
    'setting.pause-on-focus.name': 'Pause when Desktop Loses Focus',
    'setting.pause-on-maximize-or-fullscreen.name': 'Pause on Maximize or Fullscreen',
    'setting.pause-on-mpris-playing.name': 'Pause on Media Player Playing',
    'setting.render-device.name': 'Rendering GPU',
    'setting.scene-fps.name': 'Frame Rate',
    'setting.scene-fps.note': 'Applies to scene, web, and video wallpapers',
    'setting.show-panel-menu.name': 'Show Panel Menu',
    'setting.startup-delay.name': 'Startup Delay',
    'setting.startup-delay.note': 'Milliseconds',
    'setting.stop-on-applications.name': 'Stop on Applications',
    'setting.stop-on-applications.note': 'Desktop IDs, WM_CLASS values, or executable names',
    'setting.volume.name': 'Volume Level',
    'settingOption.change-wallpaper-mode.0': 'Sequential',
    'settingOption.change-wallpaper-mode.1': 'Inverse Sequential',
    'settingOption.change-wallpaper-mode.2': 'Random',
    'settingOption.content-fit.0': 'Fill',
    'settingOption.content-fit.1': 'Contain',
    'settingOption.content-fit.2': 'Cover',
    'settingOption.content-fit.3': 'Scale down',
    'settingOption.pause-on-battery.0': 'Never',
    'settingOption.pause-on-battery.1': 'Low Battery',
    'settingOption.pause-on-battery.2': 'Always',
    'settingOption.pause-on-maximize-or-fullscreen.0': 'Never',
    'settingOption.pause-on-maximize-or-fullscreen.1': 'Any Monitor',
    'settingOption.pause-on-maximize-or-fullscreen.2': 'All Monitors',
    'sort.name': 'Name',
    'sort.type': 'Type',
    'sort.updated': 'Updated',
    'status.applyingProperties': 'Applying wallpaper properties...',
    'status.connected': 'Connected',
    'status.connecting': 'Connecting to producer...',
    'status.disconnected': 'Disconnected',
    'status.loadedWallpapers': 'Loaded {count} wallpapers',
    'status.loadingWallpapers': 'Loading wallpapers...',
    'status.propertiesApplied': 'Wallpaper properties applied',
    'status.defaultsRestored': 'Defaults restored',
    'status.restoringDefaults': 'Restoring defaults...',
    'status.saved': 'Saved',
    'status.saving': 'Saving...',
    'status.selectingWallpaper': 'Selecting wallpaper...',
    'status.wallpaperSelected': 'Wallpaper selected',
    'tab.browse': 'Browse',
    'tab.settings': 'Settings',
    'tag.unspecified': 'Unspecified',
    'theme.current': 'Theme: {theme}. Click to change.',
    'theme.dark': 'Dark',
    'theme.light': 'Light',
    'theme.system': 'System',
    'type.none': 'None',
    'type.scene': 'Scene',
    'type.unknown': 'unknown',
    'type.video': 'Video',
    'type.web': 'Web',
    'property.unsupported': 'Unsupported',
  },
  'zh-CN': {
    'action.backToBrowse': '返回浏览',
    'action.hideProperties': '隐藏属性',
    'action.openWallpaperSettings': '配置壁纸',
    'action.refresh': '刷新',
    'action.restoreDefaults': '恢复默认设置',
    'action.showProperties': '显示属性',
    'aria.browseWallpapers': '浏览壁纸',
    'aria.mainViews': '主视图',
    'aria.metadataFilters': '壁纸元数据筛选',
    'aria.producerSettings': 'Producer 设置',
    'aria.wallpaperFilters': '壁纸筛选',
    'aria.wallpaperGrid': '壁纸网格',
    'aria.wallpaperProperties': '壁纸属性',
    'count.wallpapers': '{count} 个壁纸',
    'confirm.restoreDefaults': '确定要恢复所有全局设置和壁纸特有设置为默认值吗？',
    'empty.noConfigurableProperties': '这个壁纸没有可配置属性。',
    'empty.noMatches': '没有壁纸匹配当前筛选条件。',
    'empty.setLibrary': '请在设置中填写 Steam 库路径以浏览壁纸。',
    'filter.contentRating': '内容分级',
    'filter.tags': '标签',
    'filter.type': '类型',
    'inspector.noneSelected': '未选择壁纸',
    'inspector.wallpaper': '壁纸',
    'label.filter': '筛选',
    'label.language': '语言',
    'label.search': '搜索',
    'label.sort': '排序',
    'label.theme': '主题',
    'option.auto': '自动',
    'option.unavailable': '{value}（不可用）',
    'placeholder.search': '搜索壁纸、标签或项目名称',
    'project.subtitle': '{type} - {detail}',
    'project.untitled': '未命名',
    'rating.Everyone': '全年龄',
    'rating.Mature': '成人',
    'rating.Questionable': '敏感',
    'reset.defaultsDescription': '恢复所有全局设置和壁纸特有设置为默认值。',
    'section.autopause': '自动暂停',
    'section.developer': '开发者',
    'section.experimental': '实验性',
    'section.general': '通用设置',
    'section.producerState': 'Producer 状态',
    'section.reset': '恢复默认',
    'section.scene': '场景设置',
    'section.web': '网页设置',
    'setting.change-wallpaper-directory-path.name': 'Steam 库',
    'setting.change-wallpaper-directory-path.note': 'Wallpaper Engine 库根目录',
    'setting.change-wallpaper-interval.name': '自动更换间隔',
    'setting.change-wallpaper-interval.note': '分钟',
    'setting.change-wallpaper-mode.name': '自动更换模式',
    'setting.change-wallpaper.name': '自动更换壁纸',
    'setting.content-fit.name': '适配模式',
    'setting.content-fit.note': '壁纸在显示器上的缩放方式',
    'setting.debug-mode.name': '调试模式',
    'setting.low-battery-threshold.name': '低电量阈值',
    'setting.mute.name': '静音',
    'setting.pause-on-battery.name': '电池供电时暂停',
    'setting.pause-on-focus.name': '桌面失焦时暂停',
    'setting.pause-on-maximize-or-fullscreen.name': '最大化或全屏时暂停',
    'setting.pause-on-mpris-playing.name': '媒体播放器播放时暂停',
    'setting.render-device.name': '渲染 GPU',
    'setting.scene-fps.name': '帧率',
    'setting.scene-fps.note': '作用于场景、网页和视频壁纸',
    'setting.show-panel-menu.name': '显示面板菜单',
    'setting.startup-delay.name': '启动延迟',
    'setting.startup-delay.note': '毫秒',
    'setting.stop-on-applications.name': '遇到应用时停止',
    'setting.stop-on-applications.note': '桌面 ID、WM_CLASS 或可执行文件名',
    'setting.volume.name': '音量',
    'settingOption.change-wallpaper-mode.0': '顺序',
    'settingOption.change-wallpaper-mode.1': '反向顺序',
    'settingOption.change-wallpaper-mode.2': '随机',
    'settingOption.content-fit.0': '填充',
    'settingOption.content-fit.1': '包含',
    'settingOption.content-fit.2': '覆盖',
    'settingOption.content-fit.3': '缩小适配',
    'settingOption.pause-on-battery.0': '从不',
    'settingOption.pause-on-battery.1': '低电量时',
    'settingOption.pause-on-battery.2': '总是',
    'settingOption.pause-on-maximize-or-fullscreen.0': '从不',
    'settingOption.pause-on-maximize-or-fullscreen.1': '任意显示器',
    'settingOption.pause-on-maximize-or-fullscreen.2': '全部显示器',
    'sort.name': '名称',
    'sort.type': '类型',
    'sort.updated': '更新时间',
    'status.applyingProperties': '正在应用壁纸属性...',
    'status.connected': '已连接',
    'status.connecting': '正在连接 producer...',
    'status.disconnected': '未连接',
    'status.loadedWallpapers': '已加载 {count} 个壁纸',
    'status.loadingWallpapers': '正在加载壁纸...',
    'status.propertiesApplied': '壁纸属性已应用',
    'status.defaultsRestored': '已恢复默认设置',
    'status.restoringDefaults': '正在恢复默认设置...',
    'status.saved': '已保存',
    'status.saving': '正在保存...',
    'status.selectingWallpaper': '正在选择壁纸...',
    'status.wallpaperSelected': '壁纸已选择',
    'tab.browse': '浏览',
    'tab.settings': '设置',
    'tag.unspecified': '未指定',
    'theme.current': '当前主题：{theme}。点击切换。',
    'theme.dark': '夜间',
    'theme.light': '日间',
    'theme.system': '跟随系统',
    'type.none': '无',
    'type.scene': '场景',
    'type.unknown': '未知',
    'type.video': '视频',
    'type.web': '网页',
    'property.unsupported': '不支持',
  },
};

function template(text, replacements = {}) {
  return `${text}`.replace(/\{(\w+)\}/g, (match, key) =>
    Object.prototype.hasOwnProperty.call(replacements, key) ? `${replacements[key]}` : match);
}

function t(key, replacements = {}) {
  const catalog = TRANSLATIONS[app.locale] ?? TRANSLATIONS.en;
  return template(catalog[key] ?? TRANSLATIONS.en[key] ?? key, replacements);
}

function tFallback(key, fallback, replacements = {}) {
  const catalog = TRANSLATIONS[app.locale] ?? TRANSLATIONS.en;
  const text = catalog[key] ?? TRANSLATIONS.en[key];
  return template(text ?? fallback, replacements);
}

function scheduleIdleWork(callback, timeout = projectRenderIdleTimeoutMs) {
  if ('requestIdleCallback' in window) {
    return {
      type: 'idle',
      id: window.requestIdleCallback(callback, {timeout}),
    };
  }

  return {
    type: 'timeout',
    id: window.setTimeout(() => callback({
      didTimeout: true,
      timeRemaining: () => 0,
    }), 16),
  };
}

function cancelIdleWork(handle) {
  if (!handle)
    return;
  if (handle.type === 'idle' && 'cancelIdleCallback' in window)
    window.cancelIdleCallback(handle.id);
  else
    window.clearTimeout(handle.id);
}

function logSlowWork(label, startedAt, details = {}) {
  const elapsedMs = performance.now() - startedAt;
  if (elapsedMs < slowWorkLogThresholdMs)
    return;
  console.info(`Vivid WebUI: ${label} took ${Math.round(elapsedMs)}ms`, details);
}

function normalizeLocale(value) {
  const normalized = `${value ?? ''}`.replace('_', '-').toLowerCase();
  if (normalized === 'zh' || normalized.startsWith('zh-'))
    return 'zh-CN';
  return 'en';
}

function detectInitialLocale() {
  const stored = window.localStorage.getItem(localeStorageKey);
  if (stored)
    return normalizeLocale(stored);
  for (const language of navigator.languages ?? [navigator.language]) {
    if (language)
      return normalizeLocale(language);
  }
  return 'en';
}

function normalizeThemeMode(value) {
  return ['system', 'light', 'dark'].includes(value) ? value : 'system';
}

function effectiveTheme(mode = app.themeMode) {
  const normalized = normalizeThemeMode(mode);
  if (normalized === 'dark')
    return 'dark';
  if (normalized === 'light')
    return 'light';
  return systemThemeQuery.matches ? 'dark' : 'light';
}

function detectInitialThemeMode() {
  return normalizeThemeMode(window.localStorage.getItem(themeStorageKey));
}

const themeIcons = {
  system: `
    <svg class="icon" viewBox="0 0 24 24" aria-hidden="true">
      <path d="M12 2v2"></path>
      <path d="M14.837 16.385a6 6 0 1 1-7.223-7.222c.624-.147.97.66.715 1.248a4 4 0 0 0 5.26 5.259c.589-.255 1.396.09 1.248.715"></path>
      <path d="M16 12a4 4 0 0 0-4-4"></path>
      <path d="m19 5-1.256 1.256"></path>
      <path d="M20 12h2"></path>
    </svg>
  `,
  light: `
    <svg class="icon" viewBox="0 0 24 24" aria-hidden="true">
      <circle cx="12" cy="12" r="4"></circle>
      <path d="M12 2v2"></path>
      <path d="M12 20v2"></path>
      <path d="m4.93 4.93 1.41 1.41"></path>
      <path d="m17.66 17.66 1.41 1.41"></path>
      <path d="M2 12h2"></path>
      <path d="M20 12h2"></path>
      <path d="m6.34 17.66-1.41 1.41"></path>
      <path d="m19.07 4.93-1.41 1.41"></path>
    </svg>
  `,
  dark: `
    <svg class="icon" viewBox="0 0 24 24" aria-hidden="true">
      <path d="M20.99 13.1A8.5 8.5 0 1 1 10.9 3.01 6.5 6.5 0 0 0 20.99 13.1Z"></path>
    </svg>
  `,
};

function renderThemeButton() {
  if (!dom.themeButton)
    return;
  const mode = normalizeThemeMode(app.themeMode);
  dom.themeButton.innerHTML = themeIcons[mode];
  const title = t('theme.current', {theme: t(`theme.${mode}`)});
  dom.themeButton.title = title;
  dom.themeButton.setAttribute('aria-label', title);
  dom.themeButton.dataset.themeMode = mode;
}

function applyThemeMode(mode, {persist = true} = {}) {
  app.themeMode = normalizeThemeMode(mode);
  const theme = effectiveTheme(app.themeMode);
  document.documentElement.dataset.themeMode = app.themeMode;
  document.documentElement.dataset.theme = theme;
  if (persist)
    window.localStorage.setItem(themeStorageKey, app.themeMode);
  renderThemeButton();
}

function cycleThemeMode() {
  const modes = ['system', 'light', 'dark'];
  const currentIndex = modes.indexOf(normalizeThemeMode(app.themeMode));
  applyThemeMode(modes[(currentIndex + 1) % modes.length]);
}

function localizeStaticDocument() {
  document.documentElement.lang = app.locale;
  document.title = 'Vivid Wallpaper';
  document.querySelectorAll('[data-i18n]').forEach(element => {
    if (element === dom.statusText)
      return;
    element.textContent = t(element.dataset.i18n);
  });
  document.querySelectorAll('[data-i18n-title]').forEach(element => {
    element.title = t(element.dataset.i18nTitle);
  });
  document.querySelectorAll('[data-i18n-aria-label]').forEach(element => {
    element.setAttribute('aria-label', t(element.dataset.i18nAriaLabel));
  });
  document.querySelectorAll('[data-i18n-placeholder]').forEach(element => {
    element.setAttribute('placeholder', t(element.dataset.i18nPlaceholder));
  });
}

function setStatus(text, state = 'neutral') {
  app.statusLocalization = null;
  dom.statusText.title = text;
  dom.statusText.dataset.state = state;
}

function setLocalizedStatus(key, state = 'neutral', replacements = {}) {
  app.statusLocalization = {key, state, replacements};
  const text = t(key, replacements);
  dom.statusText.title = text;
  dom.statusText.dataset.state = state;
}

const SETTINGS = [
  {
    section: 'general',
    key: 'change-wallpaper-directory-path',
    name: 'Steam Library',
    note: 'Wallpaper Engine library root',
    type: 'text',
    debounce: 650,
    afterSave: refreshProjects,
  },
  {
    section: 'general',
    key: 'content-fit',
    name: 'Fit Mode',
    note: 'Wallpaper scaling on the monitor',
    type: 'select',
    valueType: 'integer',
    options: [
      ['0', 'Fill'],
      ['1', 'Contain'],
      ['2', 'Cover'],
      ['3', 'Scale down'],
    ],
  },
  {section: 'general', key: 'mute', name: 'Mute Audio', type: 'boolean'},
  {section: 'general', key: 'volume', name: 'Volume Level', type: 'range', min: 0, max: 100, step: 1},
  {section: 'general', key: 'show-panel-menu', name: 'Show Panel Menu', type: 'boolean'},
  {section: 'general', key: 'change-wallpaper', name: 'Change Wallpaper Automatically', type: 'boolean'},
  {
    section: 'general',
    key: 'change-wallpaper-mode',
    name: 'Change Wallpaper Mode',
    type: 'select',
    valueType: 'integer',
    options: [
      ['0', 'Sequential'],
      ['1', 'Inverse Sequential'],
      ['2', 'Random'],
    ],
  },
  {
    section: 'general',
    key: 'change-wallpaper-interval',
    name: 'Change Wallpaper Interval',
    note: 'Minutes',
    type: 'number',
    min: 1,
    max: 1440,
    step: 1,
    debounce: 350,
  },
  {
    /*
     * The persisted key remains scene-fps for compatibility with existing
     * producer configs and socket payloads. It is shown under General because
     * the producer applies this rate to scene, web, and video render paths.
     */
    section: 'general',
    key: 'scene-fps',
    name: 'Frame Rate',
    note: 'Applies to scene, web, and video wallpapers',
    type: 'number',
    min: 5,
    max: 240,
    step: 1,
    debounce: 250,
  },
  {
    section: 'autopause',
    key: 'pause-on-maximize-or-fullscreen',
    name: 'Pause on Maximize or Fullscreen',
    type: 'select',
    valueType: 'integer',
    options: [
      ['0', 'Never'],
      ['1', 'Any Monitor'],
      ['2', 'All Monitors'],
    ],
  },
  {section: 'autopause', key: 'pause-on-focus', name: 'Pause when Desktop Loses Focus', type: 'boolean'},
  {
    section: 'autopause',
    key: 'pause-on-battery',
    name: 'Pause on Battery',
    type: 'select',
    valueType: 'integer',
    options: [
      ['0', 'Never'],
      ['1', 'Low Battery'],
      ['2', 'Always'],
    ],
  },
  {
    section: 'autopause',
    key: 'low-battery-threshold',
    name: 'Low Battery Threshold',
    type: 'number',
    min: 0,
    max: 100,
    step: 1,
    debounce: 350,
  },
  {section: 'autopause', key: 'pause-on-mpris-playing', name: 'Pause on Media Player Playing', type: 'boolean'},
  {
    section: 'autopause',
    key: 'stop-on-applications',
    name: 'Stop on Applications',
    note: 'Desktop IDs, WM_CLASS values, or executable names',
    type: 'list',
    debounce: 650,
  },
  {
    section: 'general',
    key: 'render-device',
    name: 'Rendering GPU',
    type: 'select',
    dynamicOptions: gpuDeviceOptions,
    afterSave: () => refreshState(),
  },
  {section: 'developer', key: 'debug-mode', name: 'Debug Mode', type: 'boolean'},
  {
    section: 'developer',
    key: 'startup-delay',
    name: 'Startup Delay',
    note: 'Milliseconds',
    type: 'number',
    min: 0,
    max: 10000,
    step: 100,
    debounce: 350,
  },
];

const browserSortKeys = new Set(['name', 'updated-time', 'type']);
const contentRatings = ['Everyone', 'Questionable', 'Mature'];

const editablePropertyTypes = new Set([
  'bool',
  'color',
  'combo',
  'directory',
  'file',
  'scenetexture',
  'slider',
  'textinput',
]);

const stringPropertyTypes = new Set([
  'color',
  'combo',
  'directory',
  'file',
  'group',
  'scenetexture',
  'text',
  'textinput',
]);

const allowedMarkupTags = new Set([
  'a',
  'b',
  'big',
  'blockquote',
  'br',
  'center',
  'code',
  'div',
  'em',
  'font',
  'hr',
  'i',
  'img',
  'li',
  'ol',
  'p',
  'small',
  'span',
  'strong',
  'sub',
  'sup',
  'u',
  'ul',
]);

const dropMarkupTags = new Set([
  'script',
  'style',
  'iframe',
  'object',
  'embed',
  'canvas',
  'svg',
  'math',
  'link',
  'meta',
]);

const allowedMarkupAttributes = {
  a: new Set(['href', 'title']),
  font: new Set(['color', 'face', 'size']),
  img: new Set(['alt', 'src', 'title']),
};

async function requestJson(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: {
      'Content-Type': 'application/json',
      ...(options.headers ?? {}),
    },
  });
  const payload = await response.json();
  if (!response.ok || !payload.ok)
    throw new Error(payload.error ?? `HTTP ${response.status}`);
  return payload;
}

function controlPayload(response) {
  return response?.control?.payload ?? response?.state ?? {};
}

function gpuDeviceNode(device) {
  return device?.['render-node'] ?? device?.renderNode ?? device?.render_node ?? '';
}

function gpuDeviceOptions() {
  const options = [['auto', t('option.auto')]];
  for (const device of app.state?.['gpu-devices'] ?? []) {
    const node = gpuDeviceNode(device);
    if (!node)
      continue;
    const name = device?.name ? `${device.name}` : node;
    options.push([node, `${node} : ${name}`]);
  }
  return options;
}

function settingName(definition) {
  return tFallback(`setting.${definition.key}.name`, definition.name);
}

function settingNote(definition) {
  if (!definition.note)
    return '';
  return tFallback(`setting.${definition.key}.note`, definition.note);
}

function settingOptionText(definition, value, fallback) {
  return tFallback(`settingOption.${definition.key}.${value}`, fallback);
}

function settingOptions(definition, currentValue = '') {
  const options = definition.dynamicOptions
    ? definition.dynamicOptions()
    : (definition.options ?? []).map(([value, text]) => [
        value,
        settingOptionText(definition, value, text),
      ]);
  const normalizedValue = `${currentValue ?? ''}`;
  if (!normalizedValue || options.some(([value]) => value === normalizedValue))
    return options;
  return [...options, [normalizedValue, t('option.unavailable', {value: normalizedValue})]];
}

function rebuildSelectOptions(definition, control, currentValue = '') {
  control.replaceChildren();
  for (const [value, text] of settingOptions(definition, currentValue)) {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = text;
    control.append(option);
  }
}

function isStateOutputVisible() {
  return Boolean(dom.stateOutput && dom.stateDetails?.open);
}

function renderStateOutputNow() {
  if (!dom.stateOutput)
    return;
  const startedAt = performance.now();
  dom.stateOutput.textContent = JSON.stringify(app.state ?? {}, null, 2);
  app.stateOutputDirty = false;
  logSlowWork('state JSON render', startedAt);
}

function scheduleStateOutputRender() {
  if (!isStateOutputVisible() || app.stateOutputRenderHandle)
    return;
  app.stateOutputRenderHandle = scheduleIdleWork(() => {
    app.stateOutputRenderHandle = null;
    if (app.stateOutputDirty && isStateOutputVisible())
      renderStateOutputNow();
  }, 120);
}

function updateStateOutput() {
  /*
   * The producer state can grow with per-wallpaper property payloads. Keep the
   * developer JSON accurate without serializing and painting a large hidden
   * <pre> on every startup, project refresh, or property edit.
   */
  app.stateOutputDirty = true;
  scheduleStateOutputRender();
}

function cloneJson(value) {
  return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
}

function parseJsonObject(value) {
  if (!value)
    return {};
  if (typeof value === 'object' && !Array.isArray(value))
    return value;
  if (typeof value !== 'string')
    return {};
  try {
    const parsed = JSON.parse(value);
    return parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed : {};
  } catch (_error) {
    return {};
  }
}

function formatType(type) {
  const normalized = type ? `${type}` : '';
  return normalized
    ? tFallback(`type.${normalized}`, normalized)
    : t('type.unknown');
}

function formatContentRating(rating) {
  const normalized = rating ? `${rating}` : 'Everyone';
  return tFallback(`rating.${normalized}`, normalized);
}

function formatFilterValue(group, value) {
  if (group === 'contentrating')
    return formatContentRating(value);
  if (group === 'tags' && value === 'Unspecified')
    return t('tag.unspecified');
  return value;
}

function formatWallpaperCount(count) {
  return t('count.wallpapers', {
    count,
    plural: count === 1 ? '' : 's',
  });
}

function formatProjectDetail(project) {
  if (project.tags?.[0])
    return project.tags[0];
  if (project.contentrating)
    return formatContentRating(project.contentrating);
  return formatPathName(project.path);
}

function stripMarkup(text, fallback = '') {
  const source = typeof text === 'string' && text.trim() ? text : fallback;
  const template = document.createElement('template');
  template.innerHTML = source;
  return (template.content.textContent || source || fallback).trim();
}

function isSafeLinkUrl(value) {
  if (!value)
    return false;
  try {
    const url = new URL(value, window.location.href);
    return ['http:', 'https:', 'steam:'].includes(url.protocol);
  } catch (_error) {
    return false;
  }
}

function isRemoteImageUrl(value) {
  if (!value)
    return false;
  try {
    const url = new URL(value, window.location.href);
    return ['http:', 'https:'].includes(url.protocol);
  } catch (_error) {
    return false;
  }
}

function remoteImageProxyUrl(value) {
  return `/api/remote-image?url=${encodeURIComponent(value)}`;
}

function resolveProjectImageSource(source, project) {
  const raw = `${source ?? ''}`.trim();
  if (!raw)
    return '';
  if (isRemoteImageUrl(raw))
    return remoteImageProxyUrl(raw);

  if (raw.startsWith('file://')) {
    try {
      const url = new URL(raw);
      return `/api/thumbnail?path=${encodeURIComponent(decodeURIComponent(url.pathname))}`;
    } catch (_error) {
      return '';
    }
  }

  if (raw.startsWith('/'))
    return `/api/thumbnail?path=${encodeURIComponent(raw)}`;

  if (!project?.path)
    return '';
  return `/api/thumbnail?path=${encodeURIComponent(`${project.path}/${raw.replace(/^\.?\//, '')}`)}`;
}

function sanitizeMarkupNode(node, project) {
  if (node.nodeType === Node.TEXT_NODE)
    return;
  if (node.nodeType !== Node.ELEMENT_NODE) {
    node.remove();
    return;
  }

  const tag = node.tagName.toLowerCase();
  if (dropMarkupTags.has(tag)) {
    node.remove();
    return;
  }

  if (!allowedMarkupTags.has(tag)) {
    const children = [...node.childNodes];
    children.forEach(child => sanitizeMarkupNode(child, project));
    const fragment = document.createDocumentFragment();
    while (node.firstChild)
      fragment.append(node.firstChild);
    node.replaceWith(fragment);
    return;
  }

  const allowedAttributes = allowedMarkupAttributes[tag] ?? new Set();
  for (const attribute of [...node.attributes]) {
    const name = attribute.name.toLowerCase();
    if (name.startsWith('on') || !allowedAttributes.has(name)) {
      node.removeAttribute(attribute.name);
      continue;
    }

    if (tag === 'a' && name === 'href') {
      if (!isSafeLinkUrl(attribute.value)) {
        node.removeAttribute(attribute.name);
        continue;
      }
      node.setAttribute('target', '_blank');
      node.setAttribute('rel', 'noreferrer noopener');
    }

    if (tag === 'img' && name === 'src') {
      const resolved = resolveProjectImageSource(attribute.value, project);
      if (!resolved) {
        node.removeAttribute(attribute.name);
        continue;
      }
      node.setAttribute('src', resolved);
      node.setAttribute('loading', 'lazy');
      node.setAttribute('decoding', 'async');
    }
  }

  [...node.childNodes].forEach(child => sanitizeMarkupNode(child, project));
}

function createMarkupElement(text, fallback = '', className = '', project = app.selectedProject) {
  const source = typeof text === 'string' && text.trim() ? text : fallback;
  const element = document.createElement('div');
  if (className)
    element.className = className;

  /*
   * Wallpaper Engine project metadata commonly uses small HTML fragments for
   * labels, descriptions, and group headings. Render those fragments instead of
   * flattening them to text, but sanitize the fragment first so workshop data
   * cannot execute script in the local WebUI controller.
   *
   * Each <text> property carries a standalone HTML snippet.  Frequently the
   * author only wrote an opening <center> without a closing tag — the original
   * Wallpaper Engine concatenates all snippets into one document where a later
   * snippet supplies the missing </center>.  Since we render every snippet
   * independently, the browser parser can restructure invalid nesting (e.g.
   * <a><center> → auto-closes <a>).  Detect <center> in the raw source and
   * mirror its intent on the container so centering survives any parser
   * rearrangement.
   */
  const template = document.createElement('template');
  template.innerHTML = source || fallback || '';
  [...template.content.childNodes].forEach(child => sanitizeMarkupNode(child, project));
  element.append(template.content);
  if (!element.textContent.trim() && element.childElementCount === 0)
    element.textContent = fallback;

  if (/<center\b/i.test(source))
    element.classList.add('contains-center');

  return element;
}

function formatPathName(path) {
  if (!path)
    return '';
  return `${path}`.split('/').filter(Boolean).at(-1) ?? path;
}

function splitList(value) {
  if (Array.isArray(value))
    return [...new Set(value.map(item => `${item}`.trim()).filter(Boolean))];
  return [...new Set(`${value ?? ''}`.split(/[,\n;]/).map(item => item.trim()).filter(Boolean))];
}

function projectFilterTags(project) {
  return Array.isArray(project?.tags) && project.tags.length > 0
    ? project.tags.map(tag => `${tag}`.trim()).filter(Boolean)
    : ['Unspecified'];
}

function availableProjectTags(projects = []) {
  const tags = new Set(['Unspecified']);
  for (const project of projects) {
    for (const tag of projectFilterTags(project))
      tags.add(tag);
  }
  return [...tags].sort((left, right) => left.localeCompare(right));
}

function defaultBrowserFilterState(projects = []) {
  const state = {
    type: {
      scene: true,
      web: true,
      video: true,
    },
    contentrating: {
      Everyone: true,
      Questionable: true,
      Mature: false,
    },
    tags: {},
  };
  for (const tag of availableProjectTags(projects))
    state.tags[tag] = true;
  return state;
}

function normalizeBrowserFilterState(serialized, projects = []) {
  let parsed = {};
  if (serialized) {
    try {
      parsed = typeof serialized === 'string' ? JSON.parse(serialized) : serialized;
    } catch (_error) {
      parsed = {};
    }
  }

  const state = defaultBrowserFilterState(projects);
  for (const type of Object.keys(state.type)) {
    if (typeof parsed?.type?.[type] === 'boolean')
      state.type[type] = parsed.type[type];
  }
  for (const rating of contentRatings) {
    if (typeof parsed?.contentrating?.[rating] === 'boolean')
      state.contentrating[rating] = parsed.contentrating[rating];
  }
  for (const tag of Object.keys(parsed?.tags ?? {})) {
    const normalizedTag = `${tag}`.trim();
    if (normalizedTag)
      state.tags[normalizedTag] = typeof parsed.tags[tag] === 'boolean' ? parsed.tags[tag] : true;
  }
  return state;
}

function serializeBrowserFilterState(state) {
  const normalized = normalizeBrowserFilterState(state, app.projects);
  const payload = {
    type: {...normalized.type},
    contentrating: {...normalized.contentrating},
    tags: {},
  };
  for (const tag of Object.keys(normalized.tags).sort((left, right) => left.localeCompare(right)))
    payload.tags[tag] = normalized.tags[tag];
  return JSON.stringify(payload);
}

function browserFilterFromControls() {
  const state = normalizeBrowserFilterState(app.browserFilterState, app.projects);
  for (const control of dom.typeFilters)
    state.type[control.value] = control.checked;

  dom.dynamicFilters.querySelectorAll('input[data-filter-group]').forEach(control => {
    const group = control.dataset.filterGroup;
    const value = control.dataset.filterValue;
    if (!state[group])
      state[group] = {};
    state[group][value] = control.checked;
  });
  return state;
}

function persistBrowserFilterState() {
  app.browserFilterState = browserFilterFromControls();
  sendConfigPatch({
    'project-browser-filter-state': serializeBrowserFilterState(app.browserFilterState),
  });
}

function persistBrowserSortKey() {
  const key = browserSortKeys.has(dom.projectSort.value) ? dom.projectSort.value : 'name';
  sendConfigPatch({'project-browser-sort-key': key});
}

function settingValue(definition, control) {
  switch (definition.type) {
  case 'boolean':
    return control.checked;
  case 'select':
    if (definition.valueType === 'integer') {
      const parsed = Number.parseInt(control.value || '0', 10);
      return Number.isFinite(parsed) ? parsed : 0;
    }
    return control.value ?? '';
  case 'number':
  case 'range':
    return Number.parseInt(control.value || '0', 10);
  case 'list':
    return splitList(control.value);
  default:
    return control.value ?? '';
  }
}

function applySettingControlValue(definition, value) {
  const entry = app.settingControls.get(definition.key);
  if (!entry)
    return;
  const {control, output} = entry;
  switch (definition.type) {
  case 'boolean':
    control.checked = Boolean(value);
    break;
  case 'select':
    rebuildSelectOptions(definition, control, value);
    control.value = value !== undefined && value !== null ? `${value}` : '';
    break;
  case 'list':
    control.value = Array.isArray(value) ? value.join(', ') : `${value ?? ''}`;
    break;
  default:
    control.value = value !== undefined && value !== null ? `${value}` : '';
    break;
  }
  if (output)
    output.textContent = `${control.value || 0}`;
}

async function sendConfigPatch(patch, afterSave = null) {
  Object.assign(app.global, patch);
  updateStateOutput();
  setLocalizedStatus('status.saving', 'working');
  try {
    await requestJson('/api/config', {
      method: 'POST',
      body: JSON.stringify(patch),
    });
    setLocalizedStatus('status.saved', 'ok');
    afterSave?.();
  } catch (error) {
    setStatus(error.message, 'error');
  }
}

async function resetAllDefaults() {
  if (!window.confirm(t('confirm.restoreDefaults')))
    return;

  setLocalizedStatus('status.restoringDefaults', 'working');
  try {
    await requestJson('/api/config', {
      method: 'POST',
      body: JSON.stringify({'reset-defaults': true}),
    });
    app.selectedProject = null;
    app.selectedOverrides = {};
    app.browserFilterState = null;
    app.propertyRows.clear();
    app.propertySections = [];
    await refreshState({projects: true});
    setLocalizedStatus('status.defaultsRestored', 'ok');
  } catch (error) {
    setStatus(error.message, 'error');
  }
}

function scheduleConfigPatch(definition, value) {
  if (app.populatingSettings)
    return;

  const patch = {[definition.key]: value};
  const delay = definition.debounce ?? (definition.type === 'text' || definition.type === 'list' ? 500 : 0);
  const existingTimer = app.configTimers.get(definition.key);
  if (existingTimer)
    window.clearTimeout(existingTimer);

  const run = () => {
    app.configTimers.delete(definition.key);
    sendConfigPatch(patch, definition.afterSave);
  };

  if (delay > 0)
    app.configTimers.set(definition.key, window.setTimeout(run, delay));
  else
    run();
}

function createSettingRow(definition) {
  const row = document.createElement('label');
  row.className = 'setting-row';

  const label = document.createElement('span');
  label.className = 'setting-label';
  const name = document.createElement('span');
  name.className = 'setting-name';
  name.textContent = settingName(definition);
  label.append(name);
  let note = null;
  if (definition.note) {
    note = document.createElement('span');
    note.className = 'setting-note';
    note.textContent = settingNote(definition);
    label.append(note);
  }

  const controlWrap = document.createElement('span');
  controlWrap.className = 'setting-control';
  let control;
  let output = null;

  if (definition.type === 'select') {
    control = document.createElement('select');
    rebuildSelectOptions(definition, control, app.global[definition.key]);
    control.addEventListener('change', () => scheduleConfigPatch(definition, settingValue(definition, control)));
  } else if (definition.type === 'boolean') {
    control = document.createElement('input');
    control.type = 'checkbox';
    control.addEventListener('change', () => scheduleConfigPatch(definition, settingValue(definition, control)));
  } else if (definition.type === 'range') {
    control = document.createElement('input');
    control.type = 'range';
    control.min = `${definition.min}`;
    control.max = `${definition.max}`;
    control.step = `${definition.step ?? 1}`;
    output = document.createElement('span');
    output.className = 'value-output';
    control.addEventListener('input', () => {
      output.textContent = control.value;
      scheduleConfigPatch(definition, settingValue(definition, control));
    });
  } else if (definition.type === 'list') {
    control = document.createElement('textarea');
    control.spellcheck = false;
    control.addEventListener('input', () => scheduleConfigPatch(definition, settingValue(definition, control)));
  } else {
    control = document.createElement('input');
    control.type = definition.type === 'number' ? 'number' : 'text';
    if (definition.min !== undefined)
      control.min = `${definition.min}`;
    if (definition.max !== undefined)
      control.max = `${definition.max}`;
    if (definition.step !== undefined)
      control.step = `${definition.step}`;
    control.addEventListener('input', () => scheduleConfigPatch(definition, settingValue(definition, control)));
  }

  controlWrap.append(control);
  if (output)
    controlWrap.append(output);
  row.append(label, controlWrap);
  app.settingControls.set(definition.key, {definition, control, output, name, note});
  return row;
}

function buildSettings() {
  for (const definition of SETTINGS) {
    const target = document.querySelector(`.setting-list[data-section="${definition.section}"]`);
    target?.append(createSettingRow(definition));
  }
  updateEmptySettingSections();
}

function updateEmptySettingSections() {
  document.querySelectorAll('.settings-section').forEach(section => {
    if (section.classList.contains('reset-section')) {
      section.classList.remove('is-empty');
      return;
    }
    const hasRows = Boolean(section.querySelector('.setting-row'));
    const hasDetails = Boolean(section.querySelector('.state-details'));
    section.classList.toggle('is-empty', !hasRows && !hasDetails);
  });
}

function updateSettingTexts() {
  for (const {definition, control, name, note} of app.settingControls.values()) {
    name.textContent = settingName(definition);
    if (note)
      note.textContent = settingNote(definition);
    if (definition.type === 'select') {
      const currentValue = control.value;
      rebuildSelectOptions(definition, control, currentValue);
      control.value = currentValue;
    }
  }
}

function populateSettings() {
  app.populatingSettings = true;
  for (const definition of SETTINGS)
    applySettingControlValue(definition, app.global[definition.key]);
  app.populatingSettings = false;
}

function normalizePropertyValue(type, value, fallback = null) {
  switch (type) {
  case 'bool':
    if (typeof value === 'boolean')
      return value;
    if (typeof value === 'number')
      return Math.abs(value) >= 0.0001;
    if (typeof value === 'string') {
      const normalized = value.trim().toLowerCase();
      if (['', '0', 'false', 'off', 'no'].includes(normalized))
        return false;
      if (['1', 'true', 'on', 'yes'].includes(normalized))
        return true;
    }
    return Boolean(fallback);
  case 'slider': {
    const parsed = typeof value === 'number' ? value : Number.parseFloat(`${value ?? ''}`.trim());
    if (Number.isFinite(parsed))
      return parsed;
    return Number.isFinite(fallback) ? fallback : 0;
  }
  case 'color':
    if (Array.isArray(value))
      return value.map(component => `${component}`).join(' ');
    if (typeof value === 'string')
      return value.trim();
    if (typeof value === 'number' && Number.isFinite(value))
      return `${value}`;
    return typeof fallback === 'string' ? fallback : '';
  case 'combo':
  case 'directory':
  case 'file':
  case 'group':
  case 'scenetexture':
  case 'text':
  case 'textinput':
    if (typeof value === 'string')
      return value;
    if (typeof value === 'number' && Number.isFinite(value))
      return `${value}`;
    if (typeof value === 'boolean')
      return value ? 'true' : 'false';
    return typeof fallback === 'string' ? fallback : '';
  default:
    return value ?? fallback;
  }
}

function propertyValuesEqual(type, left, right) {
  const normalizedLeft = normalizePropertyValue(type, left);
  const normalizedRight = normalizePropertyValue(type, right);
  if (type === 'slider')
    return Math.abs(normalizedLeft - normalizedRight) < 0.0001;
  return normalizedLeft === normalizedRight;
}

function resolvePropertyValue(property, overrides = {}) {
  const hasOverride = Object.prototype.hasOwnProperty.call(overrides, property.name);
  return normalizePropertyValue(
    property.type,
    hasOverride ? overrides[property.name] : property.defaultValue,
    property.defaultValue,
  );
}

function payloadToOverrides(project, payload) {
  const source = parseJsonObject(payload);
  const overrides = {};
  for (const property of project?.sceneProperties ?? []) {
    if (!editablePropertyTypes.has(property.type))
      continue;
    if (!Object.prototype.hasOwnProperty.call(source, property.name))
      continue;
    const entry = source[property.name];
    const value = entry && typeof entry === 'object' && !Array.isArray(entry) && Object.prototype.hasOwnProperty.call(entry, 'value')
      ? entry.value
      : entry;
    overrides[property.name] = normalizePropertyValue(property.type, value, property.defaultValue);
  }
  return overrides;
}

function normalizeWebFilesystemPropertyValue(value) {
  if (typeof value !== 'string')
    return value;
  const trimmed = value.trim();
  if (!trimmed)
    return '';
  if (/^[A-Za-z]:[\\/]/.test(trimmed))
    return trimmed;
  if (trimmed.startsWith('/'))
    return trimmed.replace(/^\/+/, '');
  return trimmed;
}

function buildSceneUserPropertyPayload(project, overrides = {}) {
  const payload = {};
  for (const property of project?.sceneProperties ?? []) {
    if (!property.editable)
      continue;
    if (!Object.prototype.hasOwnProperty.call(overrides, property.name))
      continue;
    const value = normalizePropertyValue(property.type, overrides[property.name], property.defaultValue);
    if (propertyValuesEqual(property.type, value, property.defaultValue))
      continue;
    payload[property.name] = {
      type: property.type,
      value,
    };
  }
  return payload;
}

function buildWebUserPropertyPayload(project, overrides = {}) {
  const payload = {};
  for (const property of project?.sceneProperties ?? []) {
    if (!property.editable)
      continue;
    let value = resolvePropertyValue(property, overrides);
    const entry = {value};
    if (['file', 'directory', 'scenetexture'].includes(property.type)) {
      value = normalizeWebFilesystemPropertyValue(value);
      entry.value = value;
    }
    if (property.type === 'combo') {
      const option = property.options.find(item => item.value === `${value}`);
      if (option) {
        entry.value = option.rawValue;
        entry.text = option.text;
      }
    }
    payload[property.name] = entry;
  }
  return payload;
}

function buildUserPropertyPayload(project, overrides = {}) {
  if (project?.type === 'web')
    return buildWebUserPropertyPayload(project, overrides);
  if (project?.type === 'scene')
    return buildSceneUserPropertyPayload(project, overrides);
  return {};
}

function storedPayloadForProject(project) {
  if (!project)
    return {};
  const wallpapers = app.state?.wallpapers ?? {};
  const stored = wallpapers?.[project.path]?.['user-properties'];
  if (stored !== undefined)
    return cloneJson(stored);
  if (app.global?.['project-path'] === project.path)
    return parseJsonObject(app.global?.['user-properties']);
  return {};
}

function setStoredPayloadForProject(project, payload) {
  if (!project)
    return;
  if (!app.state.wallpapers)
    app.state.wallpapers = {};
  app.state.wallpapers[project.path] = {'user-properties': cloneJson(payload)};
  if (app.global['project-path'] === project.path)
    app.global['user-properties'] = JSON.stringify(payload);
  updateStateOutput();
}

function buildValueMap(project, overrides = {}) {
  const valueMap = {};
  /*
   * Conditions are evaluated against effective user-visible values, not the raw
   * defaults from project.json. Persisted per-wallpaper overrides therefore win
   * whenever a property condition references another property, matching the old
   * Vivid Browse inspector semantics.
   */
  for (const property of project?.sceneProperties ?? [])
    valueMap[property.name] = resolvePropertyValue(property, overrides);
  return valueMap;
}

function isTruthy(value) {
  if (typeof value === 'boolean')
    return value;
  if (typeof value === 'number')
    return Math.abs(value) >= 0.0001;
  if (typeof value === 'string') {
    const normalized = value.trim().toLowerCase();
    return normalized !== '' && normalized !== '0' && normalized !== 'false';
  }
  return Boolean(value);
}

function parseExpressionNumber(value) {
  if (typeof value === 'number' && Number.isFinite(value))
    return value;
  if (typeof value !== 'string')
    return null;
  const trimmed = value.trim();
  if (!trimmed)
    return null;
  const parsed = Number.parseFloat(trimmed);
  return Number.isFinite(parsed) && `${parsed}` === `${Number(trimmed)}` ? parsed : null;
}

class SceneConditionParser {
  constructor(project, expression, valueMap, enabledMap, stack) {
    this.project = project;
    this.expression = expression ?? '';
    this.valueMap = valueMap;
    this.enabledMap = enabledMap;
    this.stack = stack;
    this.index = 0;
    this.propertyMap = new Map((project?.sceneProperties ?? []).map(property => [property.name, property]));
  }

  parse() {
    const value = this.parseOr();
    this.skipWhitespace();
    return value !== undefined && this.index === this.expression.length ? isTruthy(value) : false;
  }

  parseOr() {
    let value = this.parseAnd();
    while (value !== undefined) {
      this.skipWhitespace();
      if (!this.consume('||'))
        break;
      const right = this.parseAnd();
      if (right === undefined)
        return undefined;
      value = isTruthy(value) || isTruthy(right);
    }
    return value;
  }

  parseAnd() {
    let value = this.parseComparison();
    while (value !== undefined) {
      this.skipWhitespace();
      if (!this.consume('&&'))
        break;
      const right = this.parseComparison();
      if (right === undefined)
        return undefined;
      value = isTruthy(value) && isTruthy(right);
    }
    return value;
  }

  parseComparison() {
    let value = this.parseUnary();
    if (value === undefined)
      return undefined;

    while (true) {
      this.skipWhitespace();
      const operator = this.consumeOperator(['==', '!=', '<=', '>=', '<', '>']);
      if (!operator)
        break;

      const right = this.parseUnary();
      if (right === undefined)
        return undefined;

      if (operator === '==') {
        value = this.areEqual(value, right);
        continue;
      }
      if (operator === '!=') {
        value = !this.areEqual(value, right);
        continue;
      }

      const leftNumber = parseExpressionNumber(value);
      const rightNumber = parseExpressionNumber(right);
      if (leftNumber === null || rightNumber === null)
        return undefined;

      if (operator === '<')
        value = leftNumber < rightNumber;
      else if (operator === '<=')
        value = leftNumber <= rightNumber;
      else if (operator === '>')
        value = leftNumber > rightNumber;
      else if (operator === '>=')
        value = leftNumber >= rightNumber;
    }
    return value;
  }

  parseUnary() {
    this.skipWhitespace();
    if (this.consume('!')) {
      const value = this.parseUnary();
      return value === undefined ? undefined : !isTruthy(value);
    }
    return this.parsePrimary();
  }

  parsePrimary() {
    this.skipWhitespace();
    if (this.consume('(')) {
      const value = this.parseOr();
      this.skipWhitespace();
      return this.consume(')') ? value : undefined;
    }

    const quoted = this.parseQuotedString();
    if (quoted !== undefined)
      return quoted;

    const identifier = this.parseIdentifier();
    if (identifier) {
      if (identifier === 'true')
        return true;
      if (identifier === 'false')
        return false;
      const propertyName = this.consume('.value') ? identifier : identifier;
      if (!this.isPropertyEnabled(propertyName))
        return undefined;
      return Object.prototype.hasOwnProperty.call(this.valueMap, propertyName)
        ? this.valueMap[propertyName]
        : undefined;
    }

    return this.parseNumber();
  }

  parseQuotedString() {
    const quote = this.expression[this.index];
    if (quote !== '"' && quote !== '\'')
      return undefined;
    this.index++;
    let value = '';
    while (this.index < this.expression.length) {
      const current = this.expression[this.index++];
      if (current === quote)
        return value;
      if (current === '\\' && this.index < this.expression.length) {
        value += this.expression[this.index++];
        continue;
      }
      value += current;
    }
    return undefined;
  }

  parseIdentifier() {
    const match = this.expression.slice(this.index).match(/^[A-Za-z_][A-Za-z0-9_]*/);
    if (!match)
      return null;
    this.index += match[0].length;
    return match[0];
  }

  parseNumber() {
    const match = this.expression.slice(this.index).match(/^-?(?:\d+(?:\.\d*)?|\.\d+)/);
    if (!match)
      return undefined;
    this.index += match[0].length;
    const parsed = Number.parseFloat(match[0]);
    return Number.isFinite(parsed) ? parsed : undefined;
  }

  skipWhitespace() {
    while (this.index < this.expression.length && /\s/.test(this.expression[this.index]))
      this.index++;
  }

  consume(token) {
    if (!this.expression.startsWith(token, this.index))
      return false;
    this.index += token.length;
    return true;
  }

  consumeOperator(operators) {
    for (const operator of operators) {
      if (this.consume(operator))
        return operator;
    }
    return null;
  }

  areEqual(left, right) {
    const leftNumber = parseExpressionNumber(left);
    const rightNumber = parseExpressionNumber(right);
    if (leftNumber !== null && rightNumber !== null)
      return Math.abs(leftNumber - rightNumber) < 0.0001;
    if (typeof left === 'string' && typeof right === 'string')
      return left.trim() === right.trim();
    return isTruthy(left) === isTruthy(right);
  }

  isPropertyEnabled(propertyName) {
    if (!this.propertyMap.has(propertyName))
      return false;
    if (this.enabledMap.has(propertyName))
      return this.enabledMap.get(propertyName);
    if (this.stack.includes(propertyName))
      return false;
    const property = this.propertyMap.get(propertyName);
    if (!property.condition) {
      this.enabledMap.set(propertyName, true);
      return true;
    }
    this.stack.push(propertyName);
    const parser = new SceneConditionParser(
      this.project,
      property.condition,
      this.valueMap,
      this.enabledMap,
      this.stack,
    );
    const result = parser.parse();
    this.stack.pop();
    this.enabledMap.set(propertyName, result);
    return result;
  }
}

function isPropertyVisible(project, property, valueMap, enabledMap = new Map()) {
  if (!property)
    return false;
  if (!property.condition)
    return true;
  return new SceneConditionParser(project, property.condition, valueMap, enabledMap, []).parse();
}

function updateInspectorVisibility() {
  if (!app.selectedProject)
    return;
  const valueMap = buildValueMap(app.selectedProject, app.selectedOverrides);
  const enabledMap = new Map();
  for (const section of app.propertySections) {
    const groupVisible = section.groupProperty
      ? isPropertyVisible(app.selectedProject, section.groupProperty, valueMap, enabledMap)
      : true;
    let visibleRowCount = 0;

    for (const entry of section.rows) {
      const visible = groupVisible && isPropertyVisible(app.selectedProject, entry.property, valueMap, enabledMap);
      entry.row.classList.toggle('is-hidden', !visible);
      if (visible)
        visibleRowCount++;
    }

    const sectionVisible = groupVisible && visibleRowCount > 0;
    section.element.classList.toggle('is-hidden', !sectionVisible);
    section.title?.classList.toggle('is-hidden', !sectionVisible);
  }
}

function colorComponents(value) {
  const parts = `${value ?? ''}`.split(/[ ,]+/).map(Number).filter(Number.isFinite);
  while (parts.length < 3)
    parts.push(0);
  return parts.map((component, index) => {
    if (component > 1)
      component /= 255;
    if (index < 3)
      return Math.max(0, Math.min(1, component));
    return Math.max(0, Math.min(1, component));
  });
}

function colorToHex(value) {
  const [r, g, b] = colorComponents(value);
  return `#${[r, g, b].map(component => Math.round(component * 255).toString(16).padStart(2, '0')).join('')}`;
}

function hexToColorString(hex, defaultValue) {
  const match = `${hex ?? ''}`.match(/^#?([0-9a-f]{6})$/i);
  if (!match)
    return normalizePropertyValue('color', defaultValue, '0 0 0');
  const raw = match[1];
  const components = [0, 2, 4].map(index => Number.parseInt(raw.slice(index, index + 2), 16) / 255);
  const defaultComponents = colorComponents(defaultValue);
  if (defaultComponents.length >= 4)
    components.push(defaultComponents[3]);
  return components.map(component => Number(component.toFixed(6))).join(' ');
}

async function sendWallpaperProperties() {
  if (!app.selectedProject)
    return;
  const payload = buildUserPropertyPayload(app.selectedProject, app.selectedOverrides);
  setStoredPayloadForProject(app.selectedProject, payload);
  setLocalizedStatus('status.applyingProperties', 'working');
  try {
    await requestJson('/api/wallpaper/properties', {
      method: 'POST',
      body: JSON.stringify({
        projectPath: app.selectedProject.path,
        wallpaperId: app.selectedProject.path,
        projectType: app.selectedProject.type,
        properties: payload,
      }),
    });
    setLocalizedStatus('status.propertiesApplied', 'ok');
  } catch (error) {
    setStatus(error.message, 'error');
  }
}

function scheduleWallpaperPropertyUpdate() {
  if (app.propertyTimer)
    window.clearTimeout(app.propertyTimer);
  app.propertyTimer = window.setTimeout(() => {
    app.propertyTimer = 0;
    sendWallpaperProperties();
  }, 140);
}

function updateProperty(property, rawValue) {
  const nextValue = normalizePropertyValue(property.type, rawValue, property.defaultValue);
  if (propertyValuesEqual(property.type, nextValue, property.defaultValue))
    delete app.selectedOverrides[property.name];
  else
    app.selectedOverrides[property.name] = nextValue;
  updateInspectorVisibility();
  scheduleWallpaperPropertyUpdate();
}

function createPropertyRow(property) {
  const row = document.createElement('div');
  row.className = 'property-row';
  row.dataset.propertyName = property.name;

  if (property.type === 'text') {
    row.classList.add('is-text');
    const note = createMarkupElement(property.text, property.name, 'property-text rich-markup', app.selectedProject);
    row.append(note);
    app.propertyRows.set(property.name, row);
    return row;
  }

  const label = document.createElement('div');
  label.className = 'property-label';
  const title = createMarkupElement(property.text, property.name, 'property-name', app.selectedProject);
  label.append(title);

  const controlWrap = document.createElement('div');
  controlWrap.className = 'property-control';
  const currentValue = resolvePropertyValue(property, app.selectedOverrides);

  if (property.type === 'bool') {
    const control = document.createElement('input');
    control.type = 'checkbox';
    control.checked = Boolean(currentValue);
    control.addEventListener('change', () => updateProperty(property, control.checked));
    controlWrap.append(control);
  } else if (property.type === 'slider') {
    const control = document.createElement('input');
    control.type = 'range';
    const lower = Number.isFinite(property.min) ? property.min : 0;
    const upper = Number.isFinite(property.max) ? property.max : Math.max(lower + 1, Number(currentValue) || lower + 1);
    control.min = `${Math.min(lower, upper)}`;
    control.max = `${Math.max(lower, upper)}`;
    control.step = `${Number.isFinite(property.step) ? property.step : 0.1}`;
    control.value = `${currentValue}`;
    const output = document.createElement('span');
    output.className = 'value-output';
    output.textContent = `${Number(currentValue).toFixed(control.step.includes('.') ? 2 : 0)}`;
    control.addEventListener('input', () => {
      output.textContent = `${Number(control.value).toFixed(control.step.includes('.') ? 2 : 0)}`;
      updateProperty(property, Number.parseFloat(control.value));
    });
    controlWrap.append(control, output);
  } else if (property.type === 'combo') {
    const control = document.createElement('select');
    for (const option of property.options ?? []) {
      const item = document.createElement('option');
      item.value = option.value;
      item.textContent = stripMarkup(option.text, option.value);
      control.append(item);
    }
    control.value = `${currentValue}`;
    control.addEventListener('change', () => updateProperty(property, control.value));
    controlWrap.append(control);
  } else if (property.type === 'color') {
    const control = document.createElement('input');
    control.type = 'color';
    control.value = colorToHex(currentValue);
    control.addEventListener('input', () => updateProperty(property, hexToColorString(control.value, property.defaultValue)));
    controlWrap.append(control);
  } else if (['textinput', 'file', 'directory', 'scenetexture'].includes(property.type)) {
    const control = document.createElement('input');
    control.type = 'text';
    control.value = `${currentValue ?? ''}`;
    control.spellcheck = false;
    control.addEventListener('input', () => updateProperty(property, control.value));
    controlWrap.append(control);
  } else {
    const note = document.createElement('div');
    note.className = 'property-note';
    note.textContent = t('property.unsupported');
    controlWrap.append(note);
  }

  row.append(label, controlWrap);
  app.propertyRows.set(property.name, row);
  return row;
}

function renderInspectorMessage(title, subtitle) {
  dom.inspectorTitle.textContent = title;
  dom.inspectorSubtitle.textContent = subtitle;
  dom.inspectorType.textContent = t('type.none');
  dom.inspectorPreview.replaceChildren();
  dom.inspectorProperties.replaceChildren();
  app.propertyRows.clear();
  app.propertySections = [];
  updateInspectorPanelState();
}

function setInspectorOpen(open, {focusPanel = false} = {}) {
  app.inspectorOpen = Boolean(open && app.selectedProject);
  updateInspectorPanelState();
  if (focusPanel && app.inspectorOpen && !wideBrowseQuery.matches)
    dom.inspectorTitle.focus?.();
}

function updateInspectorPanelState() {
  const canOpen = Boolean(app.selectedProject);
  if (!canOpen)
    app.inspectorOpen = false;

  const isOpen = canOpen && app.inspectorOpen;
  dom.browseLayout.classList.toggle('is-inspector-open', isOpen);
  dom.inspector.setAttribute('aria-hidden', String(!isOpen));

  if (!isOpen && !wideBrowseQuery.matches)
    dom.inspectorBackButton.style.display = 'none';
  else
    dom.inspectorBackButton.style.display = '';
}

function renderInspector() {
  const project = app.selectedProject;
  if (!project) {
    renderInspectorMessage(t('inspector.wallpaper'), t('inspector.noneSelected'));
    return;
  }

  dom.inspectorTitle.textContent = project.title || project.basename || t('inspector.wallpaper');
  dom.inspectorSubtitle.textContent = project.path;
  dom.inspectorType.textContent = formatType(project.type);
  dom.inspectorPreview.replaceChildren();
  if (project.previewPath) {
    const image = document.createElement('img');
    image.alt = '';
    image.src = `/api/thumbnail?path=${encodeURIComponent(project.previewPath)}`;
    dom.inspectorPreview.append(image);
  }

  dom.inspectorProperties.replaceChildren();
  app.propertyRows.clear();
  app.propertySections = [];
  const properties = project.sceneProperties ?? [];
  if (properties.length === 0) {
    const empty = document.createElement('p');
    empty.className = 'empty-state is-visible';
    empty.textContent = t('empty.noConfigurableProperties');
    dom.inspectorProperties.append(empty);
    return;
  }

  let currentGroup = document.createElement('section');
  currentGroup.className = 'property-group';
  dom.inspectorProperties.append(currentGroup);
  let currentSection = {
    groupProperty: null,
    element: currentGroup,
    title: null,
    rows: [],
  };
  app.propertySections.push(currentSection);

  for (const property of properties) {
    if (property.type === 'group') {
      currentGroup = document.createElement('section');
      currentGroup.className = 'property-group';
      const title = createMarkupElement(property.text, property.name, 'property-group-title rich-markup', project);
      currentGroup.append(title);
      dom.inspectorProperties.append(currentGroup);
      currentSection = {
        groupProperty: property,
        element: currentGroup,
        title,
        rows: [],
      };
      app.propertySections.push(currentSection);
      continue;
    }
    const row = createPropertyRow(property);
    currentGroup.append(row);
    currentSection.rows.push({property, row});
  }
  updateInspectorVisibility();
  enhanceAllSelects(dom.inspectorProperties);
  updateInspectorPanelState();
}

function scrollProjectCardIntoView(projectPath = app.global['project-path']) {
  if (!projectPath || !dom.projectGrid)
    return;
  const card = [...dom.projectGrid.querySelectorAll('.project-card')]
    .find(item => item.dataset.projectPath === projectPath);
  card?.scrollIntoView({block: 'nearest', inline: 'nearest'});
}

function closeInspectorAndRestoreProject() {
  const projectPath = app.selectedProject?.path ?? app.global['project-path'];
  setInspectorOpen(false);
  requestAnimationFrame(() => scrollProjectCardIntoView(projectPath));
}

function updateActiveProjectCard(projectPath = app.global['project-path']) {
  for (const card of dom.projectGrid.querySelectorAll('.project-card'))
    card.classList.toggle('is-active', card.dataset.projectPath === projectPath);
}

async function selectProject(project, {openInspector = false} = {}) {
  app.selectedProject = project;
  app.selectedOverrides = payloadToOverrides(project, storedPayloadForProject(project));
  app.global['project-path'] = project.path;
  updateActiveProjectCard(project.path);
  renderInspector();
  if (openInspector)
    setInspectorOpen(true, {focusPanel: true});

  const payload = buildUserPropertyPayload(project, app.selectedOverrides);
  setStoredPayloadForProject(project, payload);
  setLocalizedStatus('status.selectingWallpaper', 'working');
  try {
    await requestJson('/api/wallpaper/select', {
      method: 'POST',
      body: JSON.stringify({
        projectPath: project.path,
        wallpaperId: project.path,
        projectType: project.type,
        properties: payload,
      }),
    });
    setLocalizedStatus('status.wallpaperSelected', 'ok');
    populateSettings();
  } catch (error) {
    setStatus(error.message, 'error');
  }
}

async function openProjectSettings(project) {
  await selectProject(project, {openInspector: true});
  if (!wideBrowseQuery.matches)
    dom.inspector.scrollIntoView({block: 'start'});
}

function buildProjectSearchText(project) {
  return [
    project.title,
    project.basename,
    project.type,
    project.description,
    ...(project.tags ?? []),
  ].join(' ').toLowerCase();
}

function prepareProjects(projects = []) {
  const prepared = [];
  app.projectByPath = new Map();
  for (const project of projects) {
    const item = {
      ...project,
      searchText: buildProjectSearchText(project),
    };
    prepared.push(item);
    app.projectByPath.set(item.path, item);
  }
  return prepared;
}

function projectByPath(path) {
  return app.projectByPath.get(path) ?? null;
}

function renderFilterGroup(title, group, values, state) {
  const details = document.createElement('details');
  details.className = 'filter-group';

  const summary = document.createElement('summary');
  summary.textContent = title;
  const options = document.createElement('div');
  options.className = 'filter-options';

  for (const value of values) {
    const label = document.createElement('label');
    const control = document.createElement('input');
    control.type = 'checkbox';
    control.dataset.filterGroup = group;
    control.dataset.filterValue = value;
    control.checked = state[group]?.[value] !== false;
    label.append(control, document.createTextNode(formatFilterValue(group, value)));
    options.append(label);
  }

  details.append(summary, options);
  return details;
}

function renderDynamicFilters() {
  app.browserFilterState = normalizeBrowserFilterState(app.browserFilterState, app.projects);
  const state = app.browserFilterState;
  dom.dynamicFilters.replaceChildren(
    renderFilterGroup(t('filter.contentRating'), 'contentrating', contentRatings, state),
    renderFilterGroup(t('filter.tags'), 'tags', availableProjectTags(app.projects), state),
  );
}

function syncBrowserControlsFromState() {
  const sortKey = browserSortKeys.has(app.global['project-browser-sort-key'])
    ? app.global['project-browser-sort-key']
    : 'name';
  dom.projectSort.value = sortKey;

  app.browserFilterState = normalizeBrowserFilterState(
    app.global['project-browser-filter-state'],
    app.projects,
  );
  for (const control of dom.typeFilters)
    control.checked = app.browserFilterState.type[control.value] !== false;
  renderDynamicFilters();
}

function visibleProjects() {
  const query = dom.projectSearch.value.trim().toLowerCase();
  const filterState = app.browserFilterState ?? normalizeBrowserFilterState(app.browserFilterState, app.projects);
  const projects = app.projects.filter(project => {
    if (!filterState.type[project.type])
      return false;
    if (filterState.contentrating[project.contentrating ?? 'Everyone'] === false)
      return false;
    if (projectFilterTags(project).some(tag => filterState.tags[tag] === false))
      return false;
    if (query && !(project.searchText ?? buildProjectSearchText(project)).includes(query))
      return false;
    return true;
  });

  switch (dom.projectSort.value) {
  case 'updated-time':
    projects.sort((left, right) => (right.updatedTime ?? 0) - (left.updatedTime ?? 0) || `${left.title}`.localeCompare(`${right.title}`));
    break;
  case 'type':
    projects.sort((left, right) => `${left.type}`.localeCompare(`${right.type}`) || `${left.title}`.localeCompare(`${right.title}`));
    break;
  case 'name':
  default:
    projects.sort((left, right) => `${left.title || left.basename}`.localeCompare(`${right.title || right.basename}`));
    break;
  }
  return projects;
}

function createProjectCard(project, index) {
  const card = document.createElement('article');
  card.className = 'project-card';
  card.dataset.projectPath = project.path;
  card.classList.toggle('is-active', app.global['project-path'] === project.path);
  card.title = project.path;

  const selectButton = document.createElement('button');
  selectButton.type = 'button';
  selectButton.className = 'project-select-button';

  const thumb = document.createElement('div');
  thumb.className = 'project-thumb';
  if (project.previewPath) {
    const image = document.createElement('img');
    image.alt = '';
    image.loading = index < 12 ? 'eager' : 'lazy';
    image.decoding = 'async';
    image.fetchPriority = index < 12 ? 'high' : 'low';
    image.src = `/api/thumbnail?path=${encodeURIComponent(project.previewPath)}`;
    thumb.append(image);
  } else {
    const fallback = document.createElement('div');
    fallback.className = 'project-thumb-fallback';
    fallback.textContent = formatType(project.type);
    thumb.append(fallback);
  }

  const meta = document.createElement('div');
  meta.className = 'project-meta';
  const title = document.createElement('div');
  title.className = 'project-title';
  title.textContent = project.title || project.basename || t('project.untitled');
  const subtitle = document.createElement('div');
  subtitle.className = 'project-subtitle';
  subtitle.textContent = t('project.subtitle', {
    type: formatType(project.type),
    detail: formatProjectDetail(project),
  });
  meta.append(title, subtitle);

  const settingsButton = document.createElement('button');
  settingsButton.type = 'button';
  settingsButton.className = 'project-settings-button';
  settingsButton.title = t('action.openWallpaperSettings');
  settingsButton.setAttribute('aria-label', t('action.openWallpaperSettings'));
  settingsButton.innerHTML = `
    <svg class="icon" viewBox="0 0 24 24" aria-hidden="true">
      <path d="M12 15.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7Z"></path>
      <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09a1.65 1.65 0 0 0-1-1.51 1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.6 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09a1.65 1.65 0 0 0 1.51-1 1.65 1.65 0 0 0-.33-1.82l-.06-.06A2 2 0 1 1 7.04 4.3l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51h.08a1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9c.22.62.8 1 1.51 1H21a2 2 0 1 1 0 4h-.09c-.7 0-1.29.38-1.51 1Z"></path>
    </svg>
  `;

  selectButton.append(thumb, meta);
  card.append(selectButton, settingsButton);
  return card;
}

function cancelProjectRender() {
  cancelIdleWork(app.projectRenderHandle);
  app.projectRenderHandle = null;
}

function appendProjectRenderBatch(projects, token, startIndex, startedAt, deadline = null) {
  if (token !== app.projectRenderToken)
    return;

  /*
   * Large Workshop libraries can contain hundreds or thousands of projects. A
   * single synchronous render makes the browser parse metadata, create images,
   * attach listeners, and recalculate layout in one long task. Build the first
   * screen immediately, then let idle slices fill the rest so opening the WebUI
   * remains responsive while the visual design stays unchanged.
   */
  const fragment = document.createDocumentFragment();
  let index = startIndex;
  let rendered = 0;
  const hasIdleBudget = () =>
    deadline && !deadline.didTimeout && typeof deadline.timeRemaining === 'function' && deadline.timeRemaining() > 4;

  while (index < projects.length) {
    fragment.append(createProjectCard(projects[index], index));
    index++;
    rendered++;
    if (rendered >= projectRenderBatchSize && !hasIdleBudget())
      break;
  }

  dom.projectGrid.append(fragment);

  if (index >= projects.length) {
    app.projectRenderHandle = null;
    logSlowWork('project grid render', startedAt, {count: projects.length});
    return;
  }

  app.projectRenderHandle = scheduleIdleWork(nextDeadline =>
    appendProjectRenderBatch(projects, token, index, startedAt, nextDeadline));
}

function renderProjects() {
  cancelProjectRender();
  const startedAt = performance.now();
  const projects = visibleProjects();
  const token = ++app.projectRenderToken;
  dom.projectGrid.replaceChildren();
  dom.projectCount.textContent = formatWallpaperCount(projects.length);
  dom.projectEmpty.classList.toggle('is-visible', projects.length === 0);
  if (projects.length === 0) {
    dom.projectEmpty.textContent = app.global['change-wallpaper-directory-path']
      ? t('empty.noMatches')
      : t('empty.setLibrary');
    return;
  }

  appendProjectRenderBatch(projects, token, 0, startedAt);
}

function handleProjectGridClick(event) {
  const target = event.target instanceof Element ? event.target : null;
  if (!target)
    return;
  const card = target.closest('.project-card');
  if (!card || !dom.projectGrid.contains(card))
    return;
  const project = projectByPath(card.dataset.projectPath);
  if (!project)
    return;
  if (target.closest('.project-settings-button')) {
    event.stopPropagation();
    openProjectSettings(project);
    return;
  }
  if (target.closest('.project-select-button'))
    selectProject(project);
}

async function refreshState({projects = false} = {}) {
  setLocalizedStatus('status.connecting', 'working');
  try {
    const response = await requestJson('/api/state');
    app.state = controlPayload(response);
    app.global = app.state.global ?? {};
    updateStateOutput();
    populateSettings();
    setLocalizedStatus('status.connected', 'ok');
    if (projects)
      await refreshProjects();
  } catch (error) {
    setStatus(error.message, 'error');
    app.state = {error: error.message};
    updateStateOutput();
  }
}

function projectsRequestPath() {
  const libraryPath = app.global['change-wallpaper-directory-path'] ?? '';
  if (!libraryPath)
    return '/api/projects';
  const params = new URLSearchParams({
    libraryPath,
    includeState: '0',
  });
  return `/api/projects?${params}`;
}

async function refreshProjects() {
  setLocalizedStatus('status.loadingWallpapers', 'working');
  try {
    const response = await requestJson(projectsRequestPath());
    if (response.state) {
      app.state = response.state;
      app.global = app.state.global ?? app.global;
    }
    app.projects = prepareProjects(response.projects ?? []);
    app.global['change-wallpaper-directory-path'] = response.libraryPath ?? '';
    applySettingControlValue(
      SETTINGS.find(item => item.key === 'change-wallpaper-directory-path'),
      app.global['change-wallpaper-directory-path'],
    );
    const activePath = app.global['project-path'];
    app.selectedProject = activePath
      ? projectByPath(activePath)
      : null;
    if (app.selectedProject)
      app.selectedOverrides = payloadToOverrides(app.selectedProject, storedPayloadForProject(app.selectedProject));
    else
      app.selectedOverrides = {};
    syncBrowserControlsFromState();
    updateStateOutput();
    renderProjects();
    renderInspector();
    setLocalizedStatus('status.loadedWallpapers', 'ok', {count: app.projects.length});
  } catch (error) {
    setStatus(error.message, 'error');
    app.projects = prepareProjects([]);
    renderProjects();
  }
}

function showView(viewId) {
  for (const view of dom.views)
    view.classList.toggle('is-active', view.id === viewId);
  const isSettings = viewId === 'settingsView';
  document.querySelector('.topbar').classList.toggle('is-settings', isSettings);
  dom.settingsBackButton.style.display = isSettings ? '' : 'none';
  dom.settingsButton.style.display = isSettings ? 'none' : '';
}

function refreshLocalizedUi() {
  localizeStaticDocument();
  if (dom.languageSelect)
    dom.languageSelect.value = app.locale;
  renderThemeButton();
  updateSettingTexts();
  if (app.statusLocalization) {
    const {key, state, replacements} = app.statusLocalization;
    setLocalizedStatus(key, state, replacements);
  }
  renderDynamicFilters();
  renderProjects();
  renderInspector();
  updateInspectorPanelState();
}

function setLocale(locale, {persist = true} = {}) {
  app.locale = normalizeLocale(locale);
  if (persist)
    window.localStorage.setItem(localeStorageKey, app.locale);
  refreshLocalizedUi();
}

function installEventHandlers() {
  dom.themeButton?.addEventListener('click', cycleThemeMode);
  dom.languageSelect?.addEventListener('change', () => setLocale(dom.languageSelect.value));
  systemThemeQuery.addEventListener('change', () => {
    if (app.themeMode === 'system')
      applyThemeMode('system', {persist: false});
  });
  dom.resetDefaultsButton?.addEventListener('click', resetAllDefaults);
  dom.settingsButton?.addEventListener('click', () => showView('settingsView'));
  dom.settingsBackButton?.addEventListener('click', () => showView('browseView'));
  dom.refreshButton.addEventListener('click', () => refreshState({projects: true}));
  dom.inspectorBackButton.addEventListener('click', closeInspectorAndRestoreProject);
  dom.projectGrid.addEventListener('click', handleProjectGridClick);
  dom.stateDetails?.addEventListener('toggle', scheduleStateOutputRender);
  wideBrowseQuery.addEventListener('change', updateInspectorPanelState);
  let searchRenderTimer = 0;
  dom.projectSearch.addEventListener('input', () => {
    window.clearTimeout(searchRenderTimer);
    // Empty queries reset immediately; typing is debounced so each keystroke
    // doesn't filter+sort the whole library and rebuild the grid.
    if (!dom.projectSearch.value.trim()) {
      renderProjects();
      return;
    }
    searchRenderTimer = window.setTimeout(renderProjects, 160);
  });
  dom.projectSort.addEventListener('change', () => {
    renderProjects();
    persistBrowserSortKey();
  });
  dom.typeFilters.forEach(filter => filter.addEventListener('change', () => {
    app.browserFilterState = browserFilterFromControls();
    renderProjects();
    persistBrowserFilterState();
  }));
  dom.dynamicFilters.addEventListener('change', event => {
    if (!(event.target instanceof HTMLInputElement))
      return;
    app.browserFilterState = browserFilterFromControls();
    renderProjects();
    persistBrowserFilterState();
  });
  dom.filterDropdownButton?.addEventListener('click', event => {
    event.stopPropagation();
    dom.filterDropdown.classList.toggle('is-open');
  });
  document.addEventListener('click', event => {
    if (dom.filterDropdown && dom.filterDropdownButton &&
        !dom.filterDropdownButton.contains(event.target) &&
        !dom.filterDropdown.contains(event.target)) {
      dom.filterDropdown.classList.remove('is-open');
    }
  });
  dom.settingsForm.addEventListener('submit', event => event.preventDefault());
}

/* ===================================================
   Custom Select Dropdown
   =================================================== */
function enhanceSelect(nativeSelect) {
  if (nativeSelect.dataset.enhanced === '1') return;

  const wrapper = document.createElement('div');
  wrapper.className = 'custom-select';

  const trigger = document.createElement('div');
  trigger.className = 'custom-select-trigger';
  trigger.tabIndex = 0;

  const text = document.createElement('span');
  text.className = 'custom-select-trigger-text';

  const arrow = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  arrow.setAttribute('class', 'custom-select-arrow');
  arrow.setAttribute('viewBox', '0 0 24 24');
  arrow.setAttribute('fill', 'none');
  arrow.setAttribute('stroke', 'currentColor');
  arrow.setAttribute('stroke-width', '2.5');
  arrow.setAttribute('stroke-linecap', 'round');
  arrow.setAttribute('stroke-linejoin', 'round');
  arrow.innerHTML = '<path d="m6 9 6 6 6-6"/>';

  trigger.append(text, arrow);

  const dropdown = document.createElement('div');
  dropdown.className = 'custom-select-dropdown';

  wrapper.append(trigger, dropdown);
  nativeSelect.parentNode.insertBefore(wrapper, nativeSelect);
  wrapper.append(nativeSelect);

  nativeSelect.style.cssText = 'position:absolute;overflow:hidden;width:1px;height:1px;clip:rect(0,0,0,0);opacity:0;pointer-events:none';
  nativeSelect.tabIndex = -1;
  nativeSelect.setAttribute('aria-hidden', 'true');
  nativeSelect.dataset.enhanced = '1';

  let isOpen = false;
  let activeIndex = -1;

  function syncText() {
    const idx = nativeSelect.selectedIndex;
    text.textContent = idx >= 0 ? nativeSelect.options[idx].textContent : '';
  }

  function renderOptions() {
    dropdown.replaceChildren();
    const selIdx = nativeSelect.selectedIndex;
    for (const option of nativeSelect.options) {
      const item = document.createElement('div');
      item.className = 'custom-select-option';
      item.textContent = option.textContent;
      if (option.index === selIdx) item.classList.add('is-selected');
      item.addEventListener('mousedown', evt => {
        evt.preventDefault();
        nativeSelect.value = option.value;
        nativeSelect.dispatchEvent(new Event('change', {bubbles: true}));
        syncText();
        close();
      });
      dropdown.append(item);
    }
  }

  function updateActive() {
    for (let i = 0; i < dropdown.children.length; i++)
      dropdown.children[i].classList.toggle('is-active', i === activeIndex);
    dropdown.children[activeIndex]?.scrollIntoView({block: 'nearest'});
  }

  function open() {
    if (isOpen) return;
    isOpen = true;
    wrapper.classList.add('is-open');
    renderOptions();
    activeIndex = nativeSelect.selectedIndex;
    updateActive();
    document.addEventListener('mousedown', onOutside, true);
    document.addEventListener('keydown', onKey);
  }

  function close() {
    if (!isOpen) return;
    isOpen = false;
    wrapper.classList.remove('is-open');
    document.removeEventListener('mousedown', onOutside, true);
    document.removeEventListener('keydown', onKey);
  }

  function onOutside(evt) { if (!wrapper.contains(evt.target)) close(); }

  function onKey(evt) {
    if (evt.key === 'Escape') { close(); trigger.focus(); return; }
    if (evt.key === 'Enter' || evt.key === ' ') {
      evt.preventDefault();
      if (isOpen && activeIndex >= 0 && activeIndex < dropdown.children.length) {
        nativeSelect.value = nativeSelect.options[activeIndex].value;
        nativeSelect.dispatchEvent(new Event('change', {bubbles: true}));
        syncText();
      }
      close();
      return;
    }
    if (evt.key === 'ArrowDown') {
      evt.preventDefault();
      activeIndex = Math.min(activeIndex + 1, dropdown.children.length - 1);
      updateActive();
      return;
    }
    if (evt.key === 'ArrowUp') {
      evt.preventDefault();
      activeIndex = Math.max(activeIndex - 1, 0);
      updateActive();
      return;
    }
  }

  trigger.addEventListener('mousedown', evt => {
    evt.preventDefault();
    isOpen ? close() : open();
  });

  trigger.addEventListener('keydown', evt => {
    if (evt.key === 'Enter' || evt.key === ' ' || evt.key === 'ArrowDown') {
      evt.preventDefault();
      open();
    }
  });

  new MutationObserver(() => {
    syncText();
    if (isOpen) renderOptions();
  }).observe(nativeSelect, {childList: true, subtree: true, attributes: true, attributeFilter: ['value']});

  syncText();
}

function enhanceAllSelects(root = document) {
  root.querySelectorAll('select:not([data-enhanced="1"])').forEach(enhanceSelect);
}

/* Patch rebuildSelectOptions so custom dropdowns stay in sync */
const _origRebuild = rebuildSelectOptions;
rebuildSelectOptions = function(def, ctrl, val) {
  _origRebuild(def, ctrl, val);
  if (ctrl.dataset.enhanced === '1') {
    const wrap = ctrl.closest('.custom-select');
    if (wrap) {
      const txt = wrap.querySelector('.custom-select-trigger-text');
      const sel = ctrl.options[ctrl.selectedIndex];
      if (txt && sel) txt.textContent = sel.textContent;
    }
  }
};

app.locale = detectInitialLocale();
app.themeMode = detectInitialThemeMode();
applyThemeMode(app.themeMode, {persist: false});
setLocalizedStatus('status.disconnected', 'neutral');
localizeStaticDocument();
if (dom.languageSelect)
  dom.languageSelect.value = app.locale;
buildSettings();
enhanceAllSelects();
installEventHandlers();
refreshLocalizedUi();
refreshState({projects: true});
