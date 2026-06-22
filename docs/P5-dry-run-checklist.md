# P5 Dry Run Checklist

## End-to-End Test (2-device pre-workshop验证)

| # | Test | Action | Expected |
|---|------|--------|----------|
| 1 | Boot + WiFi | New device (no NVS) power on | Pride rainbow WiFi → connect → Welcome screen |
| 2 | Phase 1 full | Name → dress-up → quiz 3Q → island assign | Big screen shows character on correct island |
| 3 | Phase 2 HI | A sends HI to B → B accepts | Both get new color + big screen rainbow arc |
| 4 | Phase 2 M-mode | Press M → tilt walk → DEL exit | Big screen character moves + banner shows/hides |
| 5 | Phase 2 shake=jump | Normal mode shake | M5 local jump + big screen bounce animation |
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
- [ ] `curl https://loiter.polly.wang/healthz` — confirm service healthy
- [ ] Verify CopilotX online: `curl http://127.0.0.1:24680/v1/models` on VM

### Event Day — Before Start

- [ ] Big screen computer opens `loiter.polly.wang`, confirm "Connected · LOITER" in chat log
- [ ] Admin panel `/?admin=1` — verify token passes
- [ ] Power on all 16 M5 devices → big screen shows 16 characters (within 25s heartbeat)
- [ ] ⚠️ **FROM THIS MOMENT: DO NOT `restart loiter`** (loses non-home spectrum colors + 25s blackout)

### During Event

- [ ] Phase transitions: admin `B` key (firmware) or dev panel buttons (big screen)
- [ ] Before Phase 3: confirm CopilotX alive (reading depends on it; fallback exists but AI reading is better)
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
- Sig proximity exchange requires both devices in M-mode, walking close (< 170px server coords), shaking within 1.5s window
