# 自主进化 Linux Agent 基础设施——工程指标与详细设计完整规格书（v1.1）

> **文档定位**：本文档在原有架构宪章基础上，提供完整、闭环、可直接指导工程编码、自动化测试与多 Agent 协同验收的硬性规格定义，覆盖非功能性 SLO 与配额、三我状态机、数学评价与选举模型、C++20 核心接口与 IPC 协议、防篡改存储 Schema 以及全量模块差异化实施规范。

---

## 第一部分：可量化非功能性指标与资源配额（NFR / SLO / 配额基线）

所有后端自有 C++20 核心组件、工具网关与沙箱运行时必须满足以下硬性量化指标，并纳入 Phase 8 集成验收基线。

### 1.1 延迟与吞吐指标（SLO）

| 指标项 | 目标值 (P95) | 硬上限 (P99 / Timeout) | 测量边界与说明 |
| :--- | :--- | :--- | :--- |
| 本地 IPC（Unix Domain Socket）基础调用延迟 | $\le 2\text{ ms}$ | $\le 10\text{ ms}$ | 不含外部 LLM 网络请求，含帧序列化与 `SO_PEERCRED` 鉴权 |
| 权限校验与策略引擎判定延迟 | $\le 1\text{ ms}$ | $\le 5\text{ ms}$ | 工具网关拦截到返回 `ALLOW/DENY` |
| 系统健康探针采集与评估周期 | $1\text{ s}$（周期） | $\le 200\text{ ms}$（单次耗时） | 覆盖 CPU/内存/磁盘/关键守护进程/`pidfd` |
| Watchdog 生存心跳超时判定 | $3\text{ s}$（连续 $3$ 次丢失） | $5\text{ s}$ 触发降级 | 连续 $3$ 次心跳失败触发本我接管评估 |
| 单个只读查询工具（如 `ps`/`df`/`stat`）执行耗时 | $\le 50\text{ ms}$ | $\le 500\text{ ms}$ | 超时自动发送 `SIGKILL` 给子进程组（PGID） |
| 单个受控变更工具（如服务重载/包查询）执行耗时 | $\le 2\text{ s}$ | $\le 30\text{ s}$ | 软件包安装、编译等长耗时任务必须走异步任务流 |
| 圆桌会议单轮发言/投票超时 | $30\text{ s}$ / Agent | $180\text{ s}$ / 轮次 | 超时未响应视为本轮 `ABSTAIN`（弃权） |
| 审计日志落盘延迟（异步批量刷盘） | $\le 50\text{ ms}$ | $\le 250\text{ ms}$ | 高危（Level 3/4）操作必须同步 `fsync` 落盘后再执行 |
| A/B 系统级回滚耗时（含配置与状态恢复） | $\le 15\text{ s}$ | $\le 45\text{ s}$ | 不含完整内核重启时间；纯配置/服务回滚 $\le 5\text{ s}$ |

### 1.2 资源配额与并发隔离上限（cgroups v2 硬约束）

| 资源维度 | 守护进程本体 (`agent-core`) | 单个任务沙箱 (`sandbox-worker`) | 超我研发工作区 (`superego-dev`) | 超限处置动作 |
| :--- | :--- | :--- | :--- | :--- |
| CPU 配额 (`cpu.max`) | $0.5\text{ core}$ 保障，$1.0\text{ core}$ 上限 | $1.0\text{ core}$ 上限 | $2.0\text{ core}$ 上限（闲时弹性） | 内核 CFS 节流，不触发 OOM |
| 内存硬上限 (`memory.max`) | $256\text{ MB}$ | $512\text{ MB}$（编译任务可申请至 $2\text{ GB}$） | $4\text{ GB}$ | 仅 Kill 对应沙箱 `cgroup` 内进程 |
| 最大进程/线程数 (`pids.max`) | $64$ | $128$ | $512$ | 拒绝 `fork/clone`，防 Fork 炸弹 |
| 临时目录磁盘配额 (`/tmp` / overlay) | $100\text{ MB}$ | $1\text{ GB}$ | $10\text{ GB}$ | 返回 `ENOSPC`，任务标记为配额失败 |
| 日志与审计数据库总容量上限 | 单文件 $50\text{ MB}$，保留最近 $20$ 份轮转；审计库上限 $5\text{ GB}$ | 合并入统一日志流 | 独立保留最近 $7$ 天产物 | 达到 $85\%$ 告警，$95\%$ 归档压缩旧数据 |
| 并发执行上限 | 全局最大活跃 Agent 数：$8$；待执行队列上限：$64$ | 单任务并发子进程 $\le 16$ | 最大并行构建任务：$2$ | 队列满返回 `ERR_QUEUE_FULL`（背压） |
| 外部 LLM API 预算熔断 | 单任务最大 Token 消耗：$120\text{k}$；单日全局费用熔断阈值可配置 | — | 单次研发迭代最大重试：$3$ 轮 | 触发预算熔断，转入人工确认 |

