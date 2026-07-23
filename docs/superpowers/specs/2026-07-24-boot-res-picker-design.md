# 开机分辨率选择（OSD）设计

日期: 2026-07-24

## 需求
- 每次开机（FBUI 启用时）用硬件 OSD 选择 240p / 1080p
- 默认高亮 240p；约 8 秒无操作自动选 240p
- 选定后菜单期 HDMI+VGA 统一为该分辨率（临时 vga_scaler=1）
- 进游戏 / 退出 FBUI 后恢复 MiSTer.ini 视频设置
- 同一次开机内 F3 再进 FBUI 不再弹选择

## 实现要点
- `video_menu_res_apply` / `video_menu_res_restore`（video.cpp）
- FBUI 启动前 OSD 选择页（fbui.cpp）
- 240p: NTSC 15kHz modeline；1080p: 启动时保存的当前模式
