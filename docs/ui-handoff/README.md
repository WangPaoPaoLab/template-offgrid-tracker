# KEEPTEEN App UI 工程交接包

交接版本：2026-11-16；网页界面版本仍为2026.11.14。用途：将已确认的网页设计和素材交给负责 App 的工程师实现。包含可直接使用的资源、网页参考、用户需求、接口边界和待开发清单，不是原生 App 安装包。

**先确认产品结构：根导航只有“设备 / 设置”；聊天是独立产品，通过首页聊天卡片进入。进入项圈、聊天详情或会话后隐藏根导航。不得额外增加全局聊天 Tab。**

## 1. 从哪里开始

1. 阅读 `reference/00_USER_REQUIREMENTS.md`，确认需求和已否定的方案。
2. 双击 `ASSET_PREVIEW.html` 检查透明 Logo、四种产品图和通用图标的深浅色版本。棋盘底仅用于预览，不在图片中。
3. 双击 `reference/web_prototype/index.html` 查看默认用户界面；需要检查状态与失败流程时，另开 `review.html`。不要把评审控件放进生产 App。
4. 对照 `reference/backend_alignment/BACKEND_CAPABILITIES.md` 和 `reference/web_prototype/ADAPTER_CONTRACT.md` 接接口。
5. 地图与围栏必须单独阅读 `reference/backend_alignment/MAP_GEOFENCE_IMPLEMENTATION_HANDOFF.md`。真实地图下载、可靠越界提醒与网关离线执行尚未完成，不能按“已交付功能”跳过。

## 2. 文件目录

```text
assets/
  brand/original/             官方原始透明 PNG，3651×1382，原文件未改
  brand/svg/light|dark/       KEEPTEEN为PNG封装；EdgeZ.ai为官网外观路径复现
  brand/png/light|dark/       透明 Logo，@1x / @2x / @3x
  products/svg/light|dark/    四个产品的真正独立矢量 SVG
  products/png/light|dark/    四个产品的透明 PNG，@1x / @2x / @3x
  ui/svg/light|dark/          通用操作图标 SVG
  ui/png/light|dark/          通用操作图标 PNG，@1x / @2x / @3x
platforms/
  ios/KEEPTEENAssets.xcassets/ 通用 @1x/@2x/@3x 与 Dark Appearance 资源
  android/drawable-*/         mdpi、hdpi、xhdpi、xxhdpi、xxxhdpi
  android/drawable-night-*/   对应深色资源
reference/                   当前网页、已确认需求与后端资料的固定副本
specs/DESIGN_TOKENS.json      颜色、尺寸、描边、卡片层级与根导航约束
ASSET_MANIFEST.json          每个素材的 ID、主题、尺寸、来源与 SHA-256
tools/                       从当前网页重新导出与检查资源的脚本
```

## 3. 四张产品卡片的素材对应

| 产品 | 资源 ID | SVG 文件名 | 当前网页源码键 | 页面范围 |
| --- | --- | --- | --- | --- |
| 项圈 | product_collar | collar.svg | collar | 网关 → 多项圈 → 地图 / 围栏 / 详情 |
| 聊天 | product_chat | chat.svg | radio | 频道 / 队友 → 文字、图片、语音会话 |
| 水阀 | product_water_valve | water_valve.svg | valve | 仅卡片，详情未开放 |
| 摄像头 | product_camera | camera.svg | camera | 仅卡片，详情未开放 |

不要把源码中的 `radio` 名称显示成对讲机，也不要把 `assets/ui/` 内的功能图标替换成首页产品插画。本包四张产品图从正式网页 `product-art.js` 导出，未重画或替换产品形态。

## 4. 怎么用这些素材

- 优先使用产品 SVG：无外部 CSS 变量、字体、网络图片依赖；主题颜色已经固化，供原生 SVG 渲染器直接引用。路径与当前网页相同，描边按实际页面的 1.45 导出。
- 不使用 SVG 的页面，直接使用对应 PNG。产品 PNG 基准 120×100，@2x 为 240×200，@3x 为 360×300；按 `contain` 显示，不能横向拉伸。网页中的显示框为约 91×74。
- 普通图标基准 24×24，网页常见显示尺寸 22×22；点按区域仍须至少 44×44，不能把小图像本身当全部触控区域。
- 通用图标导出的是中性态配色；主按钮和选中态需按组件的前景色处理，不能把普通灰色当成所有交互状态的固定颜色。
- iOS 可将整个 `KEEPTEENAssets.xcassets` 加入资源工程，按资源 ID 调用；已提供深色 Appearance 图片。工程接入和真机渲染仍需验收。
- Android 将对应 drawable 目录合并到目标模块 `res/`，按 `product_collar` 这类资源名调用，系统根据密度与夜间主题选择图片。
- React Native 可使用产品 SVG 或 PNG；Logo 建议直接用主题对应 PNG，不要求 App 支持网页的 SVG 滤镜。