### 1.3 熔断器与重试退避策略

* **服务崩溃重启退避**：采用指数退避 + 随机抖动（Jitter）：
  $$t_n = \min\left(1\text{ s} \times 2^{n-1} + \text{rand}(0, 500\text{ ms}),\; 30\text{ s}\right)$$
  若 $5\text{ 分钟}$ 内重启超过 $5$ 次，判定为不可自愈故障，触发本我隔离。
* **外部 API / 工具熔断器（三态模型）**：
  * `CLOSED`（正常）：连续失败次数 $\ge 5$ 次，或滑动窗口（$60\text{ s}$）失败率 $> 50\%$，切换至 `OPEN`。
  * `OPEN`（熔断）：持续 $30\text{ s}$，期间直接快速失败（Fail-Fast），不发起实际调用。
  * `HALF_OPEN`（半开）：放行最多 $2$ 个探测请求；全部成功则恢复 `CLOSED`，任一失败则重新进入 `OPEN` 且冷却时间翻倍（上限 $5\text{ 分钟}$）。

---

## 第二部分：三我状态机（本我 / 真我 / 超我）精确转移矩阵

系统在任意时刻处于且仅处于以下 $5$ 个宏观运行态之一，状态切换必须写入不可篡改状态日志，并由 `pidfd` 与 systemd/init 看门狗联动。

### 2.1 状态定义

1. **`S0_ID_SURVIVAL`（本我·生存与救援态）**：最小可信集运行，禁用所有 AI 决策写入与超我研发通道，仅保留救援 shell、基础网络恢复、只读诊断与 A/B 回滚能力。
2. **`S1_EGO_NORMAL`（真我·稳态运行态）**：日常生产态，允许执行已批准的常规运维与受控变更，超我通道处于只读观察或受限沙箱模式。
3. **`S2_EGO_DEGRADED`（真我·降级限流态）**：检测到非致命异常（如负载过高、某外部 Provider 不可用、非关键服务异常），自动收紧权限至 Level 0\~2，暂停非必要自动化。
4. **`S3_SUPEREGO_DEV`（超我·研发与演进态）**：在隔离分支/沙箱中进行代码修改、测试验证与提案生成，严禁直接接触真我生产配置与本我分区。
5. **`S4_ROLLBACK_RECOVERY`（恢复态）**：执行快照回滚、配置还原与一致性校验的过渡态。

### 2.2 状态转移矩阵

