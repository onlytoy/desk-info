# desk-info — 横屏信息台

智伴教育机器人 1X（GS705B）的**独立桌面信息台**程序，直接绘制 `/dev/fb0` 帧缓冲，
不依赖原厂后台、不依赖 zhiban 项目、不依赖任何第三方 API Key。

## 目标设备

| 项 | 规格 |
|----|------|
| SoC | ACTIONS OWL（ARM Cortex-A5） |
| 系统 | Linux 3.10.52 + uClibc + BusyBox |
| 屏幕 | `/dev/fb0`，480×1600 虚拟（2 帧 480×800），16bpp RGB565，物理横屏 800×480 |
| 可写区 | `/var/upgrade`（vfat，9.8MB） |

## 编译

需要 ARM soft-float uClibc 交叉工具链（Linux 环境 / WSL / CI）：

```bash
# 工具链放本仓库 toolchain/ 目录
bash build.sh          # 产物 build/desk-info
```

## 部署

1. 把 `desk-info` 推到设备 `/var/upgrade/`（xwebd upload 或 telnet）。
2. 停掉原厂 launcher（可选，避免抢屏）：
   `killall launcher`
3. 运行：
   `/var/upgrade/desk-info`

## 路线图

- [x] V1：直接绘制帧缓冲的大时钟 + 日期（验证通路）
- [ ] V2：天气（Open-Meteo）+ 左右分栏信息布局
- [ ] V3：日历/倒数日/行情滚动/新闻等

## 与 zhiban 的区别

zhiban 是替换原厂 `sair` 语音助手（依赖原厂后台激活/OTA/ASR/LLM）；
本项目是**完全独立**的程序，直接占用帧缓冲画图，只依赖免费公开的 Open-Meteo 天气数据。
