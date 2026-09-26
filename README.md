# prontom

通用 Malody `.mc` 轨道映射工具。

当前版本：`2.1.2`

## 用法

```text
prontom.exe input.mc target_columns [lambda] [source_columns]
prontom.exe --version
```

`lambda` 省略时默认为 `0`。`lambda` 必须位于 `[0,1]`：

- `lambda=0`：严格使用旧的逐 note 映射路径。
- `lambda>0`：使用事件级多对多键型转换。

如果不提供 `source_columns`，程序读取 `meta.mode_ext.column`。

输出文件会写成：

```text
input_to{target_columns}K.mc
```

## 映射规则

- 使用整数矩阵和确定性平衡周期，不使用浮点权重。
- `4→8` 无长条时保持原 `Pro4to8.py` 的逐音符结果。
- 等权目标轨道使用严格的循环平衡序列，例如 `0,1,2,0,1,2...`。
- 长条先按基础映射确定目标轨道，并在 `[beat,endbeat)` 内冻结源轨道和目标轨道。
- 长条冻结后，剩余轨道按 `n-k→m-k` 重新映射。
- 长条发生冲突时，不丢弃后来的长条；前一根长条的 `endbeat` 截短到冲突起点前 `1/4 beat`。
- 如果截短后不再构成正长度长条，则删除前一根的 `endbeat`，保留其起始音符。
- 冻结状态会继承各源轨道已经消耗的周期位置，不会在每次 `n-k→m-k` 重建时重置。
- 普通音符、长条头和长条尾统一遵守 `xmin * m / n` 的最小间隔约束。
- 同一 beat 的目标列会确定性避开已占用列。
- 当矩阵允许列全部无法满足最小间隔时，确定性去尾后续 note，并在日志中报告数量。
- 事件级路径以同一精确 beat 的键型作为转换事件，并使用原谱 `xmin` 作为间隔约束。
- 事件级路径使用确定性平衡周期控制增删 note，不使用随机。
- 事件级路径按中心、跨度、间隙和连续性选择目标键型。

## 构建

使用 MinGW-w64：

```text
g++ -std=c++17 -O2 -municode -s -o prontom.exe prontom.cpp
```

`json.hpp` 是单头文件依赖，已随仓库保存。
