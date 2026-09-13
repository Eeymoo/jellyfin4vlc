libjellyfin_plugin 安装说明
============================

安装（两步）
1. 把整个 libjellyfin 文件夹复制到 VLC 的 plugins 目录：
   Windows: C:\Program Files\VideoLAN\VLC\plugins\
   Linux:   /usr/lib/x86_64-linux-gnu/vlc/plugins/
   （文件夹名字 libjellyfin 可以不改，VLC 会递归扫描所有子目录）

2. 刷新插件缓存 —— 注意：Windows 上请使用绝对路径调用，
   相对路径会静默生成空缓存（不报错但插件不生效）：
   "C:\Program Files\VideoLAN\VLC\vlc-cache-gen.exe" "C:\Program Files\VideoLAN\VLC\plugins"
   Linux:  sudo vlc-cache-gen /usr/lib/x86_64-linux-gnu/vlc/plugins

   如果插件仍未被识别：右键 dll → 属性 → 勾选"解除锁定"，再重跑一次上面的命令。

使用
命令行参数不会持久化；日常使用建议在偏好设置里配置（一次配好，以后开 VLC 就生效）：
  工具 → 偏好设置 → 左下角勾"全部" →
    1. 界面 → 控制界面 → Extra interface modules 填 jellyfin（进度同步）
    2. 播放列表 → 服务发现 → 勾选 Jellyfin media library（侧边栏媒体库）
    3. 界面 → 控制界面 → Jellyfin playback sync 节点里填
       jellyfin-server / jellyfin-username / jellyfin-password
  等价的 vlcrc 配置项：services-discovery=jellyfin、extraintf=jellyfin、
  jellyfin-server=...、jellyfin-username=...（首次登录后 token 自动缓存，无需密码）

命令行方式：
  vlc --extraintf jellyfin --services-discovery jellyfin \
      --jellyfin-server http://服务器:8096 \
      --jellyfin-username 用户名 --jellyfin-password "密码" 视频文件

验证
  vlc -I dummy --list  然后在生成的 vlc-help.txt 里搜索 jellyfin；
  或查看 Jellyfin 控制台活动日志出现 jellyfin4vlc 客户端；
  侧边栏出现 Jellyfin 树（电影 / 剧集按剧分组）即服务发现生效。

要求: VLC 3.0.x，位数与本包一致（x64 / x86）。
