# Windows 编译与使用指南

Windows 版 VLC 的插件是 **`.dll`**，需要放在 VLC 安装目录的
`plugins\interface\` 下。编译推荐用 **MSYS2 + MinGW-w64**（不要用 MSVC，
VLC 本身就是 MinGW 构建的）。

## 一、准备环境

1. 安装 [VLC 3.0.x Windows 版](https://www.videolan.org/vlc/downloads-windows.html)
   （默认路径 `C:\Program Files\VideoLAN\VLC`）。
2. 安装 [MSYS2](https://www.msys2.org/)，打开 **MINGW64** 终端（开始菜单 "MSYS2 MINGW64"）。
   官方 VLC 3.0.x Windows 构建基于 msvcrt，插件必须用 MINGW64（msvcrt）
   工具链；UCRT64 构建的插件在官方 VLC 里跨 CRT 释放内存会崩溃。

```sh
pacman -Syu
pacman -S mingw-w64-x86_64-toolchain make pkgconf \
          mingw-w64-x86_64-curl
```

## 二、获取 VLC 插件头文件（二选一）

VLC 官方 Windows 安装包**不带开发头文件**，两种办法：

### 方案 A：MSYS2 的 vlc 包（省事）

```sh
pacman -S mingw-w64-x86_64-vlc
```

装完后确认 pkg-config 能找到模块：

```sh
pkg-config --modversion vlc-plugin   # 应打印 3.0.x
```

> 注意：MSYS2 仓库的 VLC 版本最好和你安装的桌面版 VLC 大版本一致
> （都是 3.0.x 即可，小版本差异通常没问题）。

### 方案 B：用 VLC 源码头文件（版本完全可控）

下载对应版本源码，例如 3.0.20：

```sh
curl -LO https://get.videolan.org/vlc/3.0.20/vlc-3.0.20.tar.xz
tar xf vlc-3.0.20.tar.xz
```

之后编译时传 `VLC_SRC` 指向源码目录（见下一步）。

## 三、编译

```sh
cd /c/Codes/jellyfin4vlc      # 你的项目目录

# 方案 A（MSYS2 vlc 包）：
make

# 方案 B（源码头文件）：
make VLC_SRC=/c/src/vlc-3.0.20
```

成功后得到 **`libjellyfin_plugin.dll`**。

## 四、安装到 VLC

```sh
# 假设 VLC 装在默认路径
cp libjellyfin_plugin.dll "/c/Program Files/VideoLAN/VLC/plugins/interface/"

# 刷新插件缓存（vlc-cache-gen.exe 在 VLC 主程序旁）
cd "/c/Program Files/VideoLAN/VLC"
./vlc-cache-gen.exe ./plugins
```

> 需要管理员权限：用管理员身份的终端，或在资源管理器里手动复制，
> 再右键管理员运行 `vlc-cache-gen.exe`（参数是 plugins 目录）。

## 五、使用

### 命令行（推荐先用这个验证）

PowerShell 或 cmd：

```bat
"C:\Program Files\VideoLAN\VLC\vlc.exe" ^
  --extraintf jellyfin --jellyfin-browse ^
  --jellyfin-server http://192.168.1.10:8096 ^
  --jellyfin-username alice --jellyfin-password secret
```

加 `-v`（或 `--verbose 2`）看日志，成功时会有：

```
jellyfin: logged in as user xxxxxxxx-...
jellyfin: loaded 1234 library items
```

登录成功后 token / user id / device id 会缓存到
`%APPDATA%\vlc\vlcrc`，以后只需：

```bat
"C:\Program Files\VideoLAN\VLC\vlc.exe" --extraintf jellyfin ^
  --jellyfin-server http://192.168.1.10:8096
```

### GUI 里持久启用（每次打开 VLC 都生效）

1. VLC → 工具 → 偏好设置，左下角选 **全部**（显示全部设置）；
2. 左侧 **界面 → 控制界面**，右侧 "Extra interface modules" 填
   `jellyfin`；
3. 在 **控制界面 → Jellyfin playback sync** 节点里填 server /
   username / password；
4. 保存后重启 VLC。

（等价于命令行 `--extraintf jellyfin` + 各 `--jellyfin-*` 选项。）

## 六、Windows 常见问题

| 现象 | 原因/处理 |
|---|---|
| 日志没有任何 jellyfin 输出，`vlc -vvv --extraintf jellyfin` 也说找不到模块 | 插件没被缓存收录：确认 dll 在 `plugins\interface\`，并重新跑过 `vlc-cache-gen.exe`；架构要一致（64 位 VLC 配 64 位编译） |
| `jellyfin: login failed: network error` | 防火墙拦了 VLC 的出站连接（第一次会弹窗，点允许）；或服务器地址在 Windows 上不可达（浏览器先打开 `http://服务器:8096` 试试） |
| `jellyfin: login failed: HTTP 401` | 用户名/密码错，或换了服务器但用了旧 token：删掉 `vlcrc` 里 `jellyfin-token=`/`jellyfin-userid=` 的值（或直接带上用户名密码启动一次） |
| 播放列表没出现媒体库条目 | 忘了 `--jellyfin-browse`；或库太大抓取慢，看 `-v` 日志里 `loaded N library items` |
| 进度不同步 | 只有"能识别为 Jellyfin 条目"的媒体才同步：要么是从播放列表里播的（浏览功能加的），要么本地路径能和服务器上的 `Path` 对上（路径不同时靠文件名匹配） |
| 换了 VLC 版本后插件失效 | 大版本变了（如升到 4.x）：本插件只支持 3.0.x，需用回 3.0.x 或等适配 |

## 七、卸载

删除 `plugins\interface\libjellyfin_plugin.dll`，重新运行
`vlc-cache-gen.exe ./plugins`，并去掉偏好设置里的 extra interface。