| 当前状态 | 触发事件 (`Event`) | 守卫条件 (`Guard`) | 目标状态 | 强制执行动作 (`Action`) |
| :--- | :--- | :--- | :--- | :--- |
| 任意状态 | `EVT_CRITICAL_FAULT`（内核/根文件系统异常、连续 $3$ 次 Watchdog 超时、安全完整性校验失败） | 无条件最高优先级 | `S0_ID_SURVIVAL` | 冻结所有超我/真我写权限，向沙箱发送 `SIGKILL`，锁定为最小可信服务集 |
| `S0_ID_SURVIVAL` | `EVT_RECOVERY_REQUEST` | 救援诊断完成且有可用健康快照 | `S4_ROLLBACK_RECOVERY` | 挂载只读镜像并校验 SHA-256 签名 |
| `S4_ROLLBACK_RECOVERY` | `EVT_HEALTH_VERIFIED` | 连续 $3$ 次全量健康探针通过（间隔 $2\text{ s}$） | `S1_EGO_NORMAL` | 解除紧急锁定，记录恢复审计事件 |
| `S4_ROLLBACK_RECOVERY` | `EVT_ROLLBACK_FAILED` | 重试次数 $\ge 2$ | `S0_ID_SURVIVAL` | 保持生存态并发送高优告警等待人工介入 |
| `S1_EGO_NORMAL` | `EVT_RESOURCE_PRESSURE`（内存 $>85\%$ 或负载超阈值或错误率上升） | 未达到致命故障阈值 | `S2_EGO_DEGRADED` | 禁用 Level 3/4 变更，关闭非核心子任务 |
| `S2_EGO_DEGRADED` | `EVT_METRICS_NORMALIZED` | 指标恢复正常且持续稳定 $\ge 60\text{ s}$ | `S1_EGO_NORMAL` | 逐步恢复常规权限等级 |
| `S1_EGO_NORMAL` | `EVT_ENTER_EVOLUTION` | 圆桌会议批准研发提案且系统负载 $<60\%$ | `S3_SUPEREGO_DEV` | 创建独立 git worktree 与 `cgroups v2` 研发沙箱 |
| `S3_SUPEREGO_DEV` | `EVT_EVOLUTION_COMPLETE` 或 `EVT_EVOLUTION_ABORT` | 沙箱已销毁且变更已合并/丢弃 | `S1_EGO_NORMAL` | 清理临时构建产物，更新 Agent 信誉与测试档案 |

---

## 第三部分：信誉评分数学模型与选举决胜算法

为消除主观模糊性，统一所有 Agent 的能力评估、样本冷启动及 Leader/Inspector 选举算法。

### 3.1 口径统一：样本冷启动规则

* **全局有效样本下限**：统一规定有效任务样本数 $N_{\text{valid}} < 20$ 时处于**冷启动观察期**（仅可参与一般讨论与常规执行，不可担任负责人）。
* **选举资格阈值**：
  * 参选 **Domain Leader（专业领域负责人）**：该领域有效样本数 $N_d \ge 30$，且近 $30$ 次任务错误率 $\le 10\%$。
  * 参选 **Chief Leader / Integration Leader**：总有效样本数 $N_{\text{total}} \ge 50$，跨领域（至少 $2$ 个领域）有效样本各 $\ge 15$，且近 $50$ 次任务错误率 $\le 8\%$。
  * 担任 **Inspector（独立审查官）**：有效样本数 $N_{\text{valid}} \ge 20$，且近 $60$ 天无未消解的高危误判记录。

### 3.2 单次任务得分与难度加权公式

每次任务 $i$ 完成后，由 Inspector 与客观自动化验证器共同给出基础质量分 $Q_i \in [0, 100]$，结合任务难度系数 $D_i$ 与时间衰减权重 $w_i$ 计算：

1. **任务难度系数** $D_i$：
   * `L1`（简单查询/单步低风险）：$D_i = 0.8$
   * `L2`（常规诊断/单模块变更）：$D_i = 1.0$
   * `L3`（跨模块排障/内核与安全敏感变更）：$D_i = 1.3$
   * `L4`（系统级故障救援/高并发架构重构）：$D_i = 1.6$
2. **时间衰减权重** $w_i$（半衰期 $T_{1/2} = 30\text{ 天}$）：
   $$w_i = \exp\left(-\frac{\ln 2}{30} \cdot \Delta t_i\right)$$
   其中 $\Delta t_i$ 为任务完成距今的天数。
