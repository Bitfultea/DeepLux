#!/usr/bin/env bash
# sam_bench 非法参数回归（原生推理计划·阶段0，评审意见2）
#
# 背景：main.cpp 曾复用单个 ok 变量做 toInt 校验，--warmup abc 会被后续
# --size 的成功解析覆盖而误收。本脚本固化参数级守卫回归，含该用例。
#
# 口径：所有用例都在任何网络活动之前失败关闭（main 的校验顺序：模式互斥 →
# 图像参数互斥 → 数值守卫 → 图像生成/读取 → 后端监听）。不做死端口探测——
# 网络/后端失败的失败关闭由 test_sambackendclient（超时/网络失败/严格解析）
# 与 test_agent_sam_tools（服务不可达）覆盖。唯一例外是末尾的正向对照，
# 它使用工具内嵌 stub（自监听自连，不依赖外部端口）。
#
# 用法：bash tools/sam_bench/check_arg_guards.sh [sam_bench可执行文件路径]
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

TMPIMG="$(mktemp /tmp/sam_bench_guard_XXXXXX.png)"
trap 'rm -f "$TMPIMG"' EXIT

pass=0
fail=0
expect() { # expect <期望退出码> <描述> <参数...>
    local want="$1"; shift
    local desc="$1"; shift
    "$BENCH" "$@" > /dev/null 2>&1
    local got=$?
    if [ "$got" = "$want" ]; then
        echo "PASS [$desc] exit=$got"
        pass=$((pass + 1))
    else
        echo "FAIL [$desc] 期望 exit=$want 实际 exit=$got"
        fail=$((fail + 1))
    fi
}

# --server 值仅用于触发互斥守卫，守卫先于任何客户端/连接创建，不会发出请求
expect 2 "server 与 embedded-stub 互斥（同时提供）" --server http://127.0.0.1:1 --embedded-stub --gen-image "$TMPIMG"
expect 2 "server 与 embedded-stub 互斥（都缺省）" --gen-image "$TMPIMG"
expect 2 "image 与 gen-image 互斥（同时提供）" --embedded-stub --image "$TMPIMG" --gen-image "$TMPIMG"
expect 2 "image 与 gen-image 互斥（都缺省）" --embedded-stub
expect 2 "runs 非整数" --embedded-stub --gen-image "$TMPIMG" --runs abc
expect 2 "warmup 非整数（ok 复用回归：其余数值参数均合法）" --embedded-stub --gen-image "$TMPIMG" --runs 5 --warmup abc --size 300
expect 2 "timeout 非整数" --embedded-stub --gen-image "$TMPIMG" --timeout xyz
expect 2 "size 非整数" --embedded-stub --gen-image "$TMPIMG" --size big
expect 2 "runs 越界(0)" --embedded-stub --gen-image "$TMPIMG" --runs 0
expect 2 "runs 越界(100001)" --embedded-stub --gen-image "$TMPIMG" --runs 100001
expect 2 "warmup 越界(-1)" --embedded-stub --gen-image "$TMPIMG" --warmup=-1
expect 2 "timeout 越界(50)" --embedded-stub --gen-image "$TMPIMG" --timeout 50
expect 2 "size 越界(10)" --embedded-stub --gen-image "$TMPIMG" --size 10
expect 3 "image 不存在（守卫后、监听前失败关闭）" --embedded-stub --image /nonexistent_sam_bench_guard.png
expect 2 "on-error 非法值" --embedded-stub --gen-image "$TMPIMG" --on-error ignore
expect 2 "inject 非整数" --embedded-stub --gen-image "$TMPIMG" --inject-invalid-embedding abc
expect 2 "inject 负值" --embedded-stub --gen-image "$TMPIMG" --inject-invalid-embedding=-1
# --server 值仅触发守卫（inject 需 embedded-stub），先于任何客户端/连接创建
expect 2 "inject 需 embedded-stub" --server http://127.0.0.1:1 --gen-image "$TMPIMG" --inject-invalid-embedding 1
expect 2 "inject-predict-error 需 embedded-stub" --server http://127.0.0.1:1 --gen-image "$TMPIMG" --inject-predict-error
expect 2 "inject-score-fault 非法值" --embedded-stub --gen-image "$TMPIMG" --inject-score-fault bogus
expect 2 "inject-score-fault 需 embedded-stub" --server http://127.0.0.1:1 --gen-image "$TMPIMG" --inject-score-fault missing
expect 0 "正向对照：内嵌 stub 小图 3 次预测" --embedded-stub --gen-image "$TMPIMG" --size 64 --runs 3

echo "-----"
echo "arg guards: $pass passed, $fail failed"
[ "$fail" = "0" ] || exit 1
