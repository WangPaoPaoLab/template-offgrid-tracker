# 本次交接包验证记录

日期：2026-11-16，首页区域、六项设置、联合品牌与工程交接版本。以下是文件、网页模型与浏览器核验，不是原生App、真实设备或生产云端验收。

## 素材

- 4类产品图、23个功能图标、2个品牌组合资源；729条清单逐项验证存在性与SHA-256。
- 58份SVG通过XML解析；无外部CSS变量或字体依赖。KEEPTEEN品牌SVG仍是原PNG封装；EdgeZ.ai品牌SVG为官网节点图形与Inter 750字标的路径复现，来源及字体OFL随包提供，不冒充对方交付的官方VI母版。
- 639份PNG经Pillow独立解码，均含透明像素和可见内容。KEEPTEEN原始PNG保留；深浅色Logo与产品图可用。
- 29个iOS Image Set，Any/Dark各@1x/@2x/@3x；10个Android密度/主题目录各29资源。
- 业务图标旧ui_signal已从当前导出中移除，扫描使用ui_bluetooth；HaLow使用ui_halow。不要将模拟扫描列表推成全产品仅BLE。

## 网页

- 11月16日模型回归27,152项检查、2,107个模板通过；12语言各463条，共5,556条翻译，生成检查通过；9地区188条驱动记录完整保留。
- 首页标题为设备；全部、牧场、花园、仓库四个区域，仅全部含四张卡片。11月14日浏览器检查区域切换、空状态、键盘导航、聊天进入返回后恢复全部；截图home-areas-all.jpg和home-areas-empty.jpg。其余浏览器检查也来自11月14日，不冒充本次重新操作。
- 设置一级严格六项，双根入口“设备/设置”；聊天仍在独立产品卡片内。
- 浏览器核验六项菜单、未连接保存手机HaLow配置、合法信道联动、设备选择、通知开关入口、通用页、账号标准返回导航及深浅色联合品牌关于页；页面无脚本错误。关于页两个Logo均加载，官网链接各自正确，无帮助重复、无硬件/软件分工标注。
- 首页加号实际22px、36px圆角细框、44×44点击区；底部两入口108×48，中心距136 CSS px（桌面手机框）。设备搜索列表确认无旧四柱图形。截图见specs/screenshots/。
- 本轮viewport覆盖没有改变实际浏览器窗口，因此不声称完成320px真实窄屏浏览器验收；截图为桌面内手机框。多语言模板有模型覆盖，最终原生窄屏仍需工程验收。

## 明确未完成

- 真实BLE/SoftAP与产品描述握手、硬件下发确认、系统推送、图片传输/已读/密码服务、真实地图下载和可靠围栏执行依然需按能力表接入与真机验收。网页适配器为demo。
- 原生源码已有nRF54/ESP32 BLE与H7608 SoftAP扫描，网页目前仅三条BLE fixture；SoftAP连接分支未展示。低功耗项圈sensor不提供BLE，节点走HaLow网关。
- 简介是依据官网改写的展示稿，并非两家官方中文原文。用户最新纠正与来源见reference/00_USER_REQUIREMENTS.md R01–R41。

## 复核入口

```text
node tools/verify-assets.cjs
node reference/web_prototype/verify-board.cjs
python reference/web_prototype/locales/compile-locales.py --check
```

ZIP排除node_modules、Git元数据、备份及缓存；附SHA-256。本次GitHub交接目标为原fork ui/prototype-handoff分支和上游PR #1，实际远端状态以PR head为准。未修改原生App/SDK/固件，不部署或合并。