3. **领域加权综合分** $S_d$：
   $$S_d = \frac{\sum_{i=1}^{N_d} w_i \cdot D_i \cdot Q_i}{\sum_{i=1}^{N_d} w_i \cdot D_i} \times \left(1 - P_{\text{penalty}}\right)$$
   其中 $P_{\text{penalty}}$ 为安全红线惩罚项：每次触发安全策略违规扣 $0.15$，造成需要回滚的故障扣 $0.25$（累计上限 $0.80$）。

### 3.3 错误率计算与 10% 硬性熔断规则

* **滑动窗口错误率** $E_{30}$：统计最近 $30$ 个已结案任务中，判定为 `FAILED_BY_AGENT`（排除外部网络断连、用户主动取消等客观外部因素）的比例：
  $$E_{30} = \frac{N_{\text{agent\_fault}, 30}}{30}$$
* **硬性熔断**：一旦 $E_{30} > 0.10$（即最近 $30$ 次中出现 $\ge 4$ 次自身失误）：
  1. 若当前担任 Leader 或 Inspector，**立即自动剥夺职务**并触发重新选举；
  2. 进入为期 $10$ 个低风险任务的“信誉修复期”，期间仅允许执行 `L1/L2` 级只读或受严格审查任务，直至 $E_{30} \le 0.10$ 方可恢复参选资格。

### 3.4 选举排序与决胜（Tie-Breaker）规则

候选人按领域综合分 $S_d$ 降序排列。当最高分与次高分分差 $|\Delta S_d| < 2.0$ 分时，视为“能力相近”，依次按以下规则决胜：

1. **高难度任务胜率优先**：比较近 $30$ 天 `L3 + L4` 任务的成功率；
2. **安全零违规优先**：比较近 $60$ 天安全违规与回滚次数（少者胜）；
3. **多样性与反垄断轮换**：若仍无法区分，优先选择最近 $3$ 个周期内未担任过该职务的候选 Agent（避免单一模型家族长期垄断）。

---

## 第四部分：核心通信协议、统一错误码与 C++20 接口契约

### 4.1 本地 IPC 帧协议（Unix Domain Socket）

所有本地模块间通信统一采用 **Length-Prefixed Binary Frame**（防止粘包、支持零拷贝切片与校验）：

```text
+-----------------------+----------------+----------------+------------------------+
| Magic (4B)            | Version (1B)   | MsgType (1B)   | PayloadLen (4B, BE)    |
| 0x41 0x4C 0x41 0x47   | 0x01           | REQ/RESP/EVENT | uint32 (max 4MB)       |
+-----------------------+----------------+----------------+------------------------+
| TraceID (16B UUIDv7)                   | Flags (2B: SYNC/ASYNC/HIGH_PRIO/STREAM)|
+----------------------------------------+----------------------------------------+
| Payload (UTF-8 Canonical JSON or Protobuf bytes)                                |
+---------------------------------------------------------------------------------+
| CRC32C Checksum (4B, Big-Endian)                                                |
+---------------------------------------------------------------------------------+
```

* **套接字安全约束**：绑定于 `/run/ala-core/ipc.sock`，权限 `0660`，属主 `root:ala-runtime`；连接建立时强制通过 `SO_PEERCRED` 获取对端 `uid`、`gid` 与 `pid`，并立即调用 `pidfd_open(pid, 0)` 锁定进程描述符以消除 PID 复用竞态。

### 4.2 统一错误码注册表（Error Code Registry）

