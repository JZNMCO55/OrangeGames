# dogfood NOTES — 2026-07-07（12 态形变 + stranding fix）

夜间自主 session 落地（对齐 `art/refs/slime-12-states-reference.png` 12 态参考图）。以下是**你早上要真机复看 / 调的项**——我能自主验的已验，主观手感 / 转场态截图蒙不中的留你。

## 一键看 reel

```
build/bin/Debug/spike-01-blob.exe --demo
```

`--demo` = 启动即进 12 态 attract 演示（脚本驱动，忽略键盘，隐藏 ImGui 面板出纯史莱姆黑底）。
循环轮一遍：Idle → Crouch → 起跳序列(Launch/Rising/Falling/Landing+溅射/回弹) → Rolling →
Squeezed → Sliding → Split → Merge。约 18s 一圈。

## 我已自主验证（截图对图）

- **Idle 静止**：圆润矮 dome，神似参考 #1 ✅
- **Crouch 下压**：宽扁对称，神似 #2 ✅
- **Rolling 滚动**：倾斜椭圆 + 翻滚旋转（眼睛保持正向不随体转），神似 #8 ✅
- 渲染线：gel 通透 / 双黄眼 / 金斑 / rim glow / 落地不穿地，全在。
- `blob_collision` ctest 1/1 绿（stranding 门控未破坏正常碰撞）。

## 要你真机复看的转场态（我盲采样没蒙中，但代码是既存已测 juice 新接 demo）

看 `--demo` reel 时重点确认这几个是否神似参考图：

1. **3.拉伸 / Launch**：起跳前应纵向拉成高瘦柱（{0.70,1.55}）。
2. **6.落地冲击 + 溅射**：落地应压扁成煎饼（{1.60,0.42}）+ 底部溅出 ~5 个水滴（`SpawnSplatter` 既存）。
3. **7.回弹**：落地后欠阻尼超调弹起（涌现，不特设）。
4. **11.分裂 / 12.合并**：大 droplet 沿横向飞出 / 靠回 + 细颈 smin 拉断 / 融合（`TriggerSplit`/
   `TriggerMerge` 既存，本轮首次接进 demo 触发）。若两 blob 没正常拉断 / 融合，是 juice 脚本
   参数问题（`UpdateJuiceScript`），非结构问题。

## 待你调参 / defer 的（拿参考图 live 调最高效）

- **5.下落水滴 taper（Tier B，defer）**：参考 #5 是明显 teardrop（上尖下圆）。本轮 Falling 只改成
  窄高椭圆 {0.82,1.28} 近似，**没做真 taper**——真 teardrop 需 `Blob.h` 逐顶点 taper（对
  `BuildRestGeometry` 的 base 按 y 加锥度），属中风险核心 sim 改动，留你拿参考图 live 调。
  态10 滑行拖尾同理（现只横拉无 taper / 拖尾）。
- **9.受挤压 / Squeezed 形状**：{1.34,0.66} 是初值猜测，参考 #9 偏宽扁 lumpy；lumpy（不规则凸起）
  需逐顶点扰动，未做。手感/形状可调 `MotionShapeTarget` 查表。
- **stranding leash 阈值**：`BlobParams.strandingLeash` 默认 0.9。**这是 BUG-2026-07-05 的修复**——
  但真机验证要你在平台关卡里把 control point 掉到平台下方，看 blob 是否 ooze 回 cp 而非永久卡
  平台顶（`--demo` 平地跳跃触发不到该场景）。若正常跳跃出现 blob 穿台 = leash 太小，调大；若还
  stranding = 调小或检查"cp 在质心下方"判据。修法 = 质心↔cp 偏离超 leash **且** cp 在质心下方时
  关碰撞（见 `Blob.h` Step 门控）。

## defer 到后续 session（非今晚 scope）

- **Crouch/Rolling/Squeezed 的 emergent 玩法输入接线**：本轮这 3 态只经 forced-state 在 demo 摆姿，
  没接真实输入（蓄力键 / 窄缝检测 / 滚动检测）。真玩法触发留后续。
- **main.cpp ↔ SlimeGameModule.cpp dedup**：两文件形变 / juice 逻辑逐行拷贝（recon 已标维护坑）。
  本轮形变本体已 port 保 SlimeEditor 外观一致，但 12 态 demo 脚本只在 standalone（编辑器不可达）。
  彻底 dedup（抽共享 header）留专门 session。
