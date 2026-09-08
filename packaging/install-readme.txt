libjellyfin_plugin 安装说明
============================

安装（两步）
1. 把整个 libjellyfin 文件夹复制到 VLC 的 plugins 目录：
   Windows: C:\Program Files\VideoLAN\VLC\plugins\
   Linux:   /usr/lib/x86_64-linux-gnu/vlc/plugins/
   （文件夹名字 libjellyfin 可以不改，VLC 会递归扫描所有子目录）

2. 刷新插件缓存：
   Windows: 在 VLC 目录运行  vlc-cache-gen.exe plugins
   Linux:   sudo vlc-cache-gen /usr/lib/x86_64-linux-gnu/vlc/plugins

使用
首次（密码只需一次，token 会自动缓存）：
  vlc --extraintf jellyfin --jellyfin-server http://服务器:8096 \
      --jellyfin-username 用户名 --jellyfin-password "密码" 视频文件

之后：
  vlc --extraintf jellyfin --jellyfin-server http://服务器:8096 视频文件

也可以在 VLC 偏好设置（显示全部）→ 界面 → 控制界面 里持久启用。

验证
  vlc -I dummy --list  然后在生成的 vlc-help.txt 里搜索 jellyfin；
  或查看 Jellyfin 控制台活动日志出现 jellyfin4vlc 客户端。

要求: VLC 3.0.x，位数与本包一致（x64 / x86）。
