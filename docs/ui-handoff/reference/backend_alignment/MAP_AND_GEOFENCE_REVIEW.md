# 原版地图、围栏、中国坐标与费用核查

核查日期：2026-11-14。源码依据为既定基线 b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501 和本地 SDK 0.1.35；没有做中国地区真机定位测试，没有更换原生地图引擎或购买服务。

## 原版已经实现什么

`app/HomeMap.tsx` 使用 SDK 的 `EdgezOrganicMap`，不是高德地图。App 把设备经纬度和手机位置组成 nodes，交给 Android 原生地图渲染；同时根据地图相机做 Mercator 投影，给节点增加点击区域。提供日夜主题、移动到手机位置、按当前区域提示下载、下载进度和错误信息。源码传入 lines=[]，不能据此声称已提供轨迹与围栏图形编辑。

原生部分在 SDK 的 `android/src/main/java/ai/edgez/react_native_sdk/EdgezOrganicMapViewManager.kt`；经纬度交给地图 Framework，标记及线条也有参数解析。SDK 的 `src/EdgezOrganicMap.tsx` 明确在非 Android 平台显示目前仅支持 Android。Organic Maps 独立 App 有 iOS 版本，不代表这个 React Native 封装已支持 iOS。

这套方案适合离网产品：先下载区域数据，地图在手机本地绘制，节点 GPS 通过无线链路更新，并不需要每次位置上报都请求地图公司的服务器。Organic Maps 官方说明其地图基于 OpenStreetMap，下载后可离线使用。[Organic Maps](https://organicmaps.app/)

## 围栏完成度

原版属于“后台已有几何判断，产品闭环尚未完成”。后台有 circle / oval / rectangle / polygon 及区域、规则、告警表；尚未找到与新 UI 对应的完整 App 创建编辑、启停、同步、告警确认权限及离网执行流程。

判断在云函数收到位置遥测时触发。当前位置处于区域内 / 外不自动等价于发生了进入 / 离开事件，首次上报、漂移、去重、失联和跨网关范围都需要补产品规则。普通地图 SDK 自带围栏服务也不能自动替代动物节点的监测：手机地理围栏通常判断手机位置，不能当成远端项圈或网关的离线围栏执行。

## 为什么在中国会出现偏移

先区分 GPS 测量误差、陈旧定位和坐标系不匹配。高德官方文档说明 GPS 常用 WGS84，高德使用 GCJ-02，百度使用 BD-09；非高德坐标显示在高德底图前需要转换。改变图层可能碰巧改变了坐标体系，不能把“换图层”当成长期纠偏方案。[高德坐标说明](https://lbs.amap.com/api/javascript-api-v2/guide/transform/convertfrom)

原版采用 Organic Maps / OSM 路线，从架构上更适合与原始 GPS 坐标配合；这是架构推断，不是“全中国绝对不偏”的测试结论。本轮 App 与 SDK 封装的可读代码中没有找到明确的 GCJ-02 / WGS84 识别与转换策略，也未完整审查预编译地图引擎及其中国地区数据处理。

另一个实际问题在 `app/location-utils.ts`：非 Android 分支允许使用最长 15 分钟的旧位置，并使用 Location.Accuracy.Low 取得后续位置；返回值只保留经纬度，没有保留采样时间和精度。Android 分支调用 SDK 原生定位。因而“我的位置”与“项圈 GPS”必须分别标明来源，手机旧位置或低精度不能归因于项圈偏移。

建议统一契约：原始位置保留 coordinateSystem、source、sampleAt、receivedAt、validity，以及设备确实提供时的 accuracyMeters。未提供精度时不伪造精度圈。内部围栏和原始位置使用同一已知坐标体系；更换底图时只在显示适配层转换，点标记和围栏轮廓同步转换，不能只转换其中一个，也不能反复加偏。中国、海外和切图层均应做实地点位与围栏往返验证。

## 五万元一年是什么

2026-11-14 查看的高德官网技术服务许可表列有每年 ¥50,000 和 ¥100,000 两档；许可权益与基础服务配额、技术支持有关。平台另列 API 调用、超额流量包和 QPS 升级计费。不是购买 GPS 卫星定位，也不是所有使用地图的 App 都必须交这笔钱；是否接入高德及商业许可范围取决于产品服务和适用协议。[高德定价](https://lbs.amap.com/upgrade)、[服务协议](https://lbs.amap.com/pages/terms/)

免费的个人 / 非商业配额不能直接当作 KEEPTEEN 商业产品的授权依据。也不要把五万元简单按用户数或项圈位置点数相乘：地图服务调用和设备 GNSS 自身输出坐标是两件事。

## 不购买高德的可行路线

1. 优先评估沿用原有 Organic Maps 封装，补完地图包管理、坐标元数据和跨平台验证，减少这轮 App 重构范围。
2. 若需要更自由的品牌样式或 iOS / Android 一致能力，可评估 MapLibre Native / React Native 与许可合适的 OSM 衍生地图数据。MapLibre Native 官方采用 BSD 2-Clause，是免费开源渲染库；它本身不等于已经提供地图数据、下载服务器和卫星影像。[MapLibre Native](https://github.com/maplibre/maplibre-native)
3. OSM 数据采用 ODbL，但官方 tile.openstreetmap.org 公共瓦片服务器有明确使用政策，禁止批量下载与离线预取；离线包应使用允许离线分发的供应方或自行生成、托管。[OSM 数据许可](https://www.openstreetmap.org/copyright)、[瓦片使用政策](https://operations.osmfoundation.org/policies/tiles/)

还需分清 Organic Maps 源码和现成二进制地图包：当前官方 DATA_LICENSE.txt 对 .mwm / .bin 数据另列署名要求，并规定白标 / 换品牌使用需书面许可。不能看到源码 Apache 2.0 就断言 KEEPTEEN 可直接去品牌分发官方地图包；应核对实际采用版本、数据包与下载服务的授权。[官方二进制数据许可](https://github.com/organicmaps/organicmaps/blob/master/DATA_LICENSE.txt)

免费方案可以不支付地图渲染库许可费，但仍需承担开发、数据制作 / 更新、下载流量、存储、维护和适用授权条件。当前动物定位产品优先需要离线底图、节点标记、位置时间和可靠围栏，不必先购买城市搜索、实时路况或导航套餐。是否增加这些服务，按产品实际需求决定。


## 实际下载链路与本次“斯德哥尔摩 / 一直排队”的核查

用户反馈：人在深圳，原 App 打开却在斯德哥尔摩，点击下载后停在排队 / 0%，开关 VPN 都不改善。

确定的初始化行为：HomeMap 无有效 Profile 中心时写死 latitude=59.3293、longitude=18.0686；进入地图不会自动执行 centerOnCurrentLocation，地图控件的定位按钮才调用该函数。斯德哥尔摩是默认视野，不是深圳坐标被 GCJ-02 变换到欧洲。地图中心在瑞典时，区域下载提示会按瑞典地图中心识别地区，不会自动下载广东。

正常调用顺序：

1. `HomeMap.tsx` 设置 enableMapDownloads。SDK 在渲染就绪后约 5 秒开始检查，此后约每 3.5 秒检查一次。
2. 地图缩放至少达到 9、中心区域还未下载时，以屏幕中心经纬度调用 nativeFindCountry，返回地区 ID 并显示下载提示。
3. 用户点击 DOWNLOAD，调用 `map.current.downloadRegion(regionId)`，经 React Native 命令传给 `MapManager.startDownload(regionId)`。
4. 注意：RN 封装在原生请求真正开始前就先发出 Queued map 事件。因此“排队 / 0%”仅证明按钮进入调用链，不证明已连上下载服务器。
5. SDK 0.1.35 依赖 EdgeZ Organic Maps AAR 0.0.8。该版本目录的数据版本为 260503，详细地图按 `maps/260503/地区ID.mwm` 下载；元数据服务为 meta.omaps.app/maps，返回镜像列表。
6. SDK 把下载字节数 / 总字节数转换成进度，STATUS_DONE 才代表本地缓存完成。底层 StorageCallbackData 具有 errorCode，但当前 RN 事件没有透传；没有映射所有状态，也没有给页面提供完整的取消、重置 / 重试和排队超时诊断。requestedRegions 集合还会去重，重复点击不一定会重启底层任务。

2026-11-14 从本机做的只读网络检查：元数据服务返回 HTTP 200；其返回的 cdn-us1、cdn-us-west2 和中央 cdn 三个镜像，对 260503 的 Sweden_Stockholm.mwm 与 China_Guangdong.mwm 的 HEAD 请求均返回 HTTP 200。响应长度分别为 69,951,419 与 199,526,934 字节。没有下载地图正文，也没有用本机测试冒充手机网络测试。写死的备用 nl1 / uk1 镜像对 World.mwm 返回 404，但中央与当前元数据返回的镜像仍可用，因此不能断言地图服务器整体停服。

原版可以先尝试点击定位准星按钮，允许手机位置权限，确认地图确实移到深圳后再看广东下载提示。它不需要先连项圈、网关或登录云账号才能取得普通离线底图。若仍排队，必须抓手机当次的地图原生日志、地区 ID、数据版本、网络错误 / HTTP 状态、写入目录和剩余空间；不能靠不停切 VPN 或购买高德许可判断根因。本机 ADB 当前没有检测到连接设备，因此尚未定位用户手机上 0% 的唯一原因，也未修改原生 App / SDK。

建议的原生修复范围（待单独确认）：首次位置选择与权限指引；明确“查看节点位置 / 我的位置”；地图包地区与大小确认；传递 queued / transferring / verifying / installed / failed 及错误码；取消 / 重试接口、超时恢复和多任务状态处理。默认城市与下载卡住应作为两个问题分别验收。

源码依据：[HomeMap 固定基线](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/app/HomeMap.tsx)、[地图目录 0.0.8](https://github.com/edgez-ai/organicmaps/blob/v0.0.8/data/countries.json)、[SDK MapManager](https://github.com/edgez-ai/organicmaps/blob/v0.0.8/android/sdk/src/main/java/app/organicmaps/sdk/downloader/MapManager.java)、[下载与元数据流程](https://github.com/edgez-ai/organicmaps/blob/v0.0.8/libs/storage/map_files_downloader.cpp)。
