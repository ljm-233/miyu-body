# 常驻

三个文件，装到系统对应的位置：

```sh
cp deploy/miyu-body-wrapper ~/.local/bin/miyu-body && chmod +x ~/.local/bin/miyu-body
cp deploy/miyu-body-ctl     ~/.local/bin/miyu-body-ctl && chmod +x ~/.local/bin/miyu-body-ctl
cp deploy/miyu-body.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now miyu-body
```

`miyu-body-ctl start|stop|restart|status|toggle|logs`

**为什么要有 wrapper**：`build/miyu-wayland` 每次重编都会变，而且它用相对路径读 `FrameworkShaders/`，
cwd 不对就是一屏 `Failed to load vertex shader`。wrapper 负责 `cd` 进去、检查二进制/着色器/wayland socket，
任何一项不对就当场报错退出。

**开机自启依赖**：`greetd → niri-session → systemctl --user --wait start niri.service`，
`niri.service` 是 `BindsTo=graphical-session.target`，所以 target 起来它必然 active，
这条服务 `WantedBy=graphical-session.target` 就跟着起来。退登时 `niri-shutdown.target` 会连它一起停。

**注意**：`WAYLAND_DISPLAY=wayland-1` 是本机实测值，换机器要改（`ls /run/user/$UID/wayland-*`）。
