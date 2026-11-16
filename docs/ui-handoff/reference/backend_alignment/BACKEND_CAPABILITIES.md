# 后端与设备能力清单：前端对齐基线

核查日期：2026-11-13。

本清单以jason hao仓库 `edgez-ai/template-offgrid-tracker` 的 `b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501` 为源码基线，对应本次 UI PR 的父提交。SDK 核查的是本机已安装、与该基线依赖版本一致的 `@edgez/react-native-sdk@0.1.35`。原目录 `01_Software_Source` 的提交为 `fb8886b`，还有本地 UI 实验，不能把两份工程的能力混为一谈。

这是源码与配置盘点，未登录生产云端、调用真实业务 API、烧录或连接硬件。下文“已有代码”表示找到实现或调用依据，不表示已通过端到端验收。外部 Appwrite Devices 服务端实现不在此仓库中，响应结构和权限仍须同服务提供方联调确认。

完整资源字段、权限配置、全部注册信道及 App 信道表对照见 [BACKEND_RESOURCE_SNAPSHOT.json](BACKEND_RESOURCE_SNAPSHOT.json)。这是现有配置快照，不是新接口，也不是可直接部署的方案。

## 1. 前端实际要对齐的三层能力

| 层级 | 提供什么 | 前端通过什么使用 | 断网时 |
| --- | --- | --- | --- |
| 云端 | 账号、团队权限、原生设备、遥测、历史、命令队列、照片存储 | Appwrite Account / Teams / TablesDB / Storage，原生 Devices 与 Time Series API | 不能依赖云端成功；保留缓存和明确的离线状态 |
| App 本地与 SDK | 本地身份、Profile、BLE 会话、节点、离线聊天、消息重试、SQLite、离线地图 | App 服务函数、`EdgezMeshSession`、`EdgezMeshSdk`、Organic Maps 原生组件 | 是离网产品的核心执行路径 |
| 设备固件 | 配网、无线连接、传感器、GPS、低功耗、命令执行、配置保存与回报 | BLE / HaLow / 网关 MQTT；不同固件构建能力不同 | 可本地运行的能力取决于型号与构建，不能由网页按钮推定 |

数据路径：

```text
云端模式：节点 ⇄ HaLow 网关 ⇄ MQTT / Devices 服务 ⇄ App / 网站
离线模式：节点 ⇄ HaLow 主控 ⇄ BLE / SDK ⇄ App 本地数据库
网页展示板：本地演示模型，不参与上述真实数据路径
```

三种连接要分别显示：手机与主控 BLE、主控与远端节点 HaLow、产品与云端连接。登录成功、BLE 连接成功、收到某节点上报是不同事件。

## 2. 产品功能矩阵

状态说明：“已有”指源码存在可复用实现；“部分”指缺适配、完整管理流程或产品验收；“未发现”指本轮源码范围内未找到，不能证明其他私有仓库不存在。

