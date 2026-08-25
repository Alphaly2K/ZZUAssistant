#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

CONFIG_DIR=${ZZUASSISTANT_AUTOMATION_DIR:-${XDG_CONFIG_HOME:-"$HOME/.config"}/zzu-assistant}
CONFIG_FILE=$CONFIG_DIR/electricity-auto-recharge.conf
PASSWORD_FILE=$CONFIG_DIR/electricity-payment-password
PENDING_FILE=$CONFIG_DIR/electricity-topup-pending.conf
LOG_FILE=$CONFIG_DIR/electricity-cron.log
LOCK_DIR=$CONFIG_DIR/electricity-cron.lock
CRON_BEGIN='# BEGIN ZZUASSISTANT ELECTRICITY'
CRON_END='# END ZZUASSISTANT ELECTRICITY'

usage() {
    cat <<'EOF'
用法：electricity_cron.sh <命令>

命令：
  --run       立即检查并在需要时充值电费
  --cron      由 cron 执行检查
  --recover   交互式创建校园卡充值订单并恢复 cron
  --install   安装或恢复 cron 任务
  --pause     暂停 cron 任务
  --status    显示任务状态
  -h, --help  显示此帮助
EOF
}

info() { printf '[INFO] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*" >&2; }
die() { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

load_config() {
    [[ -r $CONFIG_FILE ]] || die "找不到配置文件；请先运行 electricity_setup.sh：$CONFIG_FILE"
    # 文件由 setup 脚本创建、权限为 600，只应由当前用户修改。
    # shellcheck disable=SC1090
    source "$CONFIG_FILE"

    : "${ZZUASSISTANT_BIN:?配置缺少 ZZUASSISTANT_BIN}"
    : "${ZZUASSISTANT_USER:?配置缺少 ZZUASSISTANT_USER}"
    : "${ZZU_ELECTRICITY_THRESHOLD_YUAN:?配置缺少 ZZU_ELECTRICITY_THRESHOLD_YUAN}"
    : "${ZZU_ELECTRICITY_RECHARGE_YUAN:?配置缺少 ZZU_ELECTRICITY_RECHARGE_YUAN}"
    : "${ZZU_CRON_INTERVAL_MINUTES:?配置缺少 ZZU_CRON_INTERVAL_MINUTES}"
    : "${ZZU_CRON_SCRIPT:?配置缺少 ZZU_CRON_SCRIPT}"
    export ZZUASSISTANT_USER
    [[ -x $ZZUASSISTANT_BIN ]] || die "ZZUAssistant 不可执行：$ZZUASSISTANT_BIN"
}

cron_without_our_block() {
    { crontab -l 2>/dev/null || true; } |
        awk -v begin="$CRON_BEGIN" -v end="$CRON_END" '
            $0 == begin { hidden = 1; saved = $0 ORS; next }
            hidden {
                saved = saved $0 ORS
                if ($0 == end) { hidden = 0; saved = "" }
                next
            }
            { print }
            END { if (hidden) printf "%s", saved }
        '
}

cron_quote() {
    local value=${1//\'/\'\\\'\'}
    printf "'%s'" "$value"
}

install_cron() {
    load_config
    command -v crontab >/dev/null 2>&1 || die '系统中没有 crontab。'
    [[ $ZZU_CRON_INTERVAL_MINUTES =~ ^[0-9]+$ ]] &&
        ((ZZU_CRON_INTERVAL_MINUTES >= 1 && ZZU_CRON_INTERVAL_MINUTES <= 59)) ||
        die '配置中的检查间隔无效。'
    [[ $ZZU_CRON_SCRIPT != *%* && $LOG_FILE != *%* ]] ||
        die '脚本或日志路径不能包含 %（cron 会将其解释为换行）。'

    local tmp script_q log_q config_dir_q
    tmp=$(mktemp)
    script_q=$(cron_quote "$ZZU_CRON_SCRIPT")
    log_q=$(cron_quote "$LOG_FILE")
    config_dir_q=$(cron_quote "$CONFIG_DIR")
    cron_without_our_block >"$tmp"
    {
        printf '%s\n' "$CRON_BEGIN"
        printf '*/%s * * * * ZZUASSISTANT_AUTOMATION_DIR=%s %s --cron >> %s 2>&1\n' \
            "$ZZU_CRON_INTERVAL_MINUTES" "$config_dir_q" "$script_q" "$log_q"
        printf '%s\n' "$CRON_END"
    } >>"$tmp"
    crontab "$tmp"
    rm -f -- "$tmp"
    info "cron 已启动，每 $ZZU_CRON_INTERVAL_MINUTES 分钟检查一次。"
}

pause_cron() {
    command -v crontab >/dev/null 2>&1 || die '系统中没有 crontab。'
    local tmp
    tmp=$(mktemp)
    cron_without_our_block >"$tmp"
    crontab "$tmp"
    rm -f -- "$tmp"
    warn 'cron 已暂停。'
}

kv_value() {
    local input=$1 wanted=$2 key value
    while IFS='=' read -r key value; do
        if [[ $key == "$wanted" ]]; then
            printf '%s' "$value"
            return 0
        fi
    done <<<"$input"
    return 1
}

is_number() {
    [[ $1 =~ ^-?([0-9]+([.][0-9]*)?|[.][0-9]+)$ ]]
}

less_than() {
    awk -v left="$1" -v right="$2" 'BEGIN { exit !(left + 0 < right + 0) }'
}

at_least() {
    awk -v left="$1" -v right="$2" 'BEGIN { exit !(left + 0.000001 >= right + 0) }'
}

subtract_money() {
    awk -v left="$1" -v right="$2" 'BEGIN { printf "%.2f", left - right }'
}

multiply_money() {
    awk -v amount="$1" -v count="$2" 'BEGIN { printf "%.2f", amount * count }'
}

meter_value() {
    awk -v quantity="$1" -v price="$2" 'BEGIN { printf "%.2f", quantity * price }'
}

minimum_topup() {
    local deficit=$1 option
    # 当前校园卡服务端公布的充值档位均为整十金额。
    for option in 10 20 50 100; do
        if at_least "$option" "$deficit"; then
            printf '%s' "$option"
            return 0
        fi
    done
    return 1
}

query_ecard_balance() {
    local output balance
    output=$("$ZZUASSISTANT_BIN" ecard balance --porcelain)
    balance=$(kv_value "$output" balance_yuan) || return 1
    is_number "$balance" || return 1
    printf '%s' "$balance"
}

write_pending() {
    local required=$1 balance=$2 topup=$3 meters=$4 tmp
    tmp=$(mktemp "$CONFIG_DIR/.electricity-pending.XXXXXX")
    {
        printf 'PENDING_REQUIRED_YUAN=%q\n' "$required"
        printf 'PENDING_ECARD_BALANCE_YUAN=%q\n' "$balance"
        printf 'PENDING_TOPUP_YUAN=%q\n' "$topup"
        printf 'PENDING_METERS=%q\n' "$meters"
        printf 'PENDING_CREATED_AT=%q\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } >"$tmp"
    chmod 600 "$tmp"
    mv -f -- "$tmp" "$PENDING_FILE"
}

acquire_lock() {
    mkdir -p "$CONFIG_DIR"
    if ! mkdir "$LOCK_DIR" 2>/dev/null; then
        local owner=""
        [[ -r $LOCK_DIR/pid ]] && IFS= read -r owner <"$LOCK_DIR/pid"
        if [[ $owner =~ ^[0-9]+$ ]] && kill -0 "$owner" 2>/dev/null; then
            warn '已有一次余额检查正在运行，本次跳过。'
            return 1
        fi
        warn '发现上次异常退出遗留的锁，正在清理。'
        rm -f -- "$LOCK_DIR/pid"
        rmdir -- "$LOCK_DIR" 2>/dev/null || die "无法清理锁目录：$LOCK_DIR"
        mkdir "$LOCK_DIR" || die "无法创建锁目录：$LOCK_DIR"
    fi
    printf '%s\n' "$$" >"$LOCK_DIR/pid"
    cleanup_lock() {
        rm -f -- "$LOCK_DIR/pid"
        rmdir -- "$LOCK_DIR" 2>/dev/null || true
    }
    trap cleanup_lock EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
}

run_check() {
    load_config
    acquire_lock || return 0

    if [[ -f $PENDING_FILE ]]; then
        warn "存在待处理的校园卡充值，请运行：$ZZU_CRON_SCRIPT --recover"
        return 3
    fi

    local output lighting_quantity lighting_price air_quantity air_price
    local lighting_yuan air_yuan ecard_balance required deficit topup
    local -a low_meters=()

    output=$("$ZZUASSISTANT_BIN" electricity show --porcelain)
    lighting_quantity=$(kv_value "$output" lighting_quantity_kwh) || die '电费查询结果缺少 lighting_quantity_kwh。'
    lighting_price=$(kv_value "$output" lighting_price_yuan_per_kwh) || die '电费查询结果缺少 lighting_price_yuan_per_kwh。'
    air_quantity=$(kv_value "$output" air_conditioning_quantity_kwh) || die '电费查询结果缺少 air_conditioning_quantity_kwh。'
    air_price=$(kv_value "$output" air_conditioning_price_yuan_per_kwh) || die '电费查询结果缺少 air_conditioning_price_yuan_per_kwh。'
    for value in "$lighting_quantity" "$lighting_price" "$air_quantity" "$air_price"; do
        is_number "$value" || die "电费查询返回了无效数字：$value"
    done

    lighting_yuan=$(meter_value "$lighting_quantity" "$lighting_price")
    air_yuan=$(meter_value "$air_quantity" "$air_price")
    info "照明余额约 ${lighting_yuan} 元，空调余额约 ${air_yuan} 元，阈值 ${ZZU_ELECTRICITY_THRESHOLD_YUAN} 元。"

    less_than "$lighting_yuan" "$ZZU_ELECTRICITY_THRESHOLD_YUAN" && low_meters+=(lighting)
    less_than "$air_yuan" "$ZZU_ELECTRICITY_THRESHOLD_YUAN" && low_meters+=(air)
    if ((${#low_meters[@]} == 0)); then
        info '电费余额正常，无需充值。'
        return 0
    fi

    ecard_balance=$(query_ecard_balance) || die '无法读取校园卡余额。'
    required=$(multiply_money "$ZZU_ELECTRICITY_RECHARGE_YUAN" "${#low_meters[@]}")
    info "本次需从校园卡支付 ${required} 元；校园卡余额 ${ecard_balance} 元。"

    if ! at_least "$ecard_balance" "$required"; then
        deficit=$(subtract_money "$required" "$ecard_balance")
        if ! topup=$(minimum_topup "$deficit"); then
            write_pending "$required" "$ecard_balance" 0 "$(IFS=,; printf '%s' "${low_meters[*]}")"
            pause_cron
            die "校园卡至少还需 ${deficit} 元，超过单笔 100 元充值档位；请手动充值后运行 $ZZU_CRON_SCRIPT --recover。"
        fi
        write_pending "$required" "$ecard_balance" "$topup" "$(IFS=,; printf '%s' "${low_meters[*]}")"
        pause_cron
        warn "校园卡余额不足，需充值 ${topup} 元（覆盖 ${deficit} 元缺口）。"
        warn "请在终端运行：$ZZU_CRON_SCRIPT --recover"
        return 3
    fi

    [[ -r $PASSWORD_FILE ]] || die "找不到校园卡支付密码文件：$PASSWORD_FILE"
    IFS= read -r ZZUASSISTANT_ECARD_PAYMENT_PASSWORD <"$PASSWORD_FILE" || true
    [[ -n ${ZZUASSISTANT_ECARD_PAYMENT_PASSWORD:-} ]] || die '校园卡支付密码为空。'
    export ZZUASSISTANT_ECARD_PAYMENT_PASSWORD

    local meter
    for meter in "${low_meters[@]}"; do
        info "正在为 $meter 充值 ${ZZU_ELECTRICITY_RECHARGE_YUAN} 元。"
        if ! "$ZZUASSISTANT_BIN" electricity recharge "$meter" \
            "$ZZU_ELECTRICITY_RECHARGE_YUAN" --yes; then
            unset ZZUASSISTANT_ECARD_PAYMENT_PASSWORD
            pause_cron
            die "$meter 电费充值结果异常。为避免重复扣款，cron 已暂停；核对余额后请运行 $ZZU_CRON_SCRIPT --install。"
        fi
    done
    unset ZZUASSISTANT_ECARD_PAYMENT_PASSWORD
    info '本次自动充值完成。'
}

recover() {
    load_config
    [[ -t 0 && -t 1 ]] || die '--recover 必须在交互式终端中运行。'
    [[ -r $PENDING_FILE ]] || die '目前没有待处理的校园卡充值。'
    # shellcheck disable=SC1090
    source "$PENDING_FILE"
    : "${PENDING_REQUIRED_YUAN:?待处理文件缺少所需金额}"

    local current deficit topup
    current=$(query_ecard_balance) || die '无法读取校园卡余额。'
    info "继续电费充值需要 ${PENDING_REQUIRED_YUAN} 元，校园卡当前余额 ${current} 元。"

    if ! at_least "$current" "$PENDING_REQUIRED_YUAN"; then
        deficit=$(subtract_money "$PENDING_REQUIRED_YUAN" "$current")
        topup=$(minimum_topup "$deficit") ||
            die "至少还需 ${deficit} 元，超过单笔 100 元充值档位；请先手动充值校园卡。"
        read -r -p "按回车创建 ${topup} 元校园卡充值订单（Ctrl+C 取消）……"
        "$ZZUASSISTANT_BIN" ecard recharge "$topup" --yes
        read -r -p '支付完成后按回车验证校园卡余额并恢复 cron……'
        current=$(query_ecard_balance) || die '无法读取校园卡余额。'
        if ! at_least "$current" "$PENDING_REQUIRED_YUAN"; then
            die "校园卡余额仍为 ${current} 元，尚不足 ${PENDING_REQUIRED_YUAN} 元；cron 保持暂停。"
        fi
    else
        info '校园卡余额已经足够，无需创建充值订单。'
    fi

    rm -f -- "$PENDING_FILE"
    install_cron
    info '立即重新执行一次电费余额检查。'
    run_check
}

show_status() {
    load_config
    if crontab -l 2>/dev/null | grep -Fqx "$CRON_BEGIN"; then
        printf 'cron=running\n'
    else
        printf 'cron=paused\n'
    fi
    if [[ -f $PENDING_FILE ]]; then
        printf 'topup=pending\n'
    else
        printf 'topup=none\n'
    fi
    printf 'username=%s\n' "$ZZUASSISTANT_USER"
    printf 'threshold_yuan=%s\n' "$ZZU_ELECTRICITY_THRESHOLD_YUAN"
    printf 'recharge_yuan=%s\n' "$ZZU_ELECTRICITY_RECHARGE_YUAN"
}

case ${1:---cron} in
    --run|--cron) run_check ;;
    --recover) recover ;;
    --install) install_cron ;;
    --pause) pause_cron ;;
    --status) show_status ;;
    -h|--help) usage ;;
    *) usage >&2; exit 2 ;;
esac
