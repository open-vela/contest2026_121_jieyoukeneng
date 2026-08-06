#!/usr/bin/env bash
#
# 安聆 VelaGuard - 主机侧回归测试
#
#   cd tests && make test
#
# 覆盖内容：
#   1. 端侧内置验收自检（PRD-08 可自动化场景，25 项断言）
#   2. 端侧 C 特征实现 与 训练脚本 Python 特征实现 的逐维一致性
#   3. 真实识别链路（wav -> 特征 -> int8 推理 -> 观测 -> 状态机 -> 事件）
#   4. 事件日志断电重启不丢失 + 100 条环形覆盖
#   5. 控制台协议闭环（上传 / 幂等补发 / 家属侧回写 / 手机绑定 / 隐私字段拒绝）
#
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
WORK="$HERE/.vgtest"
BIN="$HERE/build/velaguard"

PASS=0
FAIL=0

# 本地回环不走代理，避免开发机的 http_proxy 干扰测试
export no_proxy="127.0.0.1,localhost"
export NO_PROXY="$no_proxy"
curlx() { curl --noproxy '*' "$@"; }

ok()   { PASS=$((PASS+1)); echo "  ✓ $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  ✗ $1"; }
head2() { echo; echo "== $1 =="; }

export VELAGUARD_DATA_DIR="$WORK/data"
export VELAGUARD_SKILL_DIR="$ROOT/agent_skill/anling-home-safety"
export VELAGUARD_CONSOLE_HOST=127.0.0.1
export VELAGUARD_CONSOLE_PORT=18080
export VELAGUARD_DEVICE_ID=velaguard_test

rm -rf "$WORK/data"
mkdir -p "$WORK/data" "$WORK/wav"

# ---------------------------------------------------------------------------
head2 "1. 端侧内置验收自检"
# ---------------------------------------------------------------------------
if "$BIN" selftest > "$WORK/selftest.log" 2>&1; then
  ok "$(grep -o '通过 [0-9]* 项，失败 [0-9]* 项' "$WORK/selftest.log" | tail -1)"
else
  bad "内置自检失败，详见 $WORK/selftest.log"
  tail -20 "$WORK/selftest.log"
fi

# ---------------------------------------------------------------------------
head2 "2. 特征实现一致性（端侧 C vs 训练脚本 Python）"
# ---------------------------------------------------------------------------
if python3 -c "import numpy" 2>/dev/null; then
  if python3 "$HERE/check_feature_parity.py"; then
    ok "特征逐维一致"
  else
    bad "特征不一致：电脑端训练指标在板上不成立"
  fi
else
  echo "  - 跳过（缺少 numpy: pip3 install numpy）"
fi

# ---------------------------------------------------------------------------
head2 "3. 真实识别链路（wav -> 特征 -> int8 推理 -> 状态机）"
# ---------------------------------------------------------------------------
if python3 -c "import numpy" 2>/dev/null; then
  python3 "$HERE/make_demo_wavs.py" > /dev/null

  check_feed() {
    local wav="$1" expect="$2" desc="$3"
    local tag="${wav%.wav}"
    rm -f "$VELAGUARD_DATA_DIR"/events.bin
    "$BIN" feed "$WORK/wav/$wav" > "$WORK/feed_$tag.log" 2>&1
    if "$BIN" log 20 2>/dev/null | grep -q "$expect"; then
      ok "$desc"
    else
      bad "$desc（未产生预期事件，详见 $WORK/feed_$tag.log）"
    fi
  }

  check_feed alarm_beep.wav "烟雾/燃气报警" "报警蜂鸣 wav 触发报警事件"
  check_feed water_flow.wav "持续水流"     "持续水流 wav 触发水流事件"
  check_feed impact.wav     "破碎或撞击"   "撞击 wav 触发撞击事件"

  # 负样本：背景噪声与普通说话不得产生任何安全事件
  rm -f "$VELAGUARD_DATA_DIR"/events.bin
  "$BIN" feed "$WORK/wav/background.wav" > "$WORK/feed_bg.log" 2>&1
  if [ "$("$BIN" log 50 2>/dev/null | grep -c '^[0-9]')" = "0" ]; then
    ok "背景噪声负样本不误报"
  else
    bad "背景噪声产生了误报事件"
    "$BIN" log 5
  fi
else
  echo "  - 跳过（缺少 numpy）"
