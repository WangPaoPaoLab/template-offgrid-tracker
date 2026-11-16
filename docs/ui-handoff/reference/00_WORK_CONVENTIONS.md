# 🎯 KEEPTEEN UI 变体版本工作约定与设计纲领
> 2026-11-14 需求阅读入口：[用户需求基线](00_USER_REQUIREMENTS.md)。修改 UI 前先核对相应需求 ID，尤其是“设备 / 设置”双 Tab、聊天归属独立产品卡片；新的设计建议不得覆盖既有用户要求。本文较早的目录名与视觉建议以当前用户要求、实际目录和能力核查为准。
> 本文档定义了我们在本项目中的**核心工作隔离准则**与**设计工作流**，旨在确保我们的专属 UI 设计独立、自由演进，同时与 GitHub 上游程序解耦。

---

## ⚠️ 第零铁律：方案先行，严禁擅自直接开工（Strict Human-in-the-Loop）

> [!IMPORTANT]
> **必须在用户明确允许之后再开始工作**：
> 1. **方案先行**：AI Agent 在进行任何实质性的界面重构、代码编写、样式修改或系统调整之前，**必须首先向用户提交详尽、清晰的设计提案或技术方案**。
> 2. **等待批准**：只有在用户明确表示“同意”、“可以”、“执行”后，方可开始着手落地，严禁“未经确认擅自直接开始工作”。
> 3. **高频汇报**：工作过程中必须保持小步快跑，经常性地向用户同步当前进度，接受用户实时监督。

---

## 一、 顶层工作目录严格隔离

为了避免我们的设计思想与进度被 GitHub 原生代码仓中的旧规则（如 `AGENTS.md` 的约束）所限制，整个工程确立 **4 大独立区域**：

```
EdgeZ Offgrid Tracker/ (根目录)
│
├── 📂 software/                  # 【1. GitHub 源码仓库】
│   ├── git 远程: origin (fork) & upstream (官方主仓)
│   ├── 当前分支: variant/custom-ui (只承载最终落地的前端代码改动)
│   └── 职责: 仅用于编译运行与提供原生通讯内核，不在此处存放设计规划
│
├── 📂 KEEPTEEN_UI_Workspace/     # 【2. 我们的设计与工作约定空间】（当前目录）
│   ├── 00_WORK_CONVENTIONS.md   # 本工作约定与设计纲领
│   ├── 01_FUNCTIONAL_SPEC.md    # 核心功能架构与交互文案手册
│   ├── 02_DESIGN_SYSTEM/        # 专属设计系统（色彩、字体、微交互、卡片）
│   └── 03_SCREEN_DESIGNS/       # 各个屏幕的精细化 UI 设计稿说明
│
├── 📂 03_UI_References/         # 【3. 官方设计参考规范库】
│   ├── 01_Apple_Design_Resources/          # 苹果官方 UI Kit 与 Figma 资源
│   ├── 02_Apple_Human_Interface_Guidelines/ # 官方 HIG 规范精读
│   ├── 03_UI_UX_Pro_Max_Skill/             # UI/UX 现代规范库
│   ├── 04_Frontend_Design_Skill/           # 前端设计技能库
│   └── 05_Apple_Home_Design_Reference/     # Apple Home（家庭 App）卡片参考
│
└── 📂 Android_HotReload_Kit/     # 【4. 安卓极速热重载套件】（通用工具资产）
    ├── README_TUTORIAL.md       # 从零到秒级热更新的保姆级实战教程
    ├── start_hotreload.bat      # 双击一键启动脚本（ADB 反代 + Metro）
    └── env_config_reference.md  # E 盘 SDK/NDK/JDK 环境变量与复用指南
```

---

## 二、 代码与设计解耦准则（防合并冲突）

1. **上游内核保护**：
   - 不修改底层 C++ 算法、HaLow 频段分配数学模型、SQLite 数据表主外键结构。
   - 所有 UI 改动聚焦于 `software/app/` 下的页面渲染、组件重构与样式美化（如 `app-styles.ts`、`App.tsx`、`HomeMap.tsx` 等）。
2. **分支独立管理**：
   - 所有的 UI 代码提交严格位于 `variant/custom-ui` 分支。
   - 未来 GitHub 上游发布底层协议或驱动更新时，可通过 `git fetch upstream` 和 `git merge upstream/main` 平滑拉取最新内核，绝不污染我们的 UI 定制成果。

---

## 三、 设计原则：面向现代移动体验重塑
* **告别极客生硬感**：不再使用堆叠式纯文本或单一深灰背景。
* **拥抱 Apple Home 卡片哲学**：状态一目了然、层次分明、触控大面积、纯色分层与轻量微动效，不使用大面积模糊。
* **高可用户外交互**：强烈的视觉层级、大字号无障碍阅读、鲜明的信号与电量指示。