| 页面 / 功能 | 云端已有依据 | App / SDK / 固件已有依据 | 对齐判断与处理 |
| --- | --- | --- | --- |
| 首页产品卡片 | Devices 可列出设备及系统状态 | 本地设备与 BLE 状态 | 部分：需要产品实例与能力模型；硬件平台 `kind` 不是产品类型 |
| 添加主控产品 | 云端可创建设备、签发 MQTT 凭证 | `ProvisioningFlow` 扫描 ESP32 / nRF54 / H7608，连接并配网 | 已有配网基础；新 UI 接真实搜索结果，不能只选类别 |
| 网关发现项圈 / 耳标 | 云端 discovered 设备及 `/discovery` 接受入口 | 解析 provisioning request、加密接受包；传感器走 HaLow 入网 | 已有基础；低功耗 nRF 传感器固件不启用 BLE，不能由手机直接 BLE 搜到 |
| 网关下多个节点 | 原生 `gatewayDeviceId`、拓扑、权限范围 | 本地 Profile 保存节点，但 `StoredDevice` 未保存网关归属 | 部分：云端关系转为 App 模型时丢失；须保留，不能把 Profile 全部节点当作某网关节点 |
| 节点重命名 | PATCH Device 的 `name` | 本地 `saveProfileDevice`，已有设备编辑界面 | 已有本地 / 云端记录修改；是否改固件广播名要另定并确认 |
| 最近连接 / 上报 | Device `lastSeenAt`、遥测 `receivedAt` | 节点 `lastSeenMs`、本地 `lastSeenAt` | 已有，但语义不同；界面分别称“最近发现 / 最近上报 / 数据接收时间” |
| 电量 | 遥测 type 12 电压，云端解析与存储 | SDK BLE 解析、SQLite 电压落库存在缺口；部分构建无电池采样 | 部分：先按能力显示电压或“未提供”；没有统一电池百分比来源 |
| 信号 | 原生拓扑链路 `rssi`；说明文件提及 RF 历史接口 | SDK 有 Report / 邻居链路模型 | 部分：需保留观测端、对端、链路类型、时间；不能显示手机 BLE RSSI 冒充节点 HaLow 信号 |
| 定位与地图 | 原生 Device `location`，遥测 type 3 / 4 | GPS / Beacon 位置、手机定位、Android Organic Maps | 已有基础；需保留来源、时间与有效性；缺位置不能补假坐标 |
| 电子围栏 | 三张表和四种形状的云端判断已有代码 | 协议有 GeoFence 字段，尚不足以证明设备端完整几何执行 | 部分：创建编辑适配、启停、确认告警、离线执行未形成闭环 |
| 文字聊天 | 本仓库没有云端聊天业务表 / 同步服务 | SDK 私聊、公共频道，App 收发、SQLite、ACK 和文字重试 | 离线文字基础已有；不应为了新 UI 强行依赖云端账号 |
| 图片聊天 | 摄像头照片服务存在，但不是聊天附件服务 | Mime.image 枚举与附件存储字段存在；Session 接收只处理 text / voice | 未发现完整聊天图片发送 / 接收链路，需要新增 |
| 语音消息 | 未发现云端语音聊天存储服务 | SDK 有录音、分片发送、重组、播放及 Android 原生实现 | SDK 已有；App 新入口、附件状态与语音重试需接通和验收 |
| 已送达 | 不通过云端判定 | SDK 处理消息 ACK，SQLite 对重试消息归并 Delivered | 已有基础；不能把“传给本地 SDK”当成对端送达 |
| 已读 | 未发现独立云端业务契约 | 消息模型未发现 `readAt`；ACK 不表示用户阅读 | 需要新增协议、持久化与回执鉴权，网页当前仅模拟 |
| 实时语音对讲 / 通话 | 未核实 | SDK 暴露方法，但 Android live audio 明确返回 `not_available` | 不能标为已支持；与语音消息分别定义 |
| 验证码登录 / 退出 | 配置启用 email-otp、jwt | App / 网站有邮箱 OTP、创建会话、退出调用 | 已有调用；短信验证码不在本次确认范围 |
| 密码登录 | 配置 `email-password.enabled=false` | App 未找到密码登录入口；网站邀请页可设置密码 | 部分基础存在，但当前不是可用登录流程；需服务开关、客户端、找回流程一起对齐 |
| 本地身份与 Profile | 云端有 Profiles / Teams 和当前 Profile 偏好 | 身份生成、公私钥、本地 Profile CRUD 与共享身份 | 已有基础；账号与无线身份分离，私钥不能当公开资料展示 |
| HaLow 设置 | Profiles 存网络参数 | BLE 初始化 / 配网、固件保存；国家与信道解析 | 已有基础；App 表与默认固件注册表有差异，多产品目标与读回要补 |
| 节点采样设置 | 原生 `/commands`、`/downlinks` 调用 | `sensor.settings`，ESP / nRF 固件有保存与处理 | 已有代码；需要按构建限制选项，排队成功不等于应用成功 |
| 摄像头 | JPEG 分片入库、Storage、完成记录、图库查询、设置下发 | 摄像头固件与 App `CameraGallery` 已有代码 | 已有照片链路；当前 UI 只做卡片是产品范围选择，不是后端完全没有能力 |
| 水阀 / 雷达 | 未发现对应业务命令与专用状态模型 | 图标 / 泛用设备类型不能证明控制或测量能力 | 保留入口；后续另立能力与命令契约 |
| OTA / 固件与诊断 | OTA 命令、状态入库、版本对账 | SDK OTA / USB / BLE 能力与固件工程 | 已有基础，按目标硬件验证；升级过程不能统一成一个立即成功开关 |
| 离线地图与语言 | 不应由云端账号开关决定 | Android 地图下载方法已有；12 语言当前在网页展示板 | 地图管理页与语言包迁移分别实施；网页语言包不是 App 已国际化的证明 |

