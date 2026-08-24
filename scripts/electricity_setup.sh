#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
CONFIG_DIR=${XDG_CONFIG_HOME:-"$HOME/.config"}/zzu-assistant
CONFIG_FILE=$CONFIG_DIR/electricity-auto-recharge.conf
PASSWORD_FILE=$CONFIG_DIR/electricity-payment-password
CRON_SCRIPT=$SCRIPT_DIR/electricity_cron.sh

username=""
assistant_bin=${ZZUASSISTANT_BIN:-}
interval=5

usage() {
    cat <<'EOF'
用法：electricity_setup.sh [选项]

登录超级 App、设置宿舍房间，保存自动充值配置并安装 cron 任务。

选项：
  --username <学号>   预先指定登录用户
  --bin <路径>        指定 ZZUAssistant 可执行文件
  --interval <分钟>   检查间隔，范围 1～59（默认：5）
  -h, --help          显示此帮助
EOF
}

die() {
    printf '[ERROR] %s\n' "$*" >&2
    exit 1
}

is_positive_number() {
    [[ $1 =~ ^([0-9]+([.][0-9]*)?|[.][0-9]+)$ ]] &&
        awk -v value="$1" 'BEGIN { exit !(value > 0) }'
}

find_assistant() {
    local candidate

    if [[ -n $assistant_bin ]]; then
        [[ -x $assistant_bin ]] || die "ZZUAssistant 不可执行：$assistant_bin"
        assistant_bin=$(cd -- "$(dirname -- "$assistant_bin")" && pwd -P)/$(basename -- "$assistant_bin")
        return
    fi

    for candidate in \
        "$PROJECT_DIR/cmake-build-debug/ZZUAssistant.exe" \
        "$PROJECT_DIR/cmake-build-release/ZZUAssistant.exe" \
        "$PROJECT_DIR/out/build/vcpkg-debug/ZZUAssistant.exe" \
        "$PROJECT_DIR/out/build/vcpkg-release/ZZUAssistant.exe" \
        "$PROJECT_DIR/out/build/vcpkg-debug/ZZUAssistant" \
        "$PROJECT_DIR/out/build/vcpkg-release/ZZUAssistant" \
        "$PROJECT_DIR/build/ZZUAssistant" \
        "$PROJECT_DIR/build/ZZUAssistant.exe"; do
        if [[ -x $candidate ]]; then
            assistant_bin=$candidate
            return
        fi
    done

    if command -v ZZUAssistant >/dev/null 2>&1; then
        assistant_bin=$(command -v ZZUAssistant)
    elif command -v ZZUAssistant.exe >/dev/null 2>&1; then
        assistant_bin=$(command -v ZZUAssistant.exe)
    else
        die '找不到 ZZUAssistant；请使用 --bin 指定可执行文件。'
    fi
}

while (($#)); do
    case $1 in
        --username)
            (($# >= 2)) || die '--username 缺少参数'
            username=$2
            shift 2
            ;;
        --bin)
            (($# >= 2)) || die '--bin 缺少参数'
            assistant_bin=$2
            shift 2
            ;;
        --interval)
            (($# >= 2)) || die '--interval 缺少参数'
            interval=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *) die "未知参数：$1" ;;
    esac
done

command -v crontab >/dev/null 2>&1 || die '系统中没有 crontab；此脚本需要 Unix cron。'
command -v awk >/dev/null 2>&1 || die '系统中没有 awk。'
[[ $interval =~ ^[0-9]+$ ]] && ((interval >= 1 && interval <= 59)) ||
    die '检查间隔必须是 1～59 分钟的整数。'

find_assistant
[[ -x $CRON_SCRIPT ]] || chmod 700 "$CRON_SCRIPT"

if [[ -z $username ]]; then
    read -r -p '学号：' username
fi
[[ -n $username && $username != *$'\n'* ]] || die '学号不能为空。'

printf '\n[1/4] 登录超级 App\n'
"$assistant_bin" app login "$username"

printf '\n[2/4] 设置照明与空调房间\n'
"$assistant_bin" electricity setup "$username"

printf '\n[3/4] 设置自动充值参数\n'
while :; do
    read -r -p '电费余额阈值（元）：' threshold
    is_positive_number "$threshold" && break
    printf '请输入大于 0 的数字。\n' >&2
done

while :; do
    read -r -p '每个低余额房间每次充值金额（整数元）：' recharge_amount
    [[ $recharge_amount =~ ^[0-9]+$ ]] && ((recharge_amount >= 1 && recharge_amount <= 1000)) && break
    printf '请输入 1～1000 的整数。\n' >&2
done

while :; do
    read -r -s -p '校园卡支付密码：' payment_password
    printf '\n'
    read -r -s -p '再次输入校园卡支付密码：' payment_password_confirm
    printf '\n'
    [[ -n $payment_password ]] || { printf '密码不能为空。\n' >&2; continue; }
    [[ $payment_password == "$payment_password_confirm" ]] && break
    printf '两次输入的密码不一致。\n' >&2
done

mkdir -p "$CONFIG_DIR"
chmod 700 "$CONFIG_DIR"
export ZZUASSISTANT_AUTOMATION_DIR=$CONFIG_DIR
config_tmp=$(mktemp "$CONFIG_DIR/.electricity-config.XXXXXX")
password_tmp=$(mktemp "$CONFIG_DIR/.electricity-password.XXXXXX")
cleanup() {
    rm -f -- "${config_tmp:-}" "${password_tmp:-}"
}
trap cleanup EXIT

{
    printf 'ZZUASSISTANT_BIN=%q\n' "$assistant_bin"
    printf 'ZZUASSISTANT_USERNAME=%q\n' "$username"
    printf 'ZZU_ELECTRICITY_THRESHOLD_YUAN=%q\n' "$threshold"
    printf 'ZZU_ELECTRICITY_RECHARGE_YUAN=%q\n' "$recharge_amount"
    printf 'ZZU_CRON_INTERVAL_MINUTES=%q\n' "$interval"
    printf 'ZZU_CRON_SCRIPT=%q\n' "$CRON_SCRIPT"
} >"$config_tmp"
printf '%s' "$payment_password" >"$password_tmp"
chmod 600 "$config_tmp" "$password_tmp"
mv -f -- "$config_tmp" "$CONFIG_FILE"
mv -f -- "$password_tmp" "$PASSWORD_FILE"
config_tmp=""
password_tmp=""
unset payment_password payment_password_confirm

printf '\n[4/4] 安装 cron 定时任务\n'
"$CRON_SCRIPT" --install

printf '\n[OK] 自动充值已配置完成。\n'
printf '配置文件：%s\n' "$CONFIG_FILE"
printf '日志文件：%s/electricity-cron.log\n' "$CONFIG_DIR"
printf '立即检查：%s --run\n' "$CRON_SCRIPT"
printf '余额不足时，请在终端运行：%s --recover\n' "$CRON_SCRIPT"
printf '[WARN] 支付密码保存在仅当前用户可读的文件中；请保护好系统账户。\n'
