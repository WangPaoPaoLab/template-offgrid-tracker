# 当前可交付资料

更新：2026-11-16（首页区域、六项设置、联合品牌与工程资料版）。

- [当前需求 R01–R40](00_USER_REQUIREMENTS.md)：用户已确认要求、后续纠正及发布范围。
- [正式网页](web_prototype/index.html)：首页“设备”，区域“全部 / 牧场 / 花园 / 仓库”；四张卡片只在“全部”。
- [独立工程评审页](web_prototype/review.html)：生命周期、失败与能力场景，不作为客户入口。
- [工程师交接说明](04_APP_ENGINEER_HANDOFF/README.md) 与 [素材预览](04_APP_ENGINEER_HANDOFF/ASSET_PREVIEW.html)。
- `KEEPTEEN_App_UI_Handoff_2026-11-16.zip`（独立交付文件，未嵌套在包内） 及同名 `.sha256`；11月14日ZIP为旧快照。
- [后端能力清单](backend_alignment/BACKEND_CAPABILITIES.md)、[适配契约](web_prototype/ADAPTER_CONTRACT.md)。
- [地图与围栏实施交接](backend_alignment/MAP_GEOFENCE_IMPLEMENTATION_HANDOFF.md)。真实地图、下载及可靠越界闭环仍待接入与真机验收。

GitHub交付目标：`WangPaoPaoLab/template-offgrid-tracker` / `ui/prototype-handoff`，更新 [jason hao仓库的PR #1](https://github.com/edgez-ai/template-offgrid-tracker/pull/1)。仓库入口 `docs/KEEPTEEN_UI_HANDOFF.md`；最新网页位于 `docs/ui-prototype/`，完整素材包位于 `docs/ui-handoff/`。远端状态以PR head为准。

网页是交互规格板，不是原生App或生产连接。发布仅包含上述资料，不修改或部署原生App、SDK、固件、云端服务，也不合并PR。
