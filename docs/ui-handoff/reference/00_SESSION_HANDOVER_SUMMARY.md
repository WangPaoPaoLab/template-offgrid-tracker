# 🛰️ EdgeZ Offgrid Tracker 全案交接与无损重启索引手册 (MASTER HANDOVER GUIDE)

> **2026-11-16 最新接续摘要，优先于下方全部历史归档。**
> **交接称呼：统一使用 jason hao；最新需求R41记录命名、fork与上传范围纠正。**
> 先读 `00_USER_REQUIREMENTS.md` R01–R41，再读 `00_CURRENT_DELIVERY.md` 与网页契约最新节。需求记录是已确认要求的整理，不是完整对话逐字导出。
>
> 最新首页为“设备”；用“全部 / 牧场 / 花园 / 仓库”替代“我的产品”和计数；仅“全部”有项圈、聊天、水阀、摄像头四张卡片，其他区域为空展示。账号/重置已统一返回导航。根入口仅设备/设置；设置严格六项；Chat仍从产品卡片进入。灰线＋橙色、透明Logo、联合品牌及36px细框加号保持。
>
> 网页当前12语言各463条；模型27,152项检查/2,107模板；9地区188驱动记录。首页区域切换与返回已于11月14日浏览器检查，11月16日重新运行模型与翻译检查。桌面手机框截图不代表真机或320px窄屏验收。
>
> 已授权的当前任务是同步交接资料并发布到原fork分支 `ui/prototype-handoff`，更新 [上游PR #1](https://github.com/edgez-ai/template-offgrid-tracker/pull/1)；远端是否已更新以PR head和发布记录为准。只提交文档、网页与素材，不提交 `01_Software_Source` 的本机App/固件调试改动，不自动合并。
>
> 真实地图下载/定位与围栏闭环、图片传输、独立已读、密码服务、设备确认、产品识别协议仍需App/SDK/固件对接。原生有BLE及H7608 SoftAP，网页仅三条BLE fixture，SoftAP分支未完成。不得把网页演示视为这些能力已交付。
>
> 工程交接包：`04_APP_ENGINEER_HANDOFF` 与 `KEEPTEEN_App_UI_Handoff_2026-11-16.zip`；旧11月14日ZIP为历史快照。不要恢复旧摘要中的耳标、全局聊天、全局身份、重复帮助或四柱业务图标。


> **2026-11-14 最新接续摘要，优先于下方历史归档。**
> 先读 `00_USER_REQUIREMENTS.md`（当前 R01–R38）→ `00_CURRENT_DELIVERY.md` → `web_prototype/ADAPTER_CONTRACT.md` 的最新更新节。不要凭压缩摘要或历史设计稿重新推断用户意图。
>
> 已获批准并落地：账号与重置均为带返回的正常二级页面（R38）；双根入口“设备 / 设置”；设置严格六项“账号、HaLow、通知、通用、帮助、关于”；HaLow 先保存在手机、再选择兼容设备下发；聊天昵称归聊天产品；地图存储归通用；聊天从产品卡片进入，不增加全局聊天 Tab；底部入口居中收拢；首页去大黑圆、36px圆角细框＋22px加号/44px点击区，禁止裸加号；无四柱业务图标；灰色细线＋橙色，保留冷白/纯黑风格。
>
> 品牌最新纠正：关于采用 KEEPTEEN × EdgeZ.ai，平等联合出品，**不写软件/硬件分工、不用 Powered by、不放重复帮助**；Logo 下分别放公司简介和官网。简介是依据官网改写的展示稿，非官方中文原文。来源和用户原话见 R32–R35。
>
> 原生实际添加路径有 BLE 与 H7608 SoftAP；低功耗项圈节点通过 HaLow 网关。网页目前只模拟三条 BLE 记录，未展示 SoftAP 流程，不得宣称全产品均为蓝牙或该分支已完成。
>
> 本次只修改本地网页与工程交接包。未修改 App / SDK / 固件、未修复真实地图下载、未推 GitHub PR。12语言各457条，9地区188驱动记录。网页回归25,842项/2,056模板通过；它们不证明真机、推送、无线传输或地图下载已完成。
>
> `04_APP_ENGINEER_HANDOFF/` 与同级明确日期 ZIP 是工程交付；`backups/` 是历史快照。执行新修改仍遵守方案先行，已有明确授权不重复询问。本文件以下为早期历史，不是完整对话逐字备份。


> 2026-11-14 接续入口：[用户需求基线](00_USER_REQUIREMENTS.md) 汇总当前有效用户要求并关联原始记录。根导航仅“设备 / 设置”，聊天在独立产品卡片内。下文保留早期交接历史；较早的产品名称、功能完成情况与射频描述，需分别对照最新需求、`web_prototype/UI_HANDOFF.md` 和 `backend_alignment/BACKEND_CAPABILITIES.md`，不能直接当作当前事实或执行授权。

> **文档路径**：`02_KEEPTEEN_UI_Workspace/00_SESSION_HANDOVER_SUMMARY.md`  
> **更新时间**：2026年11月09日  
> **核心使命**：为新窗口/新会话的 AI Agent 与开发人员提供 100% 完整上下文，实现**无缝、无损、即插即用式的工作接续**。

---

## 一、 项目背景与物理硬件真相 (Physical Reality)

1. **产品定位**：
   - 彻底脱离公网、无蜂窝基站、无云端强耦合的 **Sub-1GHz Wi-Fi HaLow 离网追踪与通信中枢**（米家/Apple HomeKit 消费级体验 + 军工级离线应急通信）。
2. **物理射频频段铁律**：
   - **底层硬件仅支持 868 MHz (EU/GB) 与 915 MHz (US/CA/AU/NZ 等)**，严禁引入 470 MHz 或不存在的中国无线频段。
3. **主控芯片与模组拓扑**：
   - **主力手持机 / 伴侣 (KT-002)**：Nordic nRF54L15 + Quectel FGH200M (Morse Micro MM8108)，走 SPI 总线驱动，需通过 CMD0 锁定 SPI 模式；
   - **便携网关 (Heltec HT-HC33)**：ESP32-S3 + Morse Micro MM6108，具备原生 BLE 5.0（NimBLE）与 2.4GHz Wi-Fi（SoftAP 模式）；
   - **极低功耗项圈 (KT-001)**：nRF54L15 / ESP32-S3，运行 **15 秒周期唤醒 + 突发无连接 Action Frame (EZPR) + 芯片级深度休眠 (Chip Sleep)**，实现数月超长续航；
   - **自组网路由协议**：空中运行 802.11s SAE/AMPE 对等链路 + **BATMAN-IV (EtherType 0x4305)** 动态无中心多跳路由。

---

## 二、 手机 APP 与网关的连接机制及架构分工

1. **硬件与 App 的分工解耦铁律**：
   - **硬件是黑盒调制解调器（Radio Modem）**：负责所有的射频跳频、SPI 总线、802.11s 握手与 BATMAN 路由；
   - **App 是指令与状态可视化中枢（UI & State Hub）**：负责通过唯一接口接收事件流、持久化至本地 SQLite、渲染地图/聊天界面，不参与任何底层网络运算。
2. **双重连接模式**：
   - **模式 A（标配随身伴侣模式 · BLE 5.0）**：手机与伴侣硬件通过低功耗蓝牙 GATT（Service `0xFFF0`，写 `0xFFF1` / 推 `0xFFF2`）1 对 1 独占绑定。低功耗、支持后台长效保活，是消费级默认主流程；
   - **模式 B（营地基地台/车载网关模式 · 2.4GHz Wi-Fi SoftAP）**：HC33 网关释放 `EZ-<MAC>` 热点，通过 DHCP 分配 `10.80.x.x` 子网，允许多达 10 部手机免配对通过局域网 Socket 接入并共享远距离 HaLow 网络。

---

## 三、 当前已完全交付的重构成果 (Current Status)

### 1. 交互原型文件 (`02_KEEPTEEN_UI_Workspace/web_prototype/index.html`)
- **零报错自检**：已通过 Node.js VM 自动化语法与运行时函数校验，执行 **0 报错**；
- **视觉风格**：严格遵循纯色对比分层（纯黑 OLED `#000000` / 卡片层 `#1C1C1E` / 细分层 `#2C2C2E` / 边框 `#38383A`），44pt+ 触控热区，无高斯模糊掉帧；
- **Emoji 审查**：全量清理，真实 SMP Emojis 严格为 **0**。

### 2. 核心信息架构落地 (Plan A)
- **去社交化双 Tab**：根导航彻底精简为 **`[设备 (Devices)]`** 与 **`[我的 (Profile)]`**；
- **消息收敛于 KT-002 设备卡片**：彻底隐藏并移除了全局独立的“消息”根 Tab，离线通信、公共信道与 PTT 对讲收归于【KT-002 离线对讲手持机】专属卡片详情内；
- **iOS 推栈规范 (`hidesBottomBarWhenPushed`)**：进入设备详情页（项圈 KT-001 或手持机 KT-002）时，底部全局 Tab 栏自动平滑隐藏，顶部增设 `‹ 我的设备` 返回按钮。

### 3. 全面去品牌化 · 回归用户中心
- 彻底清除所有贴脸的 `KEEPTEEN` 厂商冠名；首页大标题为 **`我的设备`**；
- 彻底删除宣教式底栏 `为持续拓展的 KEEPTEEN 硬件生态而设计`；
- 个人中心身份显示为 **`本地节点 (主控终端)`**，角色为 **`离线领队`**；
- 默认网络参数中性化（`OFFGRID-MESH-01`，`mesh@secret2026`）。

---

## 四、 核心文档与文件索引

* 📄 **交互原型**：`02_KEEPTEEN_UI_Workspace/web_prototype/index.html`（双击或运行 `open_prototype.bat` 预览）
* 📄 **软硬件全链路架构规范**：`02_KEEPTEEN_UI_Workspace/02_ARCHITECTURE_AND_DATA_ALIGNMENT.md`
* 📄 **业务逻辑功能手册**：`02_KEEPTEEN_UI_Workspace/01_FUNCTIONAL_SPEC.md`
* 📄 **Wi-Fi HaLow 深度调研与设计启示**：`WIFI_HALOW_DEEP_DIVE_AND_APP_DESIGN.md`（Artifacts 目录）
* 📄 **生产源码（React Native App）**：`01_Software_Source/app/`
* 📄 **固件源码与 SPI 诊断交接**：`01_Software_Source/firmware/` & `01_Software_Source/HALOW_SPI_DIAGNOSTIC_HANDOVER.md`
* 📄 **GitHub 仓库**：
  - 个人 Fork 仓（定制 UI 分支 `variant/custom-ui`）：`https://github.com/WangPaoPaoLab/template-offgrid-tracker`
  - 官方上游主仓：`https://github.com/edgez-ai/template-offgrid-tracker`

---

## 五、 下一阶段任务目标 (Next Turn Mission)

1. **反哺与移植至 React Native 生产源码**：
   - 将在 `web_prototype/index.html` 中验证成熟的纯净双 Tab（设备 Hub + 个人中心）、KT-002 详情内嵌对讲、iOS 纯黑分层设计系统，正式落地到 `01_Software_Source/app/`（`App.tsx`、`HomeMap.tsx`、`DeviceComponents.tsx` 等）。
2. **合规国家频段与参数对齐**：
   - 在生产 App 代码中严格限制频段选择为 868MHz 与 915MHz，确保与底层硬件 100% 对齐。
