# Egga 单词机固件（开源版）

面向 **ESP32-S3 / 4.2 英寸、400 × 300 墨水屏**的 ESP-IDF 固件，基于本地 3.2.6 代码整理。包含本地词卡复习、学习进度保存、Wi-Fi 配网、墨墨学习服务接入、OTA、AI 背词教师、音频与电源管理等代码。

本仓库为独立的干净源码快照，不包含原私有仓库历史、账号信息、Wi-Fi 密码、设备标识、服务器配置或固件二进制。它不是已经过实机验收的新版产品固件。

## 开源版与产品固件的区别

- 内置三张原创示例词卡。原考研词库及录音没有明确的再分发许可，因此未公开；可导入自己有权使用的词库。
- Windows Calibri 和来源未明确的 Zfull 字形已替换为开放许可字体生成的子集。为兼容既有代码，保留 `CalibriLight_Eink_36`、`Zfull_GB_*` 符号名；实际素材分别是 Montserrat 和思源黑体。排版可能与产品固件不同。
- 墨墨、AI、OTA、在线词库等联网功能需要对应账号、网络和服务端；开源代码不提供商业服务权限，服务器代码不在本仓库中。
- 原服务器发布工具、本地工具状态、日志、构建产物和私人素材均不包含在本仓库中。

## 编译

需要 ESP-IDF **5.4 或更新版本**（建议 5.5.4）、Python 3，以及 ESP32-S3 工具链。字库生成结果已提交，正常编译无需安装 Node.js。

在已经加载 ESP-IDF 环境的终端中运行：

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p YOUR_PORT flash monitor
```

在 menuconfig 的 `Egga Word Machine` 配置自己的网络和服务地址；默认 Wi-Fi 名称、密码均为空。源码保留原有公共服务地址用于协议兼容，不保证该服务向自行编译的设备开放。自建服务应替换地址。

`build.sh` 是 Bash 打包脚本，可在 Linux、macOS、Git Bash 或 WSL 中使用：

```bash
cp .env.example .env
./build.sh --ota-url https://your.example/ota/
```

`.env` 仅供本地使用。若在编译时写入密码，生成的固件也可能含有该密码，不要公开此类二进制或设备备份。

## 使用和目录

- [使用说明](docs/USER_GUIDE.zh-CN.md)：按键、配网、学习、词库导入。
- [AI 配置](docs/AI_WORD_TEACHER_SETUP.md)：服务接入和提示词。
- [词库格式](docs/COMPACT_DECK_FORMAT.zh-CN.md)：导入自己的词库。
- `main/`：固件应用、显示、音频、学习调度及硬件驱动。
- `main/boards/egga-s3-epaper-4.2/`：引脚、电源、RTC、墨水屏及工厂测试。
- `main/vocabulary/demo_deck.inc`：可自由使用的示例词卡。
- `tools/`、`scripts/`：词库转换、字库生成和本地打包工具。
- `third_party/fonts/`：开放字体源文件、校验和与许可证。

硬件采用 ES8311 音频、PCF8563 RTC、GT23SC6699 NFC，16 MB Flash 分区见 `partitions/v2/16m_ota.csv`。刷机前核对硬件及分区兼容性；擦除 Flash 会清除学习进度和设备配置。

## 重新生成字库

安装 Node.js 后运行 `pnpm install --frozen-lockfile` 和 `python tools/regenerate_open_fonts.py`。转换器固定为 `lv_font_conv@1.5.3`，输入字体使用 SHA-256 校验。仅修改文字内容无需重复生成全部字体。

## 许可与贡献

项目代码沿用 [MIT License](LICENSE)，保留原版权声明。第三方依赖和字体遵循各自许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。MIT 不授予第三方词库、录音、商标或云服务的使用权。

提交贡献前阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。安全问题请按 [SECURITY.md](SECURITY.md) 私下报告，不要在公开 Issue 粘贴密钥或账号数据。