| 错误码段 | 枚举常量 | 数值 | 含义说明 | 是否可自动重试 |
| :--- | :--- | :--- | :--- | :--- |
| **1000-1999**（协议与路由） | `ERR_IPC_FRAME_INVALID` | `1001` | Magic/Version 错误或 CRC32C 校验失败 | 否 |
| | `ERR_IPC_PAYLOAD_TOO_LARGE` | `1002` | 单帧负载超过 $4\text{ MB}$ 硬上限 | 否 |
| | `ERR_PEER_AUTH_FAILED` | `1003` | `SO_PEERCRED` / `pidfd` 身份校验未通过 | 否 |
| | `ERR_QUEUE_FULL` | `1004` | 全局任务队列已满（$\ge 64$），触发背压 | 是（指数退避） |
| **2000-2999**（权限与治理） | `ERR_PERMISSION_DENIED` | `2001` | 当前状态或 Agent 权限等级不足 | 否 |
| | `ERR_PATH_OUT_OF_SCOPE` | `2002` | 访问路径命中受保护黑名单或超出白名单 | 否 |
| | `ERR_INSPECTOR_REJECTED` | `2003` | 高危操作未通过独立审查官或人类审批 | 否 |
| | `ERR_STATE_LOCKED_SURVIVAL` | `2004` | 系统处于 `S0_ID_SURVIVAL` 态，拒绝写操作 | 否 |
| **3000-3999**（沙箱与执行） | `ERR_SANDBOX_TIMEOUT` | `3001` | 子进程组执行超时并已被 `SIGKILL` 终止 | 视幂等性而定 |
| | `ERR_SANDBOX_OOM_KILLED` | `3002` | 沙箱触发 `cgroups v2` 内存硬上限 | 否（需调优或拆分） |
| | `ERR_SANDBOX_DISK_QUOTA` | `3003` | 临时工作区磁盘配额耗尽 (`ENOSPC`) | 否 |
| | `ERR_TOOL_EXEC_NONZERO` | `3004` | 目标命令返回非零退出码 | 视业务而定 |
| **4000-4999**（外部模型与网络） | `ERR_LLM_CIRCUIT_OPEN` | `4001` | 对应模型 Provider 处于熔断器 `OPEN` 态 | 是（切换备用模型） |
| | `ERR_LLM_BUDGET_EXCEEDED` | `4002` | 单任务 Token（$120\text{k}$）或日费用超限 | 否 |
| | `ERR_LLM_SCHEMA_MISMATCH` | `4003` | 模型输出不符合声明的 JSON Schema | 是（最多 $2$ 次） |
| **5000-5999**（状态与存储） | `ERR_AUDIT_FSYNC_FAILED` | `5001` | 高危操作前审计日志落盘失败（阻断执行） | 否（触发告警） |
| | `ERR_SNAPSHOT_HASH_MISMATCH` | `5002` | 回滚镜像 SHA-256 校验失败，禁止恢复 | 否 |

### 4.3 核心 C++20 契约接口定义（`ala_core_contracts.hpp`）

```cpp
#pragma once
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>
#include <span>

namespace ala::core {

enum class SystemState : uint8_t {
    S0_ID_SURVIVAL = 0,
    S1_EGO_NORMAL = 1,
    S2_EGO_DEGRADED = 2,
    S3_SUPEREGO_DEV = 3,
    S4_ROLLBACK_RECOVERY = 4
};

enum class RiskLevel : uint8_t {
    L0_READONLY = 0,
    L1_SAFE_LOCAL = 1,
    L2_CONTROLLED_CHANGE = 2,
    L3_PRIVILEGED_SYSTEM = 3,
    L4_CRITICAL_INFRA = 4
};

struct Error {
    uint32_t code;
    std::string message;
    std::string trace_id;
};

struct PeerIdentity {
    uint32_t uid;
    uint32_t gid;
    int32_t pid;
    int pidfd; // 由内核持有的有效进程文件描述符
};

struct ToolInvocationRequest {
    std::string trace_id;
    std::string agent_id;
    std::string tool_name;
    RiskLevel declared_risk;
    std::string args_json;
    uint32_t timeout_ms;
    std::string idempotency_key;
};

struct ToolInvocationResult {
    int32_t exit_code;
    std::string stdout_data;
    std::string stderr_data;
    uint64_t duration_ms;
    bool truncated;
};

// 工具网关核心接口：强制执行前置审计、权限校验与 cgroups v2 沙箱隔离
class IToolGateway {
public:
    virtual ~IToolGateway() = default;
    [[nodiscard]] virtual auto invoke(
        const PeerIdentity& caller,
        const ToolInvocationRequest& req
    ) -> std::expected<ToolInvocationResult, Error> = 0;
};

// 状态机控制器接口：所有状态迁移必须原子持久化
class IStateMachine {
public:
    virtual ~IStateMachine() = default;
    [[nodiscard]] virtual auto current_state() const noexcept -> SystemState = 0;
    [[nodiscard]] virtual auto transition(
        std::string_view event_name,
        std::string_view reason
    ) -> std::expected<SystemState, Error> = 0;
};

} // namespace ala::core
```

