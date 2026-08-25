# 电费自动充值示例脚本

macOS/Linux 等提供 `cron` 的系统可以使用仓库中的两个示例脚本：

```bash
bash ./scripts/electricity_setup.sh --bin /完整路径/ZZUAssistant
```

安装脚本会依次登录超级 App、设置照明和空调房间、询问电费余额阈值、每次
充值金额和校园卡支付密码，并安装定时任务。默认每 5 分钟检查一次，可用
`--interval 10` 修改间隔。电费余额按“剩余电量 × 当前单价”换算为元。

校园卡余额足够时，定时任务会为低于阈值的照明或空调账户自动充值。如果校园
卡余额不足，任务会先暂停，并在日志中给出恢复命令：

```bash
bash ./scripts/electricity_cron.sh --recover
```

恢复模式会等待用户按回车，再创建足以覆盖缺口的最小可用整十金额校园卡充值
订单并显示二维码；支付完成并再次按回车后，它会验证余额、恢复 cron，并立即
重试一次电费充值。如果电费支付返回异常，脚本也会暂停任务，避免网络故障造成
重复扣款；核对实际余额后可手动恢复。其他维护命令：

```bash
bash ./scripts/electricity_cron.sh --run       # 立即检查
bash ./scripts/electricity_cron.sh --status    # 查看状态
bash ./scripts/electricity_cron.sh --pause     # 暂停任务
bash ./scripts/electricity_cron.sh --install   # 恢复任务
```