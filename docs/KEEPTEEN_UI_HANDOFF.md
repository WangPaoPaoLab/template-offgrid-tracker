# jason hao：App UI 重构交接入口

更新：2026-11-16。当前交接基于本fork的 `ui/prototype-handoff` 分支，更新上游 [PR #1](https://github.com/edgez-ai/template-offgrid-tracker/pull/1)。

**先看当前网页，再按需求和能力表迁移到App；不要直接移植演示适配器。**

| 资料 | 入口 |
| --- | --- |
| 网页使用说明和当前截图 | [ui-prototype/README.md](ui-prototype/README.md) |
| 默认用户界面 | 下载分支后打开 `ui-prototype/index.html` |
| 工程评审界面 | 下载分支后打开 `ui-prototype/review.html` |
| 最新需求及已否定方案 R01–R41 | [需求基线](ui-handoff/reference/00_USER_REQUIREMENTS.md) |
| 原有后端和设备实际能力 | [能力清单](ui-handoff/reference/backend_alignment/BACKEND_CAPABILITIES.md) |
| 前端到真实App的对齐点 | [接口交接](ui-prototype/UI_HANDOFF.md)、[适配契约](ui-prototype/ADAPTER_CONTRACT.md) |
| 地图与电子围栏 | [专项实施交接](ui-handoff/reference/backend_alignment/MAP_GEOFENCE_IMPLEMENTATION_HANDOFF.md) |
| 透明Logo、4产品图、23功能图标、iOS/Android资源 | [素材包说明](ui-handoff/README.md)、[预览](ui-handoff/ASSET_PREVIEW.html)、[清单](ui-handoff/ASSET_MANIFEST.json) |
| 独立下载的完整交接ZIP | [2026-11-16 ZIP](ui-deliverables/KEEPTEEN_App_UI_Handoff_2026-11-16.zip)、[SHA-256](ui-deliverables/KEEPTEEN_App_UI_Handoff_2026-11-16.zip.sha256) |
| 校验与完成边界 | [VALIDATION](ui-handoff/VALIDATION.md) |

底部仅“设备 / 设置”；聊天从独立产品卡片进入。首页只有“全部”有四卡，牧场/花园/仓库为空展示；水阀和摄像头不做内页。设置一级仅账号、HaLow、通知、通用、帮助、关于。完整设计优先级与原话以需求基线为准。

本次提交范围仅 `docs/` 下UI及交接资料。手机App、SDK、固件和云端均未由本PR改动；真实地图下载、可靠围栏执行、图片传输、独立已读和密码服务尚需工程实现。网页目标设计不能当作后端已完成的证明。不自动合并PR。