## 5. 透明、填充与品牌细节

- 所有导出的图像都保留透明画布。产品形体内部的浅灰 / 深灰填充属于图形本身，外部没有白色矩形背景。
- 首页产品背后的柔和椭圆是页面层：67×51、约 0.65 不透明度、颜色为对应 surface。它没有烘焙进图标，App 工程师可在组件层绘制。
- 官方 Logo 原图本来就是透明 PNG，之前关于页白块来自 CSS 底板；正式网页已移除该底板。浅色 Logo 保留原黑字与橙色；深色导出使用浅色字，保留原橙色。
- KEEPTEEN未取得官方矢量母版，其SVG封装原始PNG；EdgeZ.ai为官网外观路径复现并附字体许可。二者均不声称对方正式交付的官方VI母版；产品和功能图标SVG为实际路径。
- 完整 Logo 用在关于等品牌说明位置，首页继续显示“设备”，不恢复整块 Logo 和宣传口号。

## 6. 哪些属于界面设计，哪些需要实现

网页是本地交互规格板，数据、账号验证、设备确认、回执与规则同步可能由演示适配器产生。生产环境不得直接沿用示例验证码、示例密码、计时成功或假设备 ID。

文字与 SDK 语音已有基础；聊天图片、独立已读、密码服务、围栏适配与执行端仍有开发依赖。必须以能力清单逐项对接，不能为了新 UI 编造现有服务接口，也不能因为底层尚缺而遗漏用户要求的目标流程。

原生 App 下载地图排队 / 0% 与默认斯德哥尔摩问题未在本次网页工作中修复。地图与围栏交接文件已经列出具体工作和验收条件。

## 7. 重新导出与验证

仅使用素材或查看网页不需要安装任何依赖。需要重新生成时：

```text
cd tools
npm ci --no-fund --no-audit
npm run export
npm run verify
```

导出器默认读取本包 `reference/web_prototype`；项目维护者可用 `KEEPTEEN_WEB_SOURCE` 指向最新源目录。不要直接编辑若干 PNG 后丢失 SVG 来源。资源清单和验证记录见 `ASSET_MANIFEST.json`、`VALIDATION.md`。

本包作为GitHub工程交接资料，随既有fork分支与[PR #1](https://github.com/edgez-ai/template-offgrid-tracker/pull/1)更新；不部署生产服务，不自动合并PR。实际发布状态以PR head为准。

## 本次设置更新

一级为账号、HaLow、通知、通用、帮助、关于。HaLow 先保存在手机，再选设备应用；产品能力描述仍需新增真实协议。详见 `reference/web_prototype/ADAPTER_CONTRACT.md` 的最新更新节。新增 `ui_halow` 自绘功能图标，非官方认证标志。

## 联合品牌与新增资源

“关于”为 KEEPTEEN × EdgeZ.ai，平等展示，不标硬件/软件，不写 Powered by，不重复放帮助。官网简介是编辑改写的展示稿，详细来源见需求 R35。EdgeZ.ai 资源 `edgez_logo` 为官网节点图形与 Inter 750 字标的中性路径SVG复现；不等于官方提供的VI母版。来源元数据与 Inter OFL 位于 `assets/brand/source/`。品牌SVG中 KEEPTEEN 是嵌入原PNG，EdgeZ.ai 是实际路径，二者不要混称。

共23个功能图标，新增 HaLow 专属图形；扫描用 `ui_bluetooth`，旧 `ui_signal` 已废弃并移除。真实设备还有 SoftAP 添加路径，网页未展示该分支；不要把蓝牙模拟列表当成全设备实现。当前12语言各463条。

账号与密码重置现为普通二级页面，和通知页共用返回头，不使用关闭叉；对应用户需求R38。

## 首页区域与交付入口

首页“设备”下为全部、牧场、花园、仓库；默认全部保留四卡，其他区域只显示空状态。没有真实区域归属服务，不要替用户在其他区域放假设备。截图 `specs/screenshots/home-areas-all.jpg` 和 `home-areas-empty.jpg` 为当前首页；`asset-preview.jpg` 是历史素材排版，素材与品牌以清单及当前预览HTML为准。

GitHub网页源码位于 `docs/ui-prototype`；本包 `reference/web_prototype` 是同一版本的固定副本。对接先读需求R01–R40、能力清单、适配契约与地图围栏交接。未来App开发由工程师在生产源码中实现，本PR不承担原生迁移。