## 3. 已有接口与调用目录

以下路径相对于已配置的 Appwrite endpoint；不要再次拼接额外 `/v1`。Devices / Time Series 是外部原生服务，表中依据为本仓库客户端调用，未把调用证据冒充服务端实现审查。

用户调用通过会话 / JWT 与项目 ID 认证。API key 仅用于服务端，不能放进 App 或展示板。团队、设备、行、文件权限都需要按实际登录用户验证。

| 编号 | 接口 / 方法 | 当前用途与主要输入 | 输出 / 状态与边界 | 代码依据 |
| --- | --- | --- | --- | --- |
| A01 | Account `createEmailToken` → `createSession` | 邮箱 → `userId`、验证码 `secret`；附安全 phrase | 创建并核验会话；不是短信登录 | [cloud-auth.ts](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/app/cloud-auth.ts#L180) |
| A02 | Account `get` / `deleteSession(current)` / `createJWT` | 当前会话 | 账号信息 / 退出 / 短期 JWT；本地缓存标记不证明会话有效 | 同文件 `verifyCloudSession`、`logoutCloudSession` |
| A03 | Teams `get/create`；TablesDB Profiles `get/list/create/update`；Account `updatePrefs` | Profile 网络参数、teamId、ownerId、identityId | 云端 Profile，当前 Profile 偏好；不能据此推定设备已应用参数 | 同文件 `ensureCloudProfile`，L420 |
| D01 | `GET /devices?includeTopology=true&queries[]=…` | 权限范围与查询；当前 App 限制 100 台 | 原生设备列表、系统状态、拓扑；当前调用没有完整翻页 | 同文件 `listCloudOnlineData`，L606 |
| D02 | `GET /devices/{deviceId}` | 原生 `$id` | Device；不能用显示名、节点编号替代 `$id` | Function `getDevice`，L499；网站设备调用 |
| D03 | `POST /devices` | serial、name、enabled、permissions、metadata、markerType / markerColor | 新建设备记录；目前由手机配网路径使用 | `prepareDeviceCredential`，L290 |
| D04 | `PATCH /devices/{deviceId}` | name、markerType / markerColor、location 或受控 metadata | 设备记录更新；location 为 `[longitude, latitude]` | `updateCloudDevice`，L642 |
| D05 | `DELETE /devices/{deviceId}` | deviceId | 删除记录；Function 删除事件另清理最新遥测，不代表抹除硬件或全部历史 | `deleteCloudDevice`，L638；Function 删除事件处理 |
| D06 | `POST /devices/{deviceId}/credentials` | 手机配网时为设备请求 MQTT 凭证 | clientId、username、password；会生成 / 轮换凭证，不是普通详情读取 | `prepareDeviceCredential`，L337 起附近 |
| D07 | `PATCH /devices/{deviceId}/discovery` | 加密接受包 base64、markerType、markerColor | 网关下传入网接受；具体返回与设备确认需联调 | `acceptCloudProvisioningRequest`，L407 |
| D08 | `POST /devices/{deviceId}/commands` | `{command:"sensor.settings",payload:{samplingIntervalMs,blockSamplingCount}}` | 可靠下行入队；休眠设备通常在后续 Beacon 周期接收 | `updateCloudSensorSettings`，L271 |
| D09 | `GET /devices/{deviceId}/downlinks?limit=50` | deviceId | 原生命令日志的 command、payload、status、retryCount；不是直接读取设备当前配置 | `getCloudSensorSettings`，L255 |
| D10 | 同 D08，`command:"ota"` | payload 包含 url、requestId | OTA 请求；结果由 OTA 状态与版本对账确认 | [site/app/page.tsx](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/site/app/page.tsx#L255) |
| T01 | TablesDB `listRows(telemetry)` | 设备范围、接收时间排序 | 每台设备最新应用遥测，不是系统在线状态数据库；当前 App 最多取 500 行 | cloud-auth.ts L606；Function `upsertLatestTelemetry` |
| T02 | `POST /timeseries/stores/{projectId}/queries` | measurement=`device_` + 规范化原生 ID、duration 秒、limit=1000 | rows 的 `_time/_field/_value`；当前 App 仅消费 `sensor_` 系列 | `getCloudSensorHistory`，L511 |
| T03 | `/devices/{deviceId}/halow-metrics` | 仓库说明提及 RF 历史入口 | 未发现本次 App 对其调用及完整参数 / 响应类型，列为待核验，不能编造契约 | 仓库 AGENTS.md |
| C01 | TablesDB `listRows(camera-images)` | deviceId、status=complete、receivedAt 倒序、limit=12、cursor | 完成照片的 fileId、width、height、totalBytes、receivedAt | cloud-auth.ts L47 |
| C02 | `GET /storage/buckets/camera-images/files/{fileId}/view` | 用户 JWT、项目 ID | 私有 JPEG；App 校验大小及 JPEG 文件头 | 同文件 L56 |
| G01 | 围栏区域 / 规则 / 告警表 | 当前只有 schema 与后台评估实现 | 未发现完整 App CRUD / 告警确认适配，不能当成已有 `/geofences` REST API | `appwrite.config.json`；Function L441 |

### 云端后台任务

Function 入口是事件处理器，不是前端通用 CRUD 控制器：

- `devices.*.mqtt.message.publish`：处理应用遥测、块数据、照片、OTA 与下行结果。
- `devices.*.delete`：清理该设备最新 telemetry 行；不应宣传为历史数据全清除。
- `*/15 * * * *`：清理过期摄像头中间分片。
- `/system/` 与旧 `/telemetry/status` 被明确排除；系统状态、固件版本、网关归属、RF 和拓扑由原生 Devices 服务拥有。

应用 MQTT 根路径为 `projects/{projectId}/devices/{serial}/telemetry/{channel}`。已核对的专用处理包括 sensors / battery 数据校验、ota 状态、`blocks/{HaLow MAC}` 的传感器块与照片分片。普通 JSON 一次接受 1–16 个对象，序列化长度上限检查为 JS 字符串长度 10000；不能直接解释为任意 UTF-8 文件的 10 KB 上限。系统状态字段会在应用遥测写入前剔除。[Function 源码](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/function/src/main.js#L542)

## 4. 数据资源与字段语义

### 4.1 当前配置实际包含的全部九张业务表

| 表 | 用途 | 前端权限与接入注意 |
| --- | --- | --- |
| profiles | 场景 / 网络参数、团队归属、身份关联 | 已登录用户可创建，行权限区分 team 阅读与 owner 修改 |
| telemetry | 每设备最新应用遥测 | Function 写，客户端按设备读权限读取 |
| ota-updates | OTA 请求与执行状态 | Function 写状态，前端查询显示 |
| downlinks | 设备上报的可靠下行状态投影 | 此表 schema 没有 command / payload；不能替代原生 `/downlinks` 日志读取期望参数 |
| geofence-areas | 区域名称、shape、geometry 字符串 | 配置允许用户创建，但 App 适配和几何 / 作用域校验需补 |
| geofence-rules | 关联区域、设备、进入 / 离开提醒 | 没有已核实 enabled / revision / gatewayDeviceId 字段 |
| geofence-alarms | 活跃 / 确认状态、位置、触发及解除时间 | Function 创建时只授予设备读取权限；未确认用户更新 acknowledged 的完整授权路径 |
| camera-images | 图片分片会话及完成后的文件元数据 | 私有读取；前端图库仅取 complete |
| camera-image-parts | 照片分片暂存与过期时间 | 后台中间表，不作为用户照片列表 |

九张表的全部 79 个字段、必填性、类型、范围、加密和权限原样保存在 JSON 附件中。另有一个以项目 ID 为标识的 Time Series Store 和一个私有 `camera-images` Storage bucket。原生 Devices 不是以上九张表中的一张；不能自行另建同名表冒充原生服务。

### 4.2 项圈 / 耳标详情建议对应的实际字段

| UI 内容 | 真实来源 | 必须保留的语义 / 缺口 |
| --- | --- | --- |
| 节点唯一 ID | 云端 Device `$id`；本地 StoredDevice.id；无线 nodeNum / MAC | 建立映射，不可直接混用；nodeNum 是 bigint，JSON 使用十进制字符串，不转 JS Number |
| 节点名称 | Device.name 或本地 DeviceDetails.name | 明确是本地别名、云端名称，还是固件广播名称；不能修改身份主键 |
| 所属网关 | gatewayDeviceId，必要时结合观测与路由 | 逻辑归属、传输网关与某次观测的报告者分开 |
| 最近时间 | lastSeenAt / lastSeenMs；telemetry.receivedAt | 接收时间不等于 GPS 采集时间，也不等于 BLE 连接时间 |
| 电池 | sensor type 12 → batteryVoltage，单位 V；部分包另有 batteryVoltageMv | 云端路径有解析；离线 SDK 与 SQLite 待补；不直接换算百分比 |
| 信号 | topology.links[].rssi，单位 dBm | 保留 reporter / peer / observedAt；没数据就是未知 |
| 位置 | Device.location；传感器 type 3 / 4；Beacon 坐标 | 云端点序 `[经度,纬度]`；保留 source、sampleAt、validity、坐标系；手机固定位置不是动物 GPS |
| 轨迹 / 历史 | Time Series lat / lon 或本地 sensor_samples | 当前 App 历史适配只筛 `sensor_`，未等于已实现经纬度轨迹接口；需补查询和采样时间策略 |
| MCU 温度 | sensor type 13 → mcuTemperature，单位 ℃ | 不是体温，也不能直接推断动物健康 |
| 其他传感量 | types 1–11 | 实际型号上报才显示，不能因为协议有字段就生成数据 |

传感器类型完整映射：1 temperature，2 humidity，3 latitude，4 longitude，5 binaryLengthBytes，6 accelX，7 accelY，8 accelZ，9 gyroX，10 gyroY，11 gyroZ，12 batteryVoltage，13 mcuTemperature。云端源类型校验覆盖 1–13；三轴加速度或陀螺仪要求对应 X / Y / Z 成组出现。温湿度、IMU、长度字段的单位和标度仍须按设备协议确认，不能把 MCU 温度标成动物体温。

### 4.3 配置和消息状态

| 对象 | 已有状态 / 字段 | 前端应如何使用 |
| --- | --- | --- |
| BLE | not-selected / preparing / disconnected / connecting / connected / ready / error | GATT connected 不等于控制通道 ready；无主控时保留配对指引 |
| 云连接 | login / offline / online | online 需会话、Profile 初始化与联网核验，不能只看本地登录标记 |
| 下行结果 | cached / transmitting / applied / failed / expired；requestId、revision、detail、retryCount | 显示“待设备接收 / 传输中 / 已应用 / 失败 / 已过期”，关联到具体产品目标 |
| OTA | pending / succeeded / failed / busy；requestId 与版本 | 版本、状态和目标设备一起对账，不把“发命令成功”当升级成功 |
| 私聊 | Sending / Sent / Delivered，文字重试另有 Retrying 与 Failed | Sent ≠ Delivered；Read 需要独立新回执 |
| 公共聊天频道 | SDK 端口 38801、38803、38805、38807、38809 | 是通信地址，不是射频中心频率；网页 c1–c5 仅是展示别名 |

目前 App 云端刷新采用 5 秒轮询，图库采用 10 秒轮询；不能承诺 WebSocket 级实时性。当前取设备 100 条、最新遥测 500 条、历史 1000 条、下行 50 条的做法都有规模边界，需要真实翻页 / 按网关查询，不能作为大量节点已完整支持的依据。

## 5. 各模块的关键接口缺口

### 5.1 项圈 / 耳标与低功耗构建

1. `storedDeviceFromCloud()` 把原生 Device 转为 `StoredDevice` 时未保留 gatewayDeviceId；现有本地模型也缺该字段。多网关展示前必须补适配与持久化策略。
2. SDK 0.1.35 的 `EdgezMeshSession.sensorData()` 只映射 types 1–11；App 的类型扩展声明 12 / 13 不会自动修改运行时解析。
3. `saveProfileSensorSamples()` 的 SQLite 写入没有 batteryVoltage / mcuTemperature 列。即使将实时解析接上，重启后的历史恢复仍需补迁移和字段映射。
4. 当前 `fgh200m_sensor.conf` 启用 GPS_ONLY、关闭 BLE 和 ADC：GPS 有效才上报位置，电池电压在该测量构建中不可用。不能对所有项圈强制显示电池数值。
5. `mesh_join_main.c` 的 GPS_ONLY 分支只接受 samplingIntervalMs=60000、blockSamplingCount=0，其他组合返回不支持。通用 App 表单范围 1–3600 秒 / 0–32 不代表这个构建允许任意修改。

依据：[Profile / SQLite](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/app/profile-store.ts#L762)、[低功耗配置](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/firmware/zephyr/fgh200m_sensor.conf)、[固件设置处理](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/firmware/nrf54/src/mesh_join_main.c#L291)。

### 5.2 电子围栏

云端已有 circle、oval、rectangle、polygon 判断，但有六个必须明确的边界：

- 当前基于有效位置和 `target.metadata.profileId` 查规则；只有 Profile 权限但没有该 metadata 的设备可能不参与判断，需要核验传感器入网后的 metadata。
- 当前根据“现在在区域内 / 外”激活 enter / exit 告警，未先证明发生了边界穿越。首次上报就在区域内也可能触发 enter；不能直接宣传为严格的进入事件。
- rule.deviceIds 为空表示当前 Profile 全部设备，不自动限制当前网关。
- 表里没有规则 enabled；几何保存用 JSON 字符串，现有后台不是完整的几何合法性校验服务。
- acknowledged / acknowledgedAt 有字段，但创建告警的行权限仅为 read；需要专门的确认写入与授权设计。
- 云端函数只在收到位置遥测时计算。纯离网判断、后台通知、规则下发 / 读回、GPS 漂移抑制、事件去重和失联策略需要另定执行端。

依据：[evaluateGeofences / containsArea](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/function/src/main.js#L417)。协议中的 GeoFence 名称 / ID / 索引字段不等于四种形状已经在网关独立执行。

### 5.3 聊天

已有文字私聊、公共频道、离线存储、ACK、自动重试基础。私聊重试参数为 15 秒 ACK 超时、5 秒检查周期、最多自动重试 3 次；只有最近 Beacon 判定在线时才自动重试，当前在线窗口为 120 秒。这个聊天窗口不能套给低频上报的动物传感器。

语音需纠正旧交接中的笼统表述：SDK 已实现 `startVoiceMessage / finishVoiceMessage / playVoiceMessage`，发送按 290 字节音频片段打包，接收端重组；Android 原生录制为 API 29+ 的 Ogg/Opus（codec=2），旧版本 3GP/AMR-NB（codec=1），并通过 MediaPlayer 播放。因此不能统一要求“必须是原始 Opus 字节”，也不能把任意浏览器 WebM 直接塞入该链路。编解码与容器要按实际 SDK 版本协商并做跨端测试。

当前 App `MeshMessages` 的发送入口只调用文字；`message-delivery.ts` 的重试也只调用 sendTextMessage。应补语音入口、取消 / 权限 / 播放状态、按媒体类型重发和逻辑消息关联。SDK 代码存在不代表手机、手持设备和不同固件之间已经全部联调。

图片仍缺专门的选择压缩、分片协议、接收分发、重组校验、存储与重试链路。SDK 的 `handleConversation()` 当前只消费 Mime.text / Mime.voice。摄像头 JPEG 上传不能直接当成聊天图片发送实现。

已读需要单独定义 messageUuid、阅读方身份、阅读时间、会话归属、回执幂等与断线补发；云端如不参与离线聊天，就不能把已读依赖到云端服务。收到普通 ACK 只能表达已有的送达语义。广播不显示虚假的“全部已送达 / 全部已读”。

依据：[App 聊天发送](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/app/MeshMessages.tsx#L212)、[重试](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/app/message-delivery.ts)、SDK 本机包 `src/EdgezMeshSession.ts:147 / 261`、`src/EdgezMeshSdk.ts:619`、Android `EdgezReactNativeSdkModule.kt:526`。

### 5.4 HaLow / Mesh 设置

现有 App 保存 country、halowChannel、meshId、meshPassphrase，初始化时派生 meshFrequencyKhz 与 meshBandwidthMhz；网关配网固件保存对应字段。网页的 desired / applied / target 是设计规格，不是现有 App 已有完整的多产品配置管理服务。

必须把国家、信道、中心频率、带宽视为合法组合。Mesh ID 按目标固件的 UTF-8 字节约束校验，密码允许空或 8–63 UTF-8 字节；不能把 SDK 字符串截取当成固件校验，也不能将 passphrase 直接称为 AES 密钥。maxHop / GeoFence 有协议字段，不等于全目标已打通可编辑的产品功能；发射功率写入 / 读回未确认。

当前完整默认注册表与 App 表对照如下。详细每条频率、带宽及所有驱动字段都在 JSON 附件，未用示例省略：

| 地区 | 固件默认模板记录数 | 当前 App 记录数 | 核对结论 |
| --- | ---: | ---: | --- |
| AU | 26 | 13 | 当前 App 的频率组合与默认 REVMF 集合不同；不能只按数量补齐，须确认构建选择 |
| CA | 48 | 26 | App 未覆盖模板全部组合 |
| EU | 5 | 5 | 本次按信道号 / 频率 / 带宽对照一致 |
| GB | 7 | 7 | 同上 |
| IN | 3 | 3 | 同上 |
| JP | 12 | 6 | App 未覆盖模板全部组合 |
| KR | 16 | 10 | App 未覆盖模板全部组合 |
| NZ | 23 | 13 | App 未覆盖模板全部组合 |
| US | 48 | 48 | 本次按信道号 / 频率 / 带宽对照一致 |

完整集合为 9 个注册地区、188 条记录，当前表无 CN。AU 采用该源码默认 REVMF；其他编译选择必须读取对应构建。表中差异不自动等于所有现售硬件都应开放那些组合。最终以固件构建、硬件 SKU 和适用区域许可的交集生成用户选项，本次没有核验现行法规或硬件认证。

保留的 11 个驱动字段：frequencyHz、dutyCycleCentiPercent、omitControlResponse、globalOperatingClass、s1gOperatingClass、number、bandwidthMHz、maxEirpDbm、minPacketSpacingUs、airtimeMinUs、airtimeMaxUs；另有显示用 frequencyMHz。

### 5.5 摄像头、水阀与雷达

摄像头已有的是周期 JPEG 照片，不是已核实的实时视频流。代码可接收不超过 512 KiB 的 JPEG，校验分片覆盖与摘要，写私有 Storage；暂存 TTL 为 24 小时，后台每 15 分钟清理。App 图库读取 complete 记录并按 12 张翻页。

当前 `sensor.settings.blockSamplingCount` 在 VC0706 摄像头固件中复用为分辨率索引：0 关闭抓拍；1=160×120、2=320×240、3=640×480、4=1024×768、5=1280×720、6=1280×960、7=1600×1200、8=1920×1080、9=2304×1296。samplingIntervalMs 为抓拍周期，不能在 UI 一律标成“采样分块数”。实际分辨率仍受摄像头型号支持限制。

用户提供的 NeoEyes NE302 外形参考不能证明它运行 VC0706 固件；两种设备不应直接共用能力声明。水阀尚未找到开关、开度、流量、定时等完整命令模型；雷达尚未找到目标、距离、速度等完整业务模型。泛用命令通道和图标存在都不足以确认这些产品已支持。

依据：[照片处理](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/function/src/camera-images.js)、[摄像头分辨率](https://github.com/edgez-ai/template-offgrid-tracker/blob/b0b35eb1d98b1ea3cdc7fcb1a3ef39a17cfc9501/firmware/nrf54/src/camera_resolutions.h)。

## 6. 给前端的适配契约草案

本节是待 App / 后端 / 固件同事共同确认的设计，不是宣称已经新增的 API。

| 对象 | 建议明确的字段 | 要解决什么 |
| --- | --- | --- |
| ProductInstance | productId、productType、model、firmwareTarget、firmwareVersion、capabilities、controlDeviceId | 卡片对应具体产品；不从用户名称猜类型 |
| Node | nodeId、cloudDeviceId、serial、nodeNumString、gatewayId、alias、hardwareName | 多种 ID、网关归属和重命名分离 |
| Observation | value、unit、sampleAt、receivedAt、source、validity；信号另含 reporterId / peerId | 不混单位、时间与链路；支持缺失 / 过期 |
| ConnectionState | productId、bleState、meshState、cloudState、lastError | 区分三种连接，避免全局一个“已连接” |
| ConfigTransaction | targetId、requestId、revision、desired、applied、status、error、confirmedAt | 保存、入队、设备应用、读回对账分开；失败保留上次确认值 |
| Message | logicalMessageId、attemptIds、threadId、senderId、recipientId、kind、text、attachment、sendState、deliveredAt、readAt | 按媒体类型发送 / 重试；送达与已读独立 |
| Fence | area、rule、scope、enabled、executionTarget、revision、syncState | 明确谁保存、谁执行、作用哪些节点、离线是否有效 |
| DeviceCapabilities | supportsGps、supportsBatteryVoltage、supportsChatImage、supportsVoiceMessage、settingsRanges、cameraModes | 按型号与构建显示功能；未知能力不默认支持 |

优先复用现有原生 Devices / TablesDB / SDK，实现 App 的适配层；只有确认现有能力无法满足时再新增服务端 / 协议字段。前端无需因重画卡片而重建一套云端设备数据库。

## 7. 待对齐工作清单与验收

| 优先级 | 事项 | 主要协作方 | 可验收结果 |
| --- | --- | --- | --- |
| P0 | 确认每款产品型号、固件目标、构建和能力 | 产品 / 固件 / App | 同一 UI 在无电池、无定位、固定 GPS 周期构建上正确隐藏或标未知 |
| P0 | 原生 Devices API 参数、权限、错误码和完整响应 | 服务提供方 / App | 两个账号、两个网关下归属和权限正确，分页无漏节点 |
| P0 | 节点 ID 与 gatewayDeviceId 贯通 | App / 云端 | 卡片进入只展示对应网关节点，改名不改变身份 |
| P0 | 电压 / MCU 温度 SDK 解析与 SQLite 迁移 | SDK / App | 实时报数、重启恢复、缺失字段都正确 |
| P0 | 配置能力表及真实应用状态 | 固件 / App / 云端 | GPS_ONLY 构建不会让用户提交注定不支持的组合；失败不显示已应用 |
| P0 | 围栏执行位置、启停和告警权限 | 产品 / 后端 / 固件 / App | 断网和首次上报语义明确，增删改与确认全流程可验收 |
| P0 | 图片聊天与独立已读契约 | SDK / 固件 / App | 不以 Mime 枚举或普通 ACK 冒充已实现 |
| P1 | 接 SDK 语音入口并按媒体类型重试 | App / SDK | 录音、取消、拒绝权限、播放、断链失败和重发保留原附件 |
| P1 | 密码登录、修改 / 找回密码 | 账号服务 / App | 服务开关与真实认证流程一致；不会只做一个无效入口 |
| P1 | 地图轨迹、离线包管理与多语言迁移 | App | 有真实下载 / 删除 / 失败状态；已有网页语言包迁移后逐页验证 |
| P2 | 摄像头详情 | 产品 / App / 固件 | 按实际型号开放照片、设置与状态；不默认承诺视频 |
| P2 | 水阀与雷达 | 产品 / 固件 / 后端 | 先定义真实数据与命令，再做可操作内页 |

每一项接口联调至少覆盖：未连接、已连空网络、多节点活跃、失联与重连、权限不足、参数被拒绝、数据缺失 / 过期、跨目标切换、应用失败。节点轮询周期与在线判定应依据该型号，不统一套用聊天的 120 秒窗口。

## 8. 本轮核查与文件范围

本轮仅生成此文档和 JSON 配置快照，未修改 App、固件、云端资源或此前 PR。配置快照应能与上述固定提交的配置逐字段对比，信道表应能与固定 mmregdb 注册列表核对；后续源码或 SDK 更新后应重新盘点。

未执行云端请求或硬件测试，也没有把已有网页的模型测试结果当成本轮后端验收。服务端原生 Devices 的实现、当前部署的认证开关、SKU 能力、地图跨平台能力与真机媒体互通仍需对应负责人确认。
