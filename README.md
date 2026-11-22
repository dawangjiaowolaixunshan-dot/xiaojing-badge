# 小鲸电子吧唧（XiaoJing Voice Badge）

> **English Overview**: An open-source, standalone ESP32-S3 Chinese voice companion badge built for kids on a 1.75" 466×466 round AMOLED touchscreen (Waveshare ESP32-S3-Touch-AMOLED-1.75C). It connects directly to DeepSeek and Alibaba Cloud Bailian (ASR + 24 kHz TTS) with no proxy or companion phone app required—featuring walkie-talkie style voice chat, synchronized karaoke-style text reveal, classical poetry reading & recitation scoring, mental math, vocabulary practice, edge-swipe back navigation, and plug-in Wi-Fi keep-alive.

一个运行在 **微雪 ESP32-S3-Touch-AMOLED-1.75C（466×466 圆屏触摸屏）** 上的独立随身中文语音小伙伴固件。无需手机 App、无需中转代理，开机直连国内语音与大模型接口，专为孩子日常好奇心问答、古诗背诵和趣味学习打造。

---

## 核心功能亮点

- **对讲机式语音聊天 & 声画同步**
  - 按住顶部电源键（PWR）即可说话，松手实时流式转文字（阿里云百炼实时语音识别）→ DeepSeek 流式思考回答 → 24 kHz 自然女声朗读。
  - 聊天气泡支持**随朗读进度逐字渐亮浮现**；点击小鲸头像一键打断闭嘴，短按历史回答气泡可重听。
  - 首页任意状态下直接按住电源键即可像对讲机一样直达聊天页开始录音。
  - 支持多款清亮自然的纯中文女声音色（聊天与古诗朗读独立选择），并支持从外部导入自定义提示词（如“温柔大姐姐”儿童版，内置作业启发引导与儿童安全边界）。

- **古诗赏读 & 背诗打分**
  - 古诗页采用**苹果风弹簧丝滑滚屏**，点任意诗句自动居中并朗读；支持按句/全文朗读。
  - 背诗模式支持实时语音比对与打分反馈，结合小鲸动画宠物互动激励。

- **趣味口算 & 背单词**
  - 内置小学口算练习与常用英文单词跟读/记忆模式，碎片时间随手练。

- **为圆屏打磨的顺手交互**
  - **全局跟手右滑返回**：所有子页面统一支持安卓风右滑返回（左圆弧弹出阻尼箭头，拉够距离高亮松手即返，防误触滑块与键盘）。
  - **全场景快捷音量 & 芯片真静音**：聊天、古诗、背诗、单词页右上角常驻小喇叭按钮；同时支持在首页短按 / 子页长按下侧键弹出半透明音量浮层；音量调到 `0%` 自动叠加音频芯片硬件静音，彻底消除底噪。
  - **纯圆屏触屏连无线网**：支持板载扫描附近 2.4 GHz Wi-Fi 列表并在 466×466 圆屏上直接触屏输入密码，也支持双网络（家里 Wi-Fi + 手机热点）自动回退。
  - **插电不断网 & 电池智能省电**：插上 USB 电源时屏幕照常到点熄灭（防止 AMOLED 烧屏），但无线网保持常连、点亮秒聊；拔线改用电池后熄屏 2 分钟自动关闭无线电省电。
  - **弱网自适应防卡顿**：根据 Wi-Fi 信号强度动态调整音频预缓冲与发射功率，降低弱网环境下的断音和卡顿。

---

## 硬件规格与按键操作

### 目标硬件
- **开发板**：Waveshare ESP32-S3-Touch-AMOLED-1.75C
- **主控与存储**：ESP32-S3（240 MHz 双核 + 8 MB Octal PSRAM + 32 MB Flash）
- **屏幕与触控**：1.75 英寸 466×466 圆形 AMOLED 触摸屏（LVGL 9）
- **音频链路**：ES8311 音频编解码芯片 + 48 kHz I2S 播放 + 麦克风收音
- **电源管理**：AXP2101 电源芯片（电池电量读取、充电/USB 插拔状态检测、软件关机）

