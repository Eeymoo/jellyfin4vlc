# jellyfin4vlc

A VLC 3.0.x 原生接口插件（C 语言），把 VLC 变成 Jellyfin 的一个"客户端"：

1. **登录** — 配置 Jellyfin 服务器地址 + 用户名密码，自动完成
   `POST /Users/AuthenticateByName`，并把 token/user id 缓存到 VLC 配置中
   （之后不再需要密码）。
2. **浏览媒体库** — 可选地把服务器上所有电影/剧集抓取下来，作为直连流
   （`/Videos/{id}/stream?static=true`）追加进 VLC 播放列表，直接在 VLC 里
   观看 Jellyfin 视频。
3. **播放状态同步** — 后台线程监视当前输入：
   - 切换到 Jellyfin 条目 → `POST /Sessions/Playing`
   - 每隔 N 秒 → `POST /Sessions/Playing/Progress`（带 `IsPaused`、
     `PositionTicks`，1 秒 = 10 000 000 ticks）
   - 停止/换台 → `POST /Sessions/Playing/Stopped`

   因此 Jellyfin Web 界面的"继续观看"和已看进度会实时更新。

两种方式识别"正在播放的是 Jellyfin 条目"：

- 播放列表里的直连流 URL（浏览功能加入的）→ 直接从 URL 提取 ItemId；
- 本地播放的 `file://` 路径 → 与启动时抓取的媒体库 `Path` 字段做
  全路径（大小写不敏感）→ 文件名 两级匹配。

架构参考了 VLC 自带的 `modules/misc/audioscrobbler.c`（同为
Control Interface 模块 + 后台线程 + libcurl）。

## 文件结构

| 文件 | 作用 |
|---|---|
| `src/jellyfin.c` | VLC 模块本体：模块注册、配置项、后台线程、播放状态机、浏览 |
| `src/jf_api.c/.h` | Jellyfin REST 客户端：登录、分页抓取媒体库、路径匹配、播放上报 |
| `src/jf_http.c/.h` | libcurl 极简封装（GET/POST + 响应缓冲） |
| `src/cJSON.c/.h` | 第三方单文件 JSON 库（MIT，来自 DaveGamble/cJSON） |

## 编译

依赖（以 Debian/Ubuntu 为例）：

```sh
sudo apt install build-essential pkg-config vlc-plugin-dev libcurl4-openssl-dev
```

```sh
make          # 产出 libjellyfin_plugin.so
sudo make install   # 安装到 VLC 插件目录并刷新缓存
```

macOS（配合 Homebrew 的 vlc）：`brew install vlc curl pkgconf`，然后 `make`，
把 `libjellyfin_plugin.so`（可改名 `libjellyfin_plugin.dylib`）放到
`$(brew --prefix)/lib/vlc/plugins/interface/` 下并运行
`…/lib/vlc/vlc-cache-gen …/lib/vlc/plugins`。

> 插件 API 针对 **VLC 3.0.x**。VLC 4.0 的插件接口（`vlc_plugin.h`、
> `input`/`playlist` API）有破坏性变更，暂不支持。

## 使用

首次运行需要服务器地址和账号：

```sh
vlc --extraintf jellyfin \
    --jellyfin-server http://192.168.1.10:8096 \
    --jellyfin-username alice \
    --jellyfin-password secret
```

登录成功后 token / user id / device id 会写入 VLC 配置
（`~/.config/vlc/vlcrc`），之后只需：

```sh
vlc --extraintf jellyfin --jellyfin-server http://192.168.1.10:8096
```

（token 仍然要求 server 一致；换服务器请同时提供用户名密码。）

### 配置项

| 选项 | 默认 | 说明 |
|---|---|---|
| `--jellyfin-server` | 空 | 服务器基础 URL，支持反代子路径（如 `http://example.com/jellyfin`） |
| `--jellyfin-username` | 空 | 用户名 |
| `--jellyfin-password` | 空 | 密码（仅首次登录需要） |
| `--jellyfin-token` | 自动缓存 | AccessToken（advanced） |
| `--jellyfin-userid` | 自动缓存 | 用户 ID（advanced） |
| `--jellyfin-device-id` | 自动生成 | 上报给服务器的设备标识（advanced） |
| `--jellyfin-browse` | 关 | 启动时把媒体库追加到播放列表 |
| `--jellyfin-interval` | 10 | 进度上报间隔（秒，5–120） |

示例——在 VLC 里直接浏览并同步：

```sh
vlc --extraintf jellyfin --jellyfin-browse \
    --jellyfin-server http://192.168.1.10:8096 \
    --jellyfin-username alice --jellyfin-password secret
```

也可以在 GUI 里勾选 *Tools → Preferences (All) → Interface → Control
interfaces → Jellyfin playback sync* 持久启用（`--extraintf jellyfin` 同义）。

## 验证

1. `vlc -v` 日志里应出现 `jellyfin: logged in as user …` 和
   `jellyfin: loaded N library items`。
2. 播放媒体库中的文件，日志出现
   `jellyfin: reporting playback of <itemId>`。
3. Jellyfin Web 控制台 → Dashboard → Activity / 或
   `GET /Sessions` 应看到 `jellyfin4vlc` 会话和实时进度。

## 已知限制（最小实现范围）

- 浏览功能是"平铺列表"而不是虚拟文件夹结构；字幕/音轨选择走流内封装。
- 不处理转码协商，直连流按 DirectPlay 上报。
- VLC 4.0 未适配。

## License

LGPL-2.1-or-later（与 VLC 模块一致）；`src/cJSON.*` 为 MIT。
