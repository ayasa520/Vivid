# Vivid

[English](README.md) | 简体中文

<p align="center">
  <img src="producer/resources/io.github.ayasa520.Vivid.svg" alt="Vivid 缩略图" width="160">
</p>

Vivid 是 Wallpaper Engine 的 Linux 开源重实现。

**本项目使用 VIBE CODING 开发。**

## 安装发行版

从 [GitHub Releases](https://github.com/ayasa520/Vivid/releases) 页面下载文件。
请使用 Release 附带的 Assets，不要下载 Actions 页面里的 artifact ZIP。Release Assets
已经解包，可以直接安装。

64 位 Intel/AMD 设备选择 `x86_64`，64 位 Arm 设备选择 `aarch64`。运行
`uname -m` 可以查看当前系统的架构。Vivid 需要安装 Flatpak producer（渲染端），
再根据桌面环境选择一个 consumer（桌面端）：

| 桌面环境 | Release 文件 |
| --- | --- |
| Producer 和控制界面 | `io.github.ayasa520.Vivid-<版本>-<架构>.flatpak` |
| GNOME Shell | `vivid-consumer-gnome-<版本>-<架构>.zip` |
| KDE Plasma | `vivid-consumer-kde-<版本>-<架构>.zip` |
| Hyprland、Sway、niri 及其他支持 layer-shell 的合成器 | `vivid-layer-shell-consumer-<版本>-<架构>` |

下面以 `1.0.6` 的 x86_64 文件为例。请在下载文件所在的目录运行命令。

### Producer

为当前用户安装 Flatpak bundle，然后从应用菜单或命令行启动 Vivid：

```sh
flatpak install --user ./io.github.ayasa520.Vivid-1.0.6-x86_64.flatpak
flatpak run io.github.ayasa520.Vivid
```

### GNOME Shell consumer

Release 中的 ZIP 可以直接交给 `gnome-extensions` 安装，支持 GNOME Shell 45 至
50：

```sh
gnome-extensions install --force ./vivid-consumer-gnome-1.0.6-x86_64.zip
gnome-extensions enable vivid-consumer-gnome@rikka.local
```

如果 GNOME Shell 没有加载刚安装的扩展，请注销后重新登录。

### KDE Plasma consumer

解压安装包，用 KPackage 安装，然后在“桌面和壁纸”设置中选择 **Vivid**：

```sh
vivid_kde_stage="$(mktemp -d)"
unzip ./vivid-consumer-kde-1.0.6-x86_64.zip -d "${vivid_kde_stage}"
kpackagetool6 --type Plasma/Wallpaper \
  --install "${vivid_kde_stage}/dev.rikka.vivid.consumer.kde"
```

更新已经安装的 KDE package 时，将 `--install` 换成 `--upgrade`。

### Layer-shell consumer

将可执行文件安装到当前用户的本地程序目录：

```sh
install -Dm755 ./vivid-layer-shell-consumer-1.0.6-x86_64 \
  "${HOME}/.local/bin/vivid-layer-shell-consumer"
```

先启动 producer，再运行 `vivid-layer-shell-consumer`。合成器的自启动配置见
[layer-shell consumer 指南](consumer/layer-shell/README.md)。

## 构建

Producer 的构建产物位于 `producer/.build`。

### Flatpak

```sh
tools/vivid.sh flatpak prefetch
tools/vivid.sh build flatpak
tools/vivid.sh flatpak run-appdir
```

`tools/vivid.sh flatpak prefetch` 会先下载并固定 Flatpak 构建所需的源文件。完成后，
可以使用缓存进行离线构建：

```sh
VIVID_FLATPAK_DISABLE_DOWNLOAD=1 tools/vivid.sh build flatpak
```

Flatpak manifest 模板位于
`producer/packaging/flatpak/io.github.ayasa520.Vivid.yml`。构建脚本会将它渲染到
`producer/.build/flatpak-manifest`。默认生成的 bundle 为
`producer/.build/io.github.ayasa520.Vivid-1.0.0-<架构>.flatpak`。

通过环境变量设置 Flatpak 软件版本：

```sh
VIVID_FLATPAK_APP_VERSION=1.0.0 \
VIVID_FLATPAK_RELEASE_DATE=2026-06-18 \
  tools/vivid.sh build flatpak
```

构建缓存位于：

- `producer/.build/flatpak-builder-state`
- `producer/.build/flatpak-builder-state/ccache`
- `producer/.build/flatpak-native-cache/native-build`
- `producer/.build/flatpak-repo/vivid-producer`

### 直接运行

```sh
tools/vivid.sh build direct-run
tools/vivid.sh direct-run run
```

Direct-run 产物位于 `producer/.build/direct-run`。

### 桌面 consumer

GNOME Shell 和 KDE Plasma 使用各自的插件：

```sh
tools/vivid.sh gnome build
tools/vivid.sh kde build
```

Sway、Hyprland、labwc、river、wayfire、niri 等支持 `zwlr_layer_shell_v1` 的
Wayland 合成器使用通用 layer-shell consumer：

```sh
tools/vivid.sh layer-shell build
tools/vivid.sh layer-shell run
```

Hyprland Lua 自启动和 `hl.layer_rule`、Sway `exec`、niri 自启动的示例位于
`consumer/layer-shell/README.md` 和 `consumer/layer-shell/examples/`。GNOME 仍需安装
Shell 扩展，因为 Mutter 不支持 layer-shell。

### 清理

```sh
tools/vivid.sh clean flatpak
tools/vivid.sh clean direct-run
```

致谢：

1. [waywallen](https://github.com/waywallen)