### 实体按键与手势
| 操作 | 效果 |
| :--- | :--- |
| **按住顶部电源键（PWR）** | 首页直接进入聊天并开始说话；聊天/背诗/背单词页内按住说话、松手识别 |
| **长按电源键约 10 秒** | 硬件强制关机（应急复位用） |
| **短按下侧键（BOOT）** | 首页呼出快捷音量浮层 |
| **长按下侧键（BOOT）** | 任意子页面呼出快捷音量浮层 |
| **从屏幕左侧向右滑** | 全局返回上一页（设置页与输密码页为防误触，需从最左弧边起滑） |

---

## 目录与分区说明

```text
badge-app/
├── main/
│   ├── app_main.c          # 系统初始化与任务编排
│   ├── ui.c / ui.h         # 屏幕驱动、全局右滑返回、音量浮层、息屏与插电不断网
│   ├── ui_home.c           # 首页（时钟、天气、电量、功能入口）
│   ├── ui_chat.c           # 语音聊天页（声画同步逐字浮现）
│   ├── ui_poem.c           # 古诗赏读页（弹簧滚动与点句朗读）
│   ├── ui_recite.c         # 背诗评分页
│   ├── ui_math.c           # 趣味口算页
│   ├── ui_words.c          # 背单词页
│   ├── ui_settings.c       # 设置页（亮度/音量、音色试听、Wi-Fi 扫描与触屏输密、声纹）
│   ├── asr.c / asr.h       # 阿里云实时语音识别（WebSocket 流式分片上传）
│   ├── chat.c / chat.h     # DeepSeek 流式对话与外部自定义提示词加载
│   ├── tts.c / tts.h       # 24 kHz 实时语音合成、弱网自适应缓冲与硬件静音
│   ├── net.c / net.h       # Wi-Fi 连接、热点切换、NTP 对时与无线电休眠
│   ├── prefs.c / prefs.h   # 用户偏好持久化（亮度、音量、休眠时长、音色索引）
│   ├── pmu.c / pmu.h       # AXP2101 电源管理与 USB 检测
│   └── console.c           # USB 串口配置与调试命令
├── partitions.csv          # 分区表（factory 12M / storage 1M / pet 16M）
└── sdkconfig.defaults      # ESP-IDF 默认编译配置
```

> **刷机保数据提示**：用户的 Wi-Fi 凭据、API 密钥、偏好设置保存在 `nvs` 分区，聊天记录和自定义提示词保存在 `storage`（SPIFFS）分区，小鲸动画素材保存在 `pet` 分区。日常更新固件只需执行 `idf.py app-flash` 仅刷写应用分区，不会丢失任何个人数据。

---

## 编译、烧录与首次配置

### 1. 环境准备
- 安装 [ESP-IDF v6.0+](https://docs.espressif.com/projects/esp-idf/)
- 准备自己的 API 密钥（**切勿硬编码进源码**）：
  - **DeepSeek API Key**（对话模型）
  - **阿里云百炼 API Key**（实时语音识别与语音合成）

### 2. 编译与烧录
```bash
# 加载 ESP-IDF 环境
source ~/esp/esp-idf-v6/export.sh

# 首次完整烧录（含分区表）
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash

# 日常更新仅刷应用分区（保留 Wi-Fi、密钥、聊天记录与提示词）
idf.py -p /dev/cu.usbmodemXXXX app-flash
```

### 3. 写入个人配置（通过 USB 串口）
设备开机连上电脑后，可通过串口命令配置密钥、Wi-Fi 与自定义提示词（全部保存在设备本地非易失存储中，不会出现在代码仓库里）：
- `>ds_key=<你的 DeepSeek 密钥>`
- `>bl_key=<你的阿里云百炼密钥>`
- `>wifi=<Wi-Fi名称><TAB><密码>`（或直接在设备圆屏「设置 → 无线网」里扫描并触屏输入）
- 自定义系统提示词：支持分片写入 UTF-8 文本（最大 16 KB），不配置时自动使用内置的“温柔大姐姐”默认提示词。

---

## 隐私与安全说明

- 本仓库**不包含**任何 API 密钥、Wi-Fi 密码、代理配置或个人对话记录。
- 对话请求启用了较高的内容安全检查等级，适合儿童日常使用。

---

## 致谢与共同开发

- **项目主导与硬件实测**：[@dlandwt-dotcom](https://github.com/dlandwt-dotcom)
- **AI 协作开发**：Google Antigravity & Claude Code
- 界面基于 [LVGL 9](https://lvgl.io/)，底层基于 [ESP-IDF](https://github.com/espressif/esp-idf)。
- 欢迎参考、二次开发与交流反馈！
