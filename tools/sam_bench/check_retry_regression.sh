#!/usr/bin/env bash
# sam_bench 重试计时回归（原生推理计划·阶段0 复核二轮 P1-2）
#
# 背景：SamBackendClient 收到 {"status":"invalid_embedding"} 会自动重 setImage
# 并重发 predict——重试期间 embeddingReady 会在 predict 计时窗口内触发。
# 旧实现 runOp 对任意操作同时监听两个完成信号，重试的 embeddingReady 会
# 提前结束计时并被误计为一次成功预测。修正后 predict 操作只以
# predictionReady 为完成信号，期间的 embeddingReady 记为 retryEvents。
#
# 断言（注入 1 次 invalid_embedding）：
#   1) exit 0 —— 重试链完整走通
#   2) retryEvents >= 1 —— 期间 embeddingReady 被记为重试事件而非完成信号
#   3) runsValid == runs 且 violationCount == 0 —— 重试样本未被丢弃/误分类
# 若回归为“任意信号即完成”：内部不变量触发 exit 5，或 retryEvents==0，
# 本脚本红。
#
# 用法：bash tools/sam_bench/check_retry_regression.sh [sam_bench路径]
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
BENCH="${1:-}"
if [ -z "$BENCH" ]; then
    for cand in "$HERE/../../build/bin/sam_bench" "$HERE/../../build-agent/bin/sam_bench"; do
        [ -x "$cand" ] && BENCH="$cand" && break
    done
fi
if [ -z "$BENCH" ] || [ ! -x "$BENCH" ]; then
    echo "找不到 sam_bench 可执行文件（传参指定，或先构建 build/bin、build-agent/bin）"
    exit 1
fi
command -v python3 > /dev/null || { echo "需要 python3 解析 JSON 断言"; exit 1; }

TMPIMG="$(mktemp /tmp/sam_bench_retry_XXXXXX.png)"
TMPJSON="$(mktemp /tmp/sam_bench_retry_XXXXXX.json)"
trap 'rm -f "$TMPIMG" "$TMPJSON"' EXIT

fail=0
run_case() { # run_case <injectN> <runs>
    "$BENCH" --embedded-stub --gen-image "$TMPIMG" --size 64 --runs "$2" \
        --inject-invalid-embedding "$1" --json "$TMPJSON" > /dev/null 2>&1
    local rc=$?
    if [ "$rc" != "0" ]; then
        echo "FAIL [inject=$1 runs=$2] exit=$rc（期望 0；5=计时早退不变量违背）"
        fail=1
        return
    fi
    if python3 - "$TMPJSON" "$1" "$2" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
inj, runs = int(sys.argv[2]), int(sys.argv[3])
errs = []
if d.get("retryEvents", 0) < 1:
    errs.append(f"retryEvents={d.get('retryEvents')} 期望>=1（embeddingReady 应记为重试事件）")
if d.get("runsValid") != runs:
    errs.append(f"runsValid={d.get('runsValid')} 期望 {runs}")
if d.get("violationCount", -1) != 0:
    errs.append(f"violationCount={d.get('violationCount')} 期望 0")
if d.get("injectedInvalidEmbedding") != inj:
    errs.append(f"injectedInvalidEmbedding={d.get('injectedInvalidEmbedding')} 期望 {inj}")
if errs:
    print("FAIL [inject=%d runs=%d]: %s" % (inj, runs, "; ".join(errs)))
    sys.exit(1)
print(f"PASS [inject={inj} runs={runs}] retryEvents={d['retryEvents']} runsValid={d['runsValid']} violations=0")
PY
    then :; else fail=1; fi
}

run_case 1 3
run_case 1 10
# 注入 2 次：客户端每轮重试都会清空 m_imageRetryPending，可对连续
# invalid_embedding 多次重试（无上限，既有客户端行为=基线文档 F7 观察项）；
# 注入耗尽后预测成功 → 仍应 exit 0 且 retryEvents>=1
run_case 2 3

# P2-4 收窄回归：/predict 持续返回 {"status":"error"}（非 F1 口径）。
# continue 模式必须同样失败关闭 exit 4，不得当作违约样本放行；
# fail 模式 exit 4（对照）。
"$BENCH" --embedded-stub --gen-image "$TMPIMG" --size 64 --runs 3 \
    --inject-predict-error --on-error continue --json "$TMPJSON" > /dev/null 2>&1
rc=$?
if [ "$rc" = "4" ]; then
    echo "PASS [continue 仅放行 F1] 非 F1 错误（status:error）exit=4"
else
    echo "FAIL [continue 仅放行 F1] exit=$rc（期望 4：非 F1 错误必须失败关闭）"
    fail=1
fi
"$BENCH" --embedded-stub --gen-image "$TMPIMG" --size 64 --runs 3 \
    --inject-predict-error --on-error fail --json "$TMPJSON" > /dev/null 2>&1
rc=$?
if [ "$rc" = "4" ]; then
    echo "PASS [fail 模式对照] 非 F1 错误 exit=4"
else
    echo "FAIL [fail 模式对照] exit=$rc（期望 4）"
    fail=1
fi

echo "-----"
if [ "$fail" = "0" ]; then
    echo "retry regression: PASS"
else
    echo "retry regression: FAIL"
    exit 1
fi
