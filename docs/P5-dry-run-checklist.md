# P5 Dry Run Checklist

## End-to-End Test (2-device pre-workshop验证)

| # | Test | Action | Expected |
|---|------|--------|----------|
| 0 | Flash ritual (B′ 语义选岛) | 跑 loiter-flash skill → 打一段能体现性格的文本（如“fiery and restless”）→ 编译烧录 | `/flash/profile` 同步语义选岛（不超 ~8s）→ 返回 profile_id；后续揭晓屏落在语义匹配的岛（fiery→EMBER / calm→GROVE / dreamy→MIST …） |
| 1 | Boot + WiFi | New device (no NVS) power on | Pride rainbow WiFi → connect → Welcome screen |
| 2 | Phase 1 full | Name → dress-up → ENTER | 揭晓屏显 server 选的岛（烧录文本语义定，不再有 quiz）+ 文艺双语 reason；大屏小人落对应岛 |
| 2b | Reset 同岛 | 揭晓后 Reset → 重输名重进 | 同 profile_id → **同岛、同 reason**（岛创建时定一次、持久化不变） |
| 3 | Phase 2 HI | A sends HI to B → B accepts | Both get new color + big screen rainbow arc |
| 4 | Phase 2 M-mode | Press M → tilt walk → DEL exit | Big screen character moves + banner shows/hides |
| 5 | Phase 2 shake=jump | Normal mode shake | M5 跳跃屏显**真实 N/need 人数**（非本地假倒计时）+ 大屏小人 bounce + 大屏"N JUMPING"胶囊；≥5 人 10s 内 → 全屏彩虹爆炸 |
| 6 | Phase 3 reading | Press B → Phase 3 loading | Bilingual reading arrives + P3-02/03 display |
| 7 | Admin DIM/REVEAL | `loiter.polly.wang/?admin=1` → DIM → REVEAL | Big screen dims → story dots + hover card |
| 8 | Admin PHOTO | Click PHOTO | Rainbow border + SMILE banner + hide chrome |
| 9 | Heartbeat recovery | `sudo systemctl restart loiter` → wait 25s | Characters reappear automatically |
| 10 | Story hover | After REVEAL, hover character/dot | Bilingual EN/CN reading in separated blocks |
| 11 | Sig system | S screen switch sig → M-mode proximity shake | Big screen particles + server sig_copy log |

---

## Day-of Checklist

### Night Before

- [ ] Flash all 16 devices with latest firmware (`pio run -e islands -t upload` × 16)
- [ ] Configure venue WiFi on each device (NVS persisted, auto-connect on boot)
- [ ] `sudo systemctl restart loiter` — clear stale test data
- [ ] `curl https://loiter.polly.wang/healthz` — confirm service healthy + `flash_open:true`
- [ ] Verify CopilotX online: `curl http://127.0.0.1:24680/v1/models` on VM — **语义选岛 + reading 都依赖它**（挂了也能开场：分岛自动走 `hash(text)%6` 确定性兑底、reading 走按岛静态文案，但 AI 版体验更好）
- [ ] 抽检 1-2 台烧录：文本能语义落到预期岛（验 B′ 分类质量）；顺便验一台“空文本/乱码”也能落岛（走 hash 不报错）

### Event Day — Before Start

- [ ] Big screen computer opens `loiter.polly.wang`, confirm "Connected · LOITER" in chat log
- [ ] Admin panel `/?admin=1` — verify token passes
- [ ] Power on all 16 M5 devices → big screen shows 16 characters (within 25s heartbeat)
- [ ] ⚠️ **FROM THIS MOMENT: DO NOT `restart loiter`** (loses non-home spectrum colors + 25s blackout)

### During Event

- [ ] Phase transitions: admin `B` key (firmware) or dev panel buttons (big screen)
- [ ] Before Phase 3: confirm CopilotX alive (reading depends on it; fallback exists but AI reading is better)
- [ ] ℹ️ 分岛也依赖 CopilotX（烧录时）：若烧录期间 CopilotX 挂了，全体走 hash 兑底——岛仍稳定且均衡，但失去“岛贴文本”的语义感（能开场，不阻塞）
- [ ] Emergency bug fix → only modify server/web + rsync (no restart); firmware fix needs USB re-flash (OTA removed in Phase D)
- [ ] If someone's device disconnects: it will self-heal within 25s (heartbeat)

### After Event

- [ ] Admin → PHOTO mode → screenshot for memories
- [ ] Save big screen screenshot
- [ ] Admin → REVEAL → hover each character for bilingual reading (photo op)
- [ ] Collect all 16 devices
- [ ] `sudo systemctl restart loiter` — safe to restart now

---

## Known Limitations (accepted)

- Server restart loses HI-collected non-home colors (heartbeat only restores quiz→home color)
- No member expiry sweep (devices turned off without LWT will linger until next restart)
- `TRIGGER_MIN = 5` for jump_burst — need 5 people jumping within 10s for full-screen rainbow effect
- Sig proximity exchange requires both devices on the big-screen map, within ~400px server coords, both shaking within a 2.5s window (放宽后可跨相邻岛)
- B′ 语义选岛不保证均衡：语义接近的人会聚到同岛（这是设计意图，不是 bug；大屏环形散布能扑单岛 ~6 人）；CopilotX 挂才走均衡的 hash