fi

# ---------------------------------------------------------------------------
head2 "4. 事件日志持久化与环形覆盖"
# ---------------------------------------------------------------------------
rm -f "$VELAGUARD_DATA_DIR"/events.bin
"$BIN" sim alarm_beep --conf 0.95 --times 8 --interval 1000 > /dev/null 2>&1
N1=$("$BIN" log 5 2>/dev/null | grep -c '^[0-9]')
# 重新拉起进程 = 模拟断电重启
N2=$("$BIN" log 5 2>/dev/null | grep -c '^[0-9]')
if [ "$N1" -ge 1 ] && [ "$N1" = "$N2" ]; then
  ok "重启后事件日志完整恢复（$N2 条）"
else
  bad "重启后日志丢失（重启前 $N1 条，重启后 $N2 条）"
fi

if [ -f "$VELAGUARD_DATA_DIR/events.bin" ]; then
  ok "事件日志已落盘 $(du -h "$VELAGUARD_DATA_DIR/events.bin" | cut -f1)"
else
  bad "事件日志未落盘"
fi

# ---------------------------------------------------------------------------
head2 "5. 控制台协议闭环"
# ---------------------------------------------------------------------------
if command -v node > /dev/null 2>&1; then
  rm -f "$WORK/console.db" "$WORK/family.cookie"
  VELAGUARD_PORT=$VELAGUARD_CONSOLE_PORT VELAGUARD_DB="$WORK/console.db" \
    VELAGUARD_ALLOW_ANONYMOUS=false VELAGUARD_ALLOW_ANONYMOUS_DEVICE=true \
    VELAGUARD_PAIRING_KEY=velaguard-demo \
    node "$ROOT/prototype/console/server.js" > "$WORK/console.log" 2>&1 &
  CONSOLE_PID=$!
  sleep 1.2

  if curlx -sf "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/health" > /dev/null; then
    ok "控制台启动成功（零 npm 依赖）"
  else
    bad "控制台启动失败，详见 $WORK/console.log"
  fi

  if curlx -sf "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/pairing" \
      | grep -q '"deviceId":"velaguard_test"'; then
    ok "小米手机配对页自动提供演示设备编号"
  else
    bad "小米手机配对页未提供设备编号"
  fi

  if VELAGUARD_DATA_DIR="$VELAGUARD_DATA_DIR" VELAGUARD_PROFILE=demo \
       "$BIN" console set 127.0.0.1 "$VELAGUARD_CONSOLE_PORT" > "$WORK/console-set.log" 2>&1 && \
     VELAGUARD_DATA_DIR="$VELAGUARD_DATA_DIR" VELAGUARD_PROFILE=demo \
       "$BIN" console show | grep -q "127.0.0.1:$VELAGUARD_CONSOLE_PORT"; then
    ok "板端 console set 保存后可在重启进程中恢复"
  else
    bad "板端 console set 保存或恢复失败"
  fi

  # 家属接口默认必须先配对；设备演示上传仍允许匿名接入。
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events?limit=1")
  if [ "$CODE" = "401" ]; then
    ok "未配对的家属会话被拒绝"
  else
    bad "未配对会话未被拒绝（返回 $CODE）"
  fi

  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d '{"deviceId":"velaguard_test","key":"wrong-key"}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/pairing")
  if [ "$CODE" = "401" ]; then
    ok "错误配对密钥被拒绝"
  else
    bad "错误配对密钥未被拒绝（返回 $CODE）"
  fi

  PAIRING_BODY=$(curlx -sf -D "$WORK/family.headers" -c "$WORK/family.cookie" -X POST \
    -H 'Content-Type: application/json' \
    -d '{"deviceId":"velaguard_test","key":"velaguard-demo"}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/pairing")
  if echo "$PAIRING_BODY" | grep -q '"paired":true' && \
     grep -q 'vg_session' "$WORK/family.cookie"; then
    ok "正确配对建立 HttpOnly 家属会话"
  else
    bad "正确配对未建立 Cookie 会话"
  fi
  if grep -qi 'vg_session=.*HttpOnly.*SameSite=Strict.*Path=/' "$WORK/family.headers" && \
     ! grep -qi 'vg_session=.*Domain=' "$WORK/family.headers"; then
    ok "家属 Cookie 启用 HttpOnly/SameSite/Path 保护且不设置 Domain"
  else
    bad "家属 Cookie 属性不符合最小安全边界"
  fi

  SSE_PIDS=()
  for SSE_INDEX in 1 2 3 4; do
    curlx -Ns --max-time 5 -b "$WORK/family.cookie" \
      "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/stream" \
      > "$WORK/sse_$SSE_INDEX.log" 2>&1 &
    SSE_PIDS+=("$!")
  done
  sleep 0.4
  if grep -q '^event: hello' "$WORK/sse_1.log"; then
    ok "家属 SSE 会话建立并收到 hello"
  else
    bad "家属 SSE 会话未建立"
  fi
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -b "$WORK/family.cookie" \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/stream")
  if [ "$CODE" = "429" ]; then
    ok "SSE 连接数按会话上限限制"
  else
    bad "SSE 连接数未受限制（返回 $CODE）"
  fi
  for SSE_PID in "${SSE_PIDS[@]}"; do
    kill "$SSE_PID" 2>/dev/null || true
    wait "$SSE_PID" 2>/dev/null || true
  done

  # 5.1 设备上传
  "$BIN" notify test > /dev/null 2>&1
  if curlx -sf -b "$WORK/family.cookie" \
       "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events/evt_test" \
       | grep -q '"ok":true'; then
    ok "端侧事件成功上传到控制台"
  else
    bad "端侧事件上传失败"
  fi

  # 5.2 幂等：重复上传同一 eventId 只更新
  "$BIN" notify test > /dev/null 2>&1
  CNT=$(curlx -sf -b "$WORK/family.cookie" \
        "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events?limit=100" \
        | grep -o '"eventId":"evt_test"' | wc -l)
  if [ "$CNT" = "1" ]; then
    ok "断网补发幂等：同一 eventId 不重复入库"
  else
    bad "幂等失败：evt_test 出现 $CNT 次"
  fi

  # 5.3 家属侧动作只创建命令，不直接改变设备事件状态
  if curlx -sf -b "$WORK/family.cookie" -X PATCH \
       -H 'Origin: http://127.0.0.1:18080' \
       -H 'Content-Type: application/json' \
       -d '{"localStatus":"handled","desiredRevision":1}' \
       "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events/evt_test" \
       | grep -q '"requested":true'; then
    ok "家属侧动作进入待执行命令"
  else
    bad "家属侧命令创建失败"
  fi

  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -b "$WORK/family.cookie" \
    -X PATCH -H 'Content-Type: application/json' \
    -d '{"localStatus":"handled","desiredRevision":1}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events/evt_test")
  if [ "$CODE" = "403" ]; then
    ok "Cookie 写操作缺少 Origin 时被拒绝"
  else
    bad "Cookie 写操作缺少 Origin 未被拒绝（返回 $CODE）"
  fi

  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -b "$WORK/family.cookie" \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events?limit=foo")
  if [ "$CODE" = "400" ]; then
    ok "事件查询拒绝非法 limit"
  else
    bad "事件查询未拒绝非法 limit（返回 $CODE）"
  fi

  if sed -n '/card\.innerHTML = `/,/empty\.style/p' \
       "$ROOT/prototype/console/public/index.html" | \
       grep -q '\${evt\.matchedPhrase}\|\${evt\.personLabel}\|· \${evt\.deviceId}'; then
    bad "手机事件卡片仍存在未转义的设备字段插值"
  else
    ok "手机事件卡片转义设备字段，阻止 HTML 注入"
  fi

  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d '{"deviceId":"rate_probe_1","key":"wrong-key"}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/pairing")
  if [ "$CODE" = "401" ]; then
    :
  else
    bad "配对限流探针初始化失败（返回 $CODE）"
  fi
  for RATE_INDEX in $(seq 2 520); do
    RATE_ID="rate_probe_$RATE_INDEX"
    curlx -s -o /dev/null -X POST -H 'Content-Type: application/json' \
      -d "{\"deviceId\":\"$RATE_ID\",\"key\":\"wrong-key\"}" \
      "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/pairing"
  done
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d '{"deviceId":"rate_probe_after_520","key":"wrong-key"}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/pairing")
  if [ "$CODE" = "429" ]; then
    ok "配对失败按客户端地址限流，不能靠更换设备号绕过"
  else
    bad "配对失败限流可被更换设备号绕过（返回 $CODE）"
  fi

  COMMAND_ID=$(curlx -sf \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/v1/devices/velaguard_test/commands" \
    | sed -n 's/.*"commandId":"\([A-Za-z0-9_.-]*\)".*/\1/p' | head -1)
  if [ -n "$COMMAND_ID" ]; then
    ok "设备可主动拉取待执行命令"
  else
    bad "设备命令拉取失败"
  fi

  curlx -Ns --max-time 5 -b "$WORK/family.cookie" \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/stream" \
    > "$WORK/command-result-sse.log" 2>&1 &
  COMMAND_SSE_PID=$!
  sleep 0.3

  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d '{"status":"failed","messageId":"missing-command-id","resultRevision":1}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/v1/devices/velaguard_test/command-results")
  if [ "$CODE" = "400" ] && \
     curlx -sf "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/health" > /dev/null; then
    ok "缺少 commandId 的结果请求被拒绝且控制台不退出"
  else
    bad "缺少 commandId 的结果请求导致控制台异常"
  fi

  if [ -n "$COMMAND_ID" ] && curlx -sf -X POST \
       -H 'Content-Type: application/json' \
       -d "{\"commandId\":\"$COMMAND_ID\",\"messageId\":\"receipt_msg_1\"}" \
       "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/v1/devices/velaguard_test/command-receipts" \
       | grep -q '"messageType":"commandReceiptAck"'; then
    ok "设备持久化命令接收回执并返回 ACK"
  else
    bad "设备命令接收回执 ACK 失败"
  fi

  if [ -n "$COMMAND_ID" ] && curlx -sf -X POST \
       -H 'Content-Type: application/json' \
       -d "{\"commandId\":\"$COMMAND_ID\",\"messageId\":\"result_msg_1\",\"status\":\"requires_local_confirmation\",\"resultRevision\":1}" \
       "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/v1/devices/velaguard_test/command-results" \
       | grep -q '"messageType":"commandResultIngressAck"'; then
    ok "设备命令结果持久化并返回接入 ACK"
  else
    bad "设备命令结果 ACK 失败"
  fi

  if [ -n "$COMMAND_ID" ] && curlx -sf -X POST \
       -H 'Content-Type: application/json' \
       -d "{\"commandId\":\"$COMMAND_ID\",\"messageId\":\"result_msg_2\",\"status\":\"applied\",\"resultRevision\":2}" \
       "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/v1/devices/velaguard_test/command-results" \
       | grep -q '"resultRevision":2'; then
    ok "命令结果修订号可以单调推进并回推状态"
  else
    bad "命令结果修订号推进失败"
  fi
  if [ -n "$COMMAND_ID" ] && curlx -sf -X POST \
       -H 'Content-Type: application/json' \
       -d "{\"commandId\":\"$COMMAND_ID\",\"messageId\":\"result_msg_2\",\"status\":\"applied\",\"resultRevision\":2}" \
       "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/v1/devices/velaguard_test/command-results" \
       | grep -q '"duplicate":true'; then
    ok "相同 command result messageId 重发只返回幂等 ACK"
  else
    bad "相同 command result messageId 未正确幂等"
  fi
  sleep 0.3
  if grep -q '^event: command-result' "$WORK/command-result-sse.log"; then
    ok "设备最终命令结果通过 SSE 回推手机"
  else
    bad "设备最终命令结果未回推手机 SSE"
  fi
  kill "$COMMAND_SSE_PID" 2>/dev/null || true
  wait "$COMMAND_SSE_PID" 2>/dev/null || true
  if curlx -sf -b "$WORK/family.cookie" \
       "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events/evt_test/commands" \
       | grep -q '"resultMessageId":"result_msg_2"'; then
    ok "手机刷新后可以读取命令最终结果"
  else
    bad "手机刷新后无法读取命令最终结果"
  fi
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d "{\"commandId\":\"$COMMAND_ID\",\"messageId\":\"stale_result\",\"status\":\"rejected\",\"resultRevision\":1}" \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/v1/devices/velaguard_test/command-results")
  if [ "$CODE" = "409" ]; then
    ok "旧命令结果修订号不能覆盖新结果"
  else
    bad "旧命令结果修订号覆盖了新结果（返回 $CODE）"
  fi
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d "{\"commandId\":\"$COMMAND_ID\",\"messageId\":\"huge_revision\",\"status\":\"failed\",\"resultRevision\":9007199254740992}" \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/v1/devices/velaguard_test/command-results")
  if [ "$CODE" = "400" ]; then
    ok "命令结果拒绝超出安全范围的修订号"
  else
    bad "命令结果接受了超出安全范围的修订号（返回 $CODE）"
  fi

  # 5.4 隐私红线：带原始音频/对话文本字段的负载必须被拒绝
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d '{"eventId":"evt_priv","deviceId":"d","eventType":"impact","level":"warning","transcript":"家里的对话内容"}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events")
  if [ "$CODE" = "400" ]; then
    ok "隐私红线：含对话文本的负载被拒绝"
  else
    bad "隐私红线失守：含 transcript 的负载返回 $CODE"
  fi

  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d '{"eventId":"evt_priv2","deviceId":"d","eventType":"impact","level":"warning","confidence":0.9,"startedAt":"2026-08-07T00:00:00.000Z","durationSec":1,"localStatus":"pending","uploadReason":"manual_test","timeReliable":true,"night":false,"summary":"撞击","audioUrl":"http://bad.example/audio.wav"}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events")
  if [ "$CODE" = "400" ]; then
    ok "隐私红线：legacy 事件中的 audioUrl 也被拒绝"
  else
    bad "隐私红线失守：legacy audioUrl 返回 $CODE"
  fi

  DEEP_JSON='{}'
  for _ in $(seq 1 40); do DEEP_JSON="{\"nested\":$DEEP_JSON}"; done
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' -d "$DEEP_JSON" \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events")
  if [ "$CODE" = "400" ] && \
     curlx -sf "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/health" > /dev/null; then
    ok "深层 JSON 被拒绝且控制台保持运行"
  else
    bad "深层 JSON 触发控制台异常"
  fi

  # 5.5 协议校验：非法 eventType 必须被拒绝
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' \
    -d '{"eventId":"evt_bad","deviceId":"d","eventType":"door_open","level":"warning"}' \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events")
  if [ "$CODE" = "400" ]; then
    ok "协议校验：非法 eventType 被拒绝"
  else
    bad "协议校验失效：非法 eventType 返回 $CODE"
  fi

  # 5.6 小米手机演示会话可主动注销，旧 Cookie 立即失效
  if curlx -sf -b "$WORK/family.cookie" -X DELETE \
       -H 'Origin: http://127.0.0.1:18080' \
       "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/pairing" \
       | grep -q '"revoked":true'; then
    ok "手机会话可以解除绑定"
  else
    bad "手机会话解除绑定失败"
  fi
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' -b "$WORK/family.cookie" \
    "http://127.0.0.1:$VELAGUARD_CONSOLE_PORT/events?limit=1")
  if [ "$CODE" = "401" ]; then
    ok "解除绑定后旧手机会话被拒绝"
  else
    bad "解除绑定后旧手机会话仍可访问（返回 $CODE）"
  fi

  # 5.7 断网补发：控制台停掉后事件入队，恢复后自动送达
  kill $CONSOLE_PID 2>/dev/null
  wait $CONSOLE_PID 2>/dev/null
  "$BIN" sim distress_voice --conf 0.95 --urgency 0.9 --times 3 \
      --interval 800 > /dev/null 2>&1
  PENDING=$("$BIN" flush 2>/dev/null | grep -o '待发送 [0-9]*' | head -1)
  if echo "$PENDING" | grep -qv '待发送 0'; then
    ok "断网期间通知进入待发送队列（$PENDING）"
  else
    bad "断网期间通知未入队"
  fi

  VELAGUARD_PORT=$VELAGUARD_CONSOLE_PORT VELAGUARD_DB="$WORK/console.db" \
    VELAGUARD_ALLOW_ANONYMOUS=false VELAGUARD_ALLOW_ANONYMOUS_DEVICE=true \
    VELAGUARD_PAIRING_KEY=velaguard-demo \
    node "$ROOT/prototype/console/server.js" >> "$WORK/console.log" 2>&1 &
  CONSOLE_PID=$!
  sleep 1.2
  "$BIN" flush > /dev/null 2>&1
  if "$BIN" flush 2>/dev/null | grep -q '待发送 0'; then
    ok "网络恢复后待发送通知全部补发成功"
  else
    bad "网络恢复后补发失败"
  fi

  kill $CONSOLE_PID 2>/dev/null
  wait $CONSOLE_PID 2>/dev/null

  PROD_PORT=$((VELAGUARD_CONSOLE_PORT + 1))
  if VELAGUARD_PROFILE=production VELAGUARD_PORT=$PROD_PORT \
       VELAGUARD_DB="$WORK/production-guard.db" \
       VELAGUARD_TLS_TERMINATED=true VELAGUARD_SESSION_TOKEN=prod-session \
       VELAGUARD_DEVICE_ID=velaguard_test VELAGUARD_PAIRING_KEY=prod-pairing \
       node "$ROOT/prototype/console/server.js" \
       > "$WORK/production-guard.log" 2>&1; then
    bad "生产反向代理未配置可信代理地址却启动成功"
  elif grep -q 'VELAGUARD_TRUSTED_PROXY' "$WORK/production-guard.log"; then
    ok "生产反向代理必须显式配置可信代理地址"
  else
    bad "生产反向代理启动保护提示不正确"
  fi

  VELAGUARD_PROFILE=production VELAGUARD_PORT=$PROD_PORT \
    VELAGUARD_DB="$WORK/production.db" VELAGUARD_TLS_TERMINATED=true \
    VELAGUARD_TRUSTED_PROXY=127.0.0.1 VELAGUARD_SESSION_TOKEN=prod-session \
    VELAGUARD_DEVICE_ID=velaguard_test VELAGUARD_PAIRING_KEY=prod-pairing \
    node "$ROOT/prototype/console/server.js" \
    > "$WORK/production.log" 2>&1 &
  PROD_PID=$!
  sleep 1.0
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$PROD_PORT/health")
  if [ "$CODE" = "421" ]; then
    ok "生产控制台拒绝未标记 HTTPS 的明文请求"
  else
    bad "生产控制台明文边界失效（返回 $CODE）"
  fi
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' \
    -H 'X-Forwarded-Proto: https' "http://127.0.0.1:$PROD_PORT/health")
  if [ "$CODE" = "200" ]; then
    ok "可信 HTTPS 终止代理可以访问生产控制台"
  else
    bad "可信 HTTPS 终止代理访问失败（返回 $CODE）"
  fi
  CODE=$(curlx -s -o /dev/null -w '%{http_code}' \
    -H 'X-Forwarded-Proto: https,http' "http://127.0.0.1:$PROD_PORT/health")
  if [ "$CODE" = "421" ]; then
    ok "生产控制台拒绝多值 X-Forwarded-Proto"
  else
    bad "生产控制台接受了多值 X-Forwarded-Proto（返回 $CODE）"
  fi
  curlx -s -D "$WORK/production-pairing.headers" -o /dev/null \
    -H 'X-Forwarded-Proto: https' -H 'Content-Type: application/json' \
    -d '{"deviceId":"velaguard_test","key":"prod-pairing"}' \
    "http://127.0.0.1:$PROD_PORT/pairing"
  if grep -q '__Host-vg_session=.*Secure' "$WORK/production-pairing.headers"; then
    ok "生产配对使用 Secure 的 __Host- 会话 Cookie"
  else
    bad "生产配对 Cookie 未启用 __Host-/Secure 保护"
  fi
  kill $PROD_PID 2>/dev/null
  wait $PROD_PID 2>/dev/null
else
  bad "未安装 node，控制台协议闭环无法验证"
fi

# ---------------------------------------------------------------------------
head2 "6. 隐私自检：仓库内不得出现原始音频与明文凭证"
# ---------------------------------------------------------------------------
AUDIO=$(cd "$ROOT" && git ls-files | grep -Ei '\.(wav|mp3|flac|pcm|ogg|m4a)$' | wc -l)
if [ "$AUDIO" = "0" ]; then
  ok "Git 跟踪文件中无任何音频文件"
else
  bad "Git 中存在 $AUDIO 个音频文件"
fi

if [ -f "$ROOT/prototype/console/config.json" ]; then
  bad "存在未忽略的真实配置文件 config.json"
else
  ok "仓库只提供 config.example.json，无真实凭证"
fi

# ---------------------------------------------------------------------------
echo
echo "======================================"
echo "  回归结果：通过 $PASS 项，失败 $FAIL 项"
echo "======================================"
[ "$FAIL" = "0" ]