---

## 第五部分：存储与防篡改审计 Schema

审计数据库（`/var/lib/ala-audit/audit.db`）采用 **SQLite 3（WAL 模式）**，由独立高权限守护进程 `ala-auditd` 独占写入权限（文件权限 `0600`，属主 `root:root`），任何业务 Agent 与沙箱进程均无直接文件系统写权限，仅能通过 IPC 发送只追加（Append-Only）事件。

### 5.1 防篡改哈希链机制

每条审计记录引入 `prev_hash` 与 `record_hash` 字段，构成密码学单向链表：
$$\text{record\_hash}_k = \text{HMAC-SHA256}\Big(K_{\text{tpm/root}},\; \text{seq}_k \parallel \text{timestamp}_k \parallel \text{trace\_id}_k \parallel \text{payload\_hash}_k \parallel \text{prev\_hash}_{k-1}\Big)$$

* 每次启动及每小时定时任务执行链完整性扫描，若发现任意记录被篡改或删除，立即触发 `EVT_CRITICAL_FAULT` 切入 `S0_ID_SURVIVAL`。

### 5.2 核心数据库表结构（DDL）

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA foreign_keys = ON;

-- 1. 不可篡改安全与操作审计主表
CREATE TABLE IF NOT EXISTS audit_events (
    seq_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    event_uuid      TEXT NOT NULL UNIQUE,
    trace_id        TEXT NOT NULL,
    timestamp_ns    INTEGER NOT NULL,
    system_state    TEXT NOT NULL,
    agent_id        TEXT NOT NULL,
    peer_uid        INTEGER NOT NULL,
    peer_pid        INTEGER NOT NULL,
    action_type     TEXT NOT NULL, -- 'TOOL_CALL' | 'STATE_CHANGE' | 'VOTE' | 'ROLLBACK'
    risk_level      INTEGER NOT NULL,
    target_resource TEXT NOT NULL,
    request_digest  TEXT NOT NULL,
    decision        TEXT NOT NULL, -- 'ALLOW' | 'DENY' | 'EXEC_OK' | 'EXEC_FAIL'
    error_code      INTEGER NOT NULL DEFAULT 0,
    prev_hash       TEXT NOT NULL,
    record_hash     TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_audit_trace ON audit_events(trace_id);
CREATE INDEX IF NOT EXISTS idx_audit_agent_time ON audit_events(agent_id, timestamp_ns);

-- 2. Agent 身份与滑动窗口信誉档案表
CREATE TABLE IF NOT EXISTS agent_reputation (
    agent_id            TEXT PRIMARY KEY,
    provider            TEXT NOT NULL,
    model_name          TEXT NOT NULL,
    role                TEXT NOT NULL, -- 'MEMBER' | 'DOMAIN_LEADER' | 'CHIEF_LEADER' | 'INSPECTOR'
    domain              TEXT NOT NULL,
    valid_samples       INTEGER NOT NULL DEFAULT 0,
    weighted_score      REAL NOT NULL DEFAULT 0.0,
    recent_30_faults    INTEGER NOT NULL DEFAULT 0,
    probation_remaining INTEGER NOT NULL DEFAULT 0,
    updated_at_ns       INTEGER NOT NULL
);

-- 3. 任务评价与难度记录明细表
CREATE TABLE IF NOT EXISTS task_evaluations (
    task_id         TEXT PRIMARY KEY,
    agent_id        TEXT NOT NULL REFERENCES agent_reputation(agent_id),
    inspector_id    TEXT NOT NULL,
    domain          TEXT NOT NULL,
    difficulty      TEXT NOT NULL, -- 'L1' | 'L2' | 'L3' | 'L4'
    quality_score   REAL NOT NULL CHECK(quality_score >= 0.0 AND quality_score <= 100.0),
    is_agent_fault  INTEGER NOT NULL DEFAULT 0 CHECK(is_agent_fault IN (0, 1)),
    penalty_delta   REAL NOT NULL DEFAULT 0.0,
    completed_at_ns INTEGER NOT NULL
);
```

---

## 第六部分：核心工具与模块差异化技术规范（替换通用模板）

原第二十一章的通用 12 步模板收敛为**所有工具共享的基础运行时基线**（由 `IToolGateway` 统一在底层框架中强制执行 `SO_PEERCRED`、路径规范化、预写审计日志、超时强杀与结构化返回）。以下为各核心工具模块的**专属差异化契约**：

### 6.1 进程与资源观测模块 (`sys.proc.*`)

* **工具清单**：`sys.proc.list`、`sys.proc.inspect`、`sys.proc.tree`、`sys.proc.cgroup_stats`
* **风险等级**：`L0_READONLY`
* **底层实现契约**：直接解析 `/proc/[pid]/stat`、`/proc/[pid]/status`、`/proc/[pid]/cmdline`（以 `\0` 分隔还原参数，禁止拼接 `ps` shell 命令）；对于关键守护进程，优先通过 `pidfd_send_signal(pidfd, 0, nullptr, 0)` 检测存活状态，避免传统 `kill(pid, 0)` 的 PID 复用误判。
* **敏感信息脱敏**：读取 `/proc/[pid]/environ` 时强制过滤包含 `KEY`、`SECRET`、`TOKEN`、`PASSWORD`、`PASSWD`、`CREDENTIAL` 的环境变量值，统一替换为 `"[REDACTED]"`。
* **输出上限**：单次列表最多返回 $500$ 个进程条目，支持按 CPU/RSS 内存排序与截断标记 `truncated: true`。

### 6.2 安全文件读写与补丁模块 (`sys.fs.*`)

* **工具清单**：`sys.fs.read_range`、`sys.fs.stat`、`sys.fs.atomic_write`、`sys.fs.apply_unified_diff`
* **风险等级**：读取为 `L0_READONLY`；写入为 `L2_CONTROLLED_CHANGE` 或 `L3_PRIVILEGED_SYSTEM`（取决于目标路径敏感级别）。
* **路径防逃逸与原子写入算法**：
  1. 使用 `openat2()` 配合 `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS` 标志解析路径，彻底杜绝符号链接跳转（Symlink Attack）与 `../` 目录穿越。
  2. 写入前强制校验目标文件当前 `SHA-256` 摘要与请求参数 `expected_base_sha256` 一致（乐观并发锁，防止覆盖人工或并发修改）。
  3. 写入流程：在同挂载点目录创建临时文件 `.tmp.<uuid>` $\rightarrow$ 写入内容并复制原文件 `mode`/`uid`/`gid`/SELinux xattr $\rightarrow$ 调用 `fsync(fd)` $\rightarrow$ `renameat2()` 原子替换 $\rightarrow$ 对父目录执行 `fsync(dir_fd)`。
* **硬编码禁止路径（内核级策略拒止）**：`/boot/**`、`/etc/shadow`、`/etc/gshadow`、`/etc/sudoers*`、`/var/lib/ala-audit/**`、`/sys/firmware/**`。

### 6.3 服务与守护进程管理模块 (`sys.svc.*`)

* **工具清单**：`sys.svc.status`、`sys.svc.reload`、`sys.svc.restart`、`sys.svc.journal_tail`
* **风险等级**：状态与日志查询为 `L0_READONLY`；`reload`/`restart` 为 `L2` 或 `L3`。
* **跨 init 系统适配层**：
  * **systemd 环境**（Debian/Ubuntu/主流发行版）：通过 D-Bus (`org.freedesktop.systemd1`) 调用 `GetUnit` / `ReloadOrRestartUnit`，禁止直接执行未转义的 `systemctl` 拼接命令。
  * **SysVinit / BusyBox init 环境**（Buildroot 嵌入式场景）：通过受控白名单映射 `/etc/init.d/<service>`（仅允许匹配正则 `^[a-zA-Z0-9._-]+$` 的服务名），使用 `execve` 传入固定参数 `status`/`reload`/`restart`。
* **自我保护红线**：严禁对 `ala-core`、`ala-auditd`、`dbus`、`systemd-journald`、`sshd` 执行 `stop` 或 `disable` 操作（仅允许受控 `reload`）。

### 6.4 软件包管理适配层 (`sys.pkg.*`)

* **工具清单**：`sys.pkg.query`、`sys.pkg.install`、`sys.pkg.upgrade`、`sys.pkg.verify_integrity`
* **风险等级**：查询与校验为 `L0`；安装与升级为 `L3_PRIVILEGED_SYSTEM`（必须经圆桌会议或人类审批通过，并在执行前自动创建配置快照）。
* **发行版后端适配**：
  * **Debian/APT 后端**：强制设置环境变量 `DEBIAN_FRONTEND=noninteractive`，调用 `/usr/bin/apt-get` 时固定传入 `-o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold"`（保留现有配置文件，防止交互阻塞或配置覆盖），包名严格校验正则 `^[a-z0-9][a-z0-9+.-]+(=[a-zA-Z0-9.+:~-]+)?$`。
  * **Buildroot / 无在线包管理器环境**：自动探测 `/etc/os-release` 与包管理器存在性；若系统为只读根文件系统或 Buildroot 裁剪环境，`sys.pkg.install` 明确返回 `ERR_PERMISSION_DENIED`（附提示：需通过 `S3_SUPEREGO_DEV` 构建新固件镜像并走 A/B 升级流程）。

### 6.5 网络诊断与防火墙受控模块 (`sys.net.*`)

* **工具清单**：`sys.net.interfaces`、`sys.net.sockets`、`sys.net.probe`、`sys.net.nft_rule_stage`
* **底层实现契约**：
  * 接口与套接字状态优先通过 `Netlink` (`NETLINK_ROUTE` / `NETLINK_SOCK_DIAG`) 直接读取，避免解析 `ifconfig`/`netstat` 文本输出。
  * 防火墙变更仅支持 `nftables` 原子事务（`nft -c -f` 语法预检 $\rightarrow$ 加载规则 $\rightarrow$ 启动 $60\text{ s}$ 死人开关定时器 `Dead-Man's Switch`）；若 $60\text{ s}$ 内控制通道未收到确认心跳，自动回滚至变更前规则集，彻底防止远程断网失联。

### 6.6 桌面环境自动化兼容矩阵 (`ui.desktop.*`)

为避免跨桌面与无头服务器环境下的不确定性，明确第一阶段环境探测与降级契约：

| 运行环境特征 | 探测方式 | 支持能力边界 | 不支持时的行为契约 |
| :--- | :--- | :--- | :--- |
| **无头服务器 / 纯 CLI**（无 `WAYLAND_DISPLAY` 且无 `DISPLAY`） | 环境变量 + D-Bus session 探测 | 仅支持 `org.freedesktop.*` 系统级 D-Bus 调用 | 所有 GUI 键鼠/窗口工具立即返回 `ERR_PERMISSION_DENIED`（附 `ENV_HEADLESS_NO_DISPLAY`） |
| **Wayland 桌面**（GNOME / KDE） | `XDG_SESSION_TYPE=wayland` | 优先使用 AT-SPI2 无障碍树 (`org.a11y.Bus`) 与 XDG Desktop Portal (`org.freedesktop.portal.*`) | 禁止绕过 Portal 直接注入底层输入；截图/录屏需用户 Portal 授权 |
| **X11 桌面**（轻量级桌面 / Xfce） | `XDG_SESSION_TYPE=x11` | 支持 AT-SPI2 + 受控窗口焦点与输入事件 | 操作前校验目标窗口 `WM_CLASS` 在白名单内，严禁操作密码管理器与终端提权弹窗 |