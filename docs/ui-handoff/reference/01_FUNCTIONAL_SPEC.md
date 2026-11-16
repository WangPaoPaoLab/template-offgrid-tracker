> 2026-11-14：本文保留早期规划，其中电量百分比、固定周期、寻物蜂鸣、实时语音和地图平台能力不能视为已支持。当前实现边界以 `backend_alignment/BACKEND_CAPABILITIES.md`、`web_prototype/UI_HANDOFF.md` 与 `backend_alignment/MAP_AND_GEOFENCE_REVIEW.md` 为准。

> 用户需求与产品入口以 [00_USER_REQUIREMENTS.md](00_USER_REQUIREMENTS.md) 及用户最新指令为准；本文的技术规划不能覆盖已确认的双 Tab 和聊天产品归属。

# 📋 EdgeZ Offgrid Tracker 纯功能与业务逻辑全景规范手册
> **文档性质**：纯功能、业务逻辑与数据模型定义书（Pure Functional & Business Logic Specification）  
> **文档用途**：作为后续 UI/UX 架构设计与界面原型构思的唯一逻辑需求基准，不包含任何视觉风格或排版预设。  
> **通信底层**：BLE（蓝牙低功耗） + Wi-Fi HaLow (IEEE 802.11ah 亚千兆赫 Mesh) + SQLite (WAL模式)

---

## 目录
1. [系统总体架构与通信机制](#一-系统总体架构与通信机制)
2. [核心数据模型与数据字典](#二-核心数据模型与数据字典)
3. [五大核心功能模块规范](#三-五大核心功能模块规范)
   - [3.1 配置档案管理（Profile Management）](#31-配置档案管理profile-management)
   - [3.2 离线地图与信标定位（Location & Beacon Tracking）](#32-离线地图与信标定位location--beacon-tracking)
   - [3.3 离线 Mesh 私有通信与遥测（Mesh Communications & Telemetry）](#33-离线-mesh-私有通信与遥测mesh-communications--telemetry)
   - [3.4 硬件蓝牙配网向导（BLE Provisioning Flow）](#34-硬件蓝牙配网向导ble-provisioning-flow)
   - [3.5 系统监控、射频诊断与设置（System & Diagnostic）](#35-系统监控射频诊断与设置system--diagnostic)
4. [系统状态机与边界异常规则](#四-系统状态机与边界异常规则)

---

## 一、 系统总体架构与通信机制

### 1.1 硬件协同与双重网络拓扑
Offgrid Tracker 是一个专为**极端无公网环境**设计的去中心化通信与定位终端。系统的完整运作依赖于三层拓扑：

```
[手机终端 (App)]
      │
      │ 1. 短距 BLE 蓝牙 (透传数据、下发配置、读取状态)
      ▼
[随身硬件网关 (ESP32 或 Nordic nRF54 + MM8108 HaLow射频)]
      │
      │ 2. IEEE 802.11ah Wi-Fi HaLow (亚千兆赫无线电 Mesh 网络，公里级无网覆盖)
      ▼
[远距离 Mesh 节点群 (队友信标 / 车载基站 / 环境传感器)]
```

* **手机角色**：主控大脑、图形人机交互界面、本地 SQLite 数据库宿主。
* **随身硬件角色**：无线电调制解调器（Modem）。负责通过 BLE 接收手机的通信指令，调制为 HaLow 射频长波并在 Mesh 网络中泛洪中继扩散；反之将空中截获的信标、文本与传感器帧解析后通过 BLE 上送手机。

### 1.2 双运行模式定义
| 运行模式 | 触发条件 | 网络依赖 | 数据存储与同步机制 |
| :--- | :--- | :--- | :--- |
| **纯离线模式 (Local-First)** | 默认开启，或 `connectToCloud = false` | 完全脱网（无蜂窝网络、无Wi-Fi、无互联网） | 纯本地闭环。所有档案、设备、聊天记录、传感器时序全部持久化在本地 SQLite 数据库中，不向任何远程服务器发包。 |
| **选配云端模式 (Cloud-Connected)** | 用户在设置中显式勾选 `connectToCloud = true` 且登录有效 Appwrite 会话 | 依赖公网或营地卫星链路 | 现场设备状态、传感器时序历史直接与 Appwrite 云端同步，供指挥中心 Web 端大屏统一监控；本地保留 SQLite 数据隔离。 |

---

## 二、 核心数据模型与数据字典

系统所有业务逻辑完全建立在以下 5 个数据实体之上：

### 2.1 Profile（配置档案）
定义当前队伍或场景的专属无线电通信参数。同组互通的队员必须具备相同的射频参数。
* `id` (String, PK)：档案唯一 UUID。
* `name` (String, 必填)：档案别名（如 "阿尔卑斯搜救分队"）。
* `country` (String, 必填)：无线电合规地区码（如 `US`, `CA`, `EU`, `GB`, `AU`, `NZ`, `JP`, `KR`, `IN` (硬件射频物理限定为 868MHz 与 915MHz 两大合法频段)）。决定允许使用的频段。
* `bandwidth` (String, 必填)：频宽，可选 `1 MHz`（距离更远、穿透更强）或 `2 MHz`（吞吐速率更高）。
* `channel` (Integer, 必填)：工作信道编号（如 27，对应具体中心频率 915.5 MHz）。
* `meshId` (String, 必填)：Mesh 局域网识别字串（同组节点必须严格相同）。
* `passphrase` (String, 选填)：无线电空中通信加密密码（8 ~ 63 位字符串，用于生成空中加密密钥）。
* `connectToCloud` (Boolean, 必填)：是否开启云端模式（默认 `false`）。
* `beaconIcon` (String, 必填)：自身在地图上显示的信标图标类型（如 `hiker`, `car`, `tent`, `base`, `drone` 等）。
* `beaconColor` (String, 必填)：自身信标在地图上的高亮色（如 `cyan`, `orange`, `purple`, `rose`, `blue` 等 8 色）。
* `locationMode` (String, 必填)：自身位置广播模式，枚举：`Off`（隐身）、`Fixed`（固定坐标）、`Live`（实时 GPS 动态刷新）。
* `fixedLatitude` / `fixedLongitude` (Float, 可空)：当模式为 `Fixed` 时的固定经纬度。
* `selectedBleDeviceId` (String, 可空)：绑定的随身硬件 BLE MAC 地址。
* `createdAt` / `updatedAt` (Timestamp)：创建与更新时间。

> **级联删除约束**：删除一个 Profile 时，系统**自动清空属于该 Profile 的所有设备、消息记录与传感器历史**。

### 2.2 Device（设备与节点）
在 Mesh 网络中发现或已绑定的节点实体。
* `serial` (String, PK)：硬件唯一序列号（通常为 12 位十六进制 MAC，如 `EZ_A4C13872BF90` 或 `NRF_...`）。
* `profileId` (String, FK)：归属的配置档案 ID。
* `name` (String, 必填)：设备名称/队员昵称。
* `deviceType` (String, 必填)：节点类型枚举：
  * `person`（随身便携终端/队员）
  * `gateway`（车载/固定中继网关）
  * `sensor`（环境监测采集站）
* `hardwareModel` (String)：硬件芯片类型（`esp32` / `nrf54` / `h7608`）。
* `halowMac` (String)：Wi-Fi HaLow 物理网卡地址。
* `publicKey` (String)：用于端到端非对称加密的 Curve25519/Ed25519 公钥。
* `latitude` / `longitude` (Float, 可空)：最新上报的经纬度。
* `batteryVoltage` (Float, 可空)：节点当前电池电压（单位：伏特 V，如 `3.85V`）。
* `lastSeenAt` (Timestamp)：最后一次接收到该节点无线电信标的时间。

### 2.3 Message（通信消息）
离线 Mesh 通信中的文本或控制报文。
* `messageId` (String, PK)：消息唯一 UUID。
* `profileId` (String, FK)：归属的配置档案 ID。
* `peerId` (String, 必填)：对端标识（如果是点对点私聊则为目标设备的 `serial`；如果是公共频道广播则为频道名如 `general`）。
* `direction` (String, 必填)：消息流向：`outbound`（发送）或 `inbound`（接收）。
* `content` (String, 必填)：消息文本正文。
* `status` (String, 必填)：投递状态状态机：
  * `sending`（正在排队通过 BLE 下发硬件发射）
  * `delivered`（收到对端硬件返回的 ACK 确认回执）
  * `failed`（3 次重试均未收到回执，标记为失败）
* `retryCount` (Integer)：自动重试计数器（0 ~ 3 次）。
* `timestamp` (Timestamp)：发送或接收时间。

### 2.4 SensorSample（传感器环境遥测数据）
环境监测节点采集的时序物理指标。
* `id` (Integer, PK 自增)
* `deviceSerial` (String, FK)：上报该数据的设备序列号。
* `timestamp` (Timestamp)：采集时间。
* **15 项核心指标字段**：
  1. `temperature` (Float)：环境温度（单位：`°C`，精度 0.1）
  2. `humidity` (Float)：相对湿度（单位：`%`，精度 0.1）
  3. `pressure` (Float)：大气压强（单位：`hPa`，精度 0.1）
  4. `altitude` (Float)：气压估算海拔高度（单位：`m`，精度 0.1）
  5. `batteryVoltage` (Float)：电池电压（单位：`V`，精度 0.01）
  6. `mcuTemperature` (Float)：主控芯片温度（单位：`°C`，精度 0.1）
  7. `vibrationAverage` (Float)：震动综合指数（精度 0.01）
  8. `accelX` / `accelY` / `accelZ` (Float)：三轴加速度分量（单位：`g`，精度 0.01）
  9. `gyroX` / `gyroY` / `gyroZ` (Float)：三轴角速度（单位：`rad/s`，精度 0.01）
  10. `latitude` / `longitude` (Float)：采集发生时的 GPS 坐标。

### 2.5 ProvisioningRequest（待入网审批请求）
未收编的新设备发起的入网意向记录。
* `id` (String, PK)
* `profileId` (String, FK)
* `deviceSerial` (String)：待入网设备的硬件序列号与 MAC。
* `deviceType` (String)：声明的设备类型。
* `requestedAt` (Timestamp)：请求时间。
* `accepted` (Boolean)：是否已批准入网。

---

## 三、 五大核心功能模块规范

### 3.1 配置档案管理（Profile Management）

#### 功能输入与字段校验规则
* **档案名称**：非空字符串。
* **国家地区码**：下拉单选（`US`, `CA`, `EU`, `GB`, `AU`, `NZ`, `JP`, `KR`, `IN` (硬件射频物理限定为 868MHz 与 915MHz 两大合法频段) 等）。
  * 规则联动：切换国家码后，信道列表（Channel）必须动态更新为该国法律允许的频段。例如：
    * `US`：支持信道 1~30（902~928 MHz）
    * `EU`：支持信道 1~10（863~868 MHz）
* **频宽**：下拉单选（`1 MHz` 或 `2 MHz`）。
* **信道与中心频率**：根据国家码和频宽自动映射（如 US + 1MHz + Ch 27 -> 915.5 MHz）。
* **Mesh ID**：字符串，同组必须相同。
* **Mesh 密码**：选填。若填写，必须在 **8 到 63 个字符** 之间。
* **云端模式开关**：布尔值切换。

#### 事件流与底层触发
1. **新建/保存 Profile**：校验通过后写入 SQLite `profiles` 表，并自动置为当前活跃档案（`activeProfile`）。
2. **切换 Profile**：
   - 立即切断当前随身硬件的 BLE 连接；
   - 切换活跃 Profile ID 并重新加载对应的设备列表与消息；
   - 唤醒 BLE 重新连接新绑定的硬件，并将新 Profile 的射频参数（信道、密码）重新灌入硬件。
3. **删除 Profile**：弹窗强制二次确认；确认后触发级联删除，彻底抹除关联的所有设备、历史对话和传感器数据。

---

### 3.2 离线地图与信标定位（Location & Beacon Tracking）

#### 功能职责与数据驱动
1. **纯离线矢量地图渲染**：
   - 基于本地 Organic Maps 引擎离线数据包渲染等高线、地形、林地、道路。
   - 地图下载检测：检测当前可视区域是否已下载离线包，若未下载，触发下载提示回调。
2. **自身信标状态机（My Beacon Broadcast）**：
   - `Off`：停止广播自身位置。手机仅接收他人信标。
   - `Fixed`：用户在地图上手动拾取固定点。手机将该固定经纬度写入广播包。
   - `Live`：手机 GPS 模块实时采点。当移动超过设定阈值或定时器触发时，自动打包最新 GPS 经纬度并交由硬件发射。
3. **队友信标解析与地图交互（Peer Tracking）**：
   - 当硬件空中接收到队友信标帧时，提取 `serial`, `lat`, `lon`, `batteryVoltage`，更新 SQLite `devices` 表。
   - 测距逻辑：根据当前自身 GPS 与对端经纬度，动态计算大圆直线距离（单位：米或公里）。
   - 离线超时机制：若超过特定时长未收到某节点信标，该节点标记为离线（灰色）。
4. **信标点击触发**：
   - 点击地图上任意节点，呼出该设备的详细信息卡片（显示名称、硬件编号、实时距离、当前电量、最后在线时间）。
   - 提供两个跳转动作：**发起私聊对话**、**查看传感器图表**。

---

### 3.3 离线 Mesh 私有通信与遥测（Mesh Communications & Telemetry）

包含 4 大业务子分栏（Tabs）：

#### 分栏 1: `Channels`（公共广播大厅）
* **通信机理**：明文广播。报文内携带当前 `meshId`，所有工作在相同频段且密码正确的节点均可接收并展示。
* **操作**：
  * 选择频道（默认 `#general`, `#broadcast`）；
  * 输入文本并点击发送；
  * 文本帧通过 BLE 注入硬件发射队列。

#### 分栏 2: `People`（点对点端到端加密私聊）
* **安全机制**：
  * 发信方使用自身 Profile 私钥与收信方设备的公钥（`publicKey`）通过 ECDH 协商出临时共享对称密钥；
  * 对正文进行高强度对称加密后打包为空中报文。任何第三方节点即便截获也无法还原明文。
* **可靠投递与重试机制（Message Delivery Protocol）**：
  1. 消息发出，状态置为 `sending`；
  2. 启动等待 ACK 计时器；
  3. 若在指定时间内收到对端硬件返回的 ACK 回执包，状态变更为 `delivered`；
  4. 若超时未收到 ACK，系统自动重试重发（最多连续重试 3 次，状态标记为 `retrying (1/3)`、`retrying (2/3)`）；
  5. 若 3 次重试后均未收到 ACK，状态最终定格为 `failed`，并允许用户手动点击重试。

#### 分栏 3: `Sensors`（传感器数据与时序看板）
* **数据输入**：来自已入网的 `sensor` 节点广播报文，包含 15 项物理遥测指标。
* **时序与聚合规范**：
  * 本地 SQLite 持续记录带有时间戳的采样记录；
  * 支持 3 档时间范围筛选：`5 min`（5分钟）、`15 min`（15分钟）、`30 min`（30分钟）；
  * 聚合输出：各指标的当前实时最新值、选定时间窗口内的最高值、最低值及趋势图表序列。

#### 分栏 4: `Discovered`（空中发现与入网审批大厅）
* **功能逻辑**：
  * 当未收编的外部硬件发出入网申请广播帧时，被当前硬件捕获并列入 `provisioning_requests` 列表。
  * 用户动作：
    * `Accept（批准入网）`：系统将当前 Profile 的无线电配置（信道、Mesh密码）通过空中加密下发给该设备，并在本地将其转为正式 `Device`。
    * `Ignore（忽略）`：移出待审批列表。

---

### 3.4 硬件蓝牙配网向导（BLE Provisioning Flow）

当手头有全新、未设置参数的硬件盒子（ESP32 或 nRF54）时，通过该向导建立连接：

```
[步骤 1: 扫描]
    手机开启蓝牙扫描，按设备广播前缀过滤：
    - "EZ_*" (ESP32 系列主控)
    - "NRF_*" (Nordic nRF54 系列主控)
    提取设备广播中的信号强度 (RSSI 分贝值) 与 MAC 地址。
       │
       ▼
[步骤 2: 握手认证]
    用户选中目标硬件，手机 BLE 连接其特定的 Provisioning GATT Service；
    通过内置安全密钥 (Proof of Possession: "abcd1234") 进行双向安全握手。
       │
       ▼
[步骤 3: 参数写入与固化]
    手机通过 BLE 通道将当前 Profile 射频配置写入硬件非易失存储 (NVS)：
    - Country Code
    - Channel & Bandwidth
    - Mesh ID & Encryption Passphrase
       │
       ▼
[步骤 4: 重启并激活无线电]
    硬件返回写入成功确认，自动重置无线电射频，正式以新参数启动 Wi-Fi HaLow Mesh 网卡；
    手机将其绑定为当前的随身网关设备。
```

---

### 3.5 系统监控、射频诊断与设置（System & Diagnostic）

#### 1. 硬件实时射频监视器（BLE Debug Monitor）
为专业户外人员提供的硬件底层健康度诊断：
* `HALOW LINK` 状态：
  * `WAITING`：蓝牙已连接，等待无线电芯片完成频段校准；
  * `UP`：无线电芯片握手成功，空中长波链路通畅；
  * `DOWN`：芯片未响应或异常断链。
* `PACKET COUNTERS`：当前会话中成功发送（TX）与成功接收（RX）的数据包总计数。
* `LAST SEEN TIMESTAMP`：最后一次与硬件完成心跳交互的时间。

#### 2. 自身信标个性化配置（Beacon Customization）
* **图标选择（6 项）**：`Hiker`（徒步者）、`Vehicle`（车辆）、`Tent`（营地）、`Base Station`（大本营）、`Drone`（无人机）、`K9`（搜救犬）。
* **色彩选择（8 种标准高辨识色）**：`Cyan`（青蓝）、`Orange`（落日橙）、`Purple`（紫晶）、`Rose`（玫瑰红）、`Blue`（深蓝）、`Lime`（荧光绿）、`Amber`（琥珀黄）、`Teal`（水鸭青）。

#### 3. 本地存储空间与维护（Database Maintenance）
* 显示当前 SQLite 数据库在手机闪存中的物理大小（MB）。
* 统计并展示：已缓存设备总数、已存储聊天记录总数、已记录传感器时序点总数。
* 提供一键操作：`Clear History（清除历史消息与时序采样）`、`Reset Database（彻底重置数据库）`。

---

## 四、 系统状态机与边界异常规则

| 场景 / 边界条件 | 系统底层判定逻辑 | 必须对外输出的状态与事件 |
| :--- | :--- | :--- |
| **手机系统蓝牙关闭** | 监测到系统 BLE 适配器处于 `PoweredOff` 状态 | 输出系统级错误提示：“蓝牙未开启”，禁止发起任何硬件通信。 |
| **随身硬件超出距离断开** | BLE GATT 连接断开（Link Lost） | 顶部状态置为 `DOWN`，信标广播挂起，自动启动后台每 5 秒重连扫描。 |
| **信道法规冲突** | Profile 设置的国家码与当前硬件固件支持的频段不重合 | 校验报错：“当前频段在目标国家受限”，禁止保存 Profile。 |
| **消息发送超时** | 发射后 10 秒内未收到对应 Message ID 的 ACK | 触发自动重试递增；达到 3 次后抛出 `DELIVERY_TIMEOUT` 事件。 |
| **离线地图无缓存** | Organic Maps 引擎未检测到当前视野的离线文件 | 触发地图数据缺失状态，抛出下载离线包事件。 |
| **节点电量极低** | 节点上报的 `batteryVoltage` 低于 3.4V | 节点状态打上低电量标记，提示即将失联。 |
| **网络双向隔离** | 手机同时处于纯离线模式与无蜂窝环境 | 正常运行。屏蔽所有云端重试，绝不阻塞本地 SQLite 与 BLE 队列。 |
