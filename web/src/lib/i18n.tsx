import {
  createContext,
  useContext,
  useEffect,
  useState,
  type ReactNode,
} from "react";

export type Lang = "en" | "zh";

type Dict = Record<string, string>;

const en: Dict = {
  "app.title": "Radxa Linkr Debugger",
  "app.subtitle": "Hardware debug & power controller",

  "status.online": "online",
  "status.offline": "offline",
  "status.auto": "auto",
  "status.live": "live",
  "status.temp": "Temp",
  "status.cpu": "CPU",
  "status.heap": "free",
  "status.refresh": "Refresh",

  "banner.unreachable": "Board unreachable",
  "banner.unreachable.detail":
    "Cannot reach the firmware HTTP API — connect the USB cable, ensure the NCM interface is up at 172.29.203.1, and that the dev proxy targets the board.",
  "banner.retry": "Retry",

  "loading": "Loading board state…",

  "advanced.title": "Advanced & recovery",
  "advanced.subtitle": "GPIO, watchdog status and bootloader tools",
  "advanced.count": "3 tools",

  "power.title": "Power Outputs",
  "power.subtitle": "Controllable rails",
  "power.none": "No power outputs reported.",
  "power.combined.title": "Power & current",
  "power.combined.subtitle": "Rail control with live current measurements",
  "power.combined.none": "No power rails or current readings reported.",
  "power.monitorOnly": "monitor only",
  "power.chart.title": "Power trend",
  "power.chart.metric": "Chart metric",
  "power.chart.current": "Current",
  "power.chart.power": "Power",
  "power.chart.last": "Last",
  "power.chart.now": "now",
  "power.chart.collecting": "Collecting samples…",
  "power.chart.trend": "trend",
  "power.locked": "locked",
  "power.on": "ON",
  "power.off": "OFF",

  "switch.title": "Routing Switches",
  "switch.subtitle": "SD and USB mux routes",
  "switch.sd": "SD / TF card",
  "switch.sd.desc": "Route the microSD between targets",
  "switch.sd.target": "Target",
  "switch.sd.usb": "USB reader",
  "switch.usb": "USB data",
  "switch.usb.desc": "Route the target USB to PC or target MCU",
  "switch.usb.pc": "PC",
  "switch.usb.target": "Target",
  "switch.route.sbc": "SBC",
  "switch.route.pc": "PC",

  "adc.title": "Current Monitoring",
  "adc.subtitle": "INA139 shunt sense per rail",
  "adc.none": "No current readings.",
  "adc.raw": "raw",
  "adc.disabled": "rail off — showing zero",

  "gpio.title": "Safe GPIO",
  "gpio.subtitle": "Allowlisted user pins",
  "gpio.none": "No safe GPIOs reported.",
  "gpio.input": "input",
  "gpio.output": "output",
  "gpio.high": "high",
  "gpio.low": "low",
  "gpio.set": "Set",
  "gpio.current": "current",

  "watchdog.title": "Watchdog",
  "watchdog.subtitle": "Firmware-supervised recovery",
  "watchdog.healthy": "healthy",
  "watchdog.unhealthy": "unhealthy",
  "watchdog.armed": "armed",
  "watchdog.automatic": "automatic",
  "watchdog.bootTimeout": "bootloader on timeout",
  "watchdog.supported": "Supported",
  "watchdog.timeout": "Timeout",
  "watchdog.failing": "Failing service",
  "watchdog.note": "Watchdog is owned by firmware; it cannot be fed manually.",

  "boot.title": "Boot / Recovery",
  "boot.subtitle": "ROM BOOTSEL entry",
  "boot.desc":
    "Reboot the board into its ROM bootloader for flashing. CDC ACM ",
  "boot.desc2": " shell command is an independent fallback.",
  "boot.confirm":
    "Enter RP2040/RP2350 BOOTSEL? The board will reboot into ROM bootloader in ~250 ms.",
  "boot.failed": "BOOTSEL failed: ",
  "boot.enter": "Enter BOOTSEL",
  "boot.rebooting": "Rebooting…",
  "boot.done": "BOOTSEL requested — board should now enumerate as a mass-storage device.",

  "serial.title": "Target Serial Console",
  "serial.subtitle": "CH347F UART to the target board",
  "serial.disconnect": "Disconnect",
  "serial.webSerial": "Web Serial",
  "serial.bridge": "Bridge",
  "serial.connect":
    "Connect to the target UART exposed by the onboard CH347F.",
  "serial.webSerialHint":
    " Web Serial is preferred (Chromium); the Node bridge is the fallback when the OS driver owns the port.",
  "serial.noWebSerial":
    " This browser has no Web Serial API — run `npm run serial-bridge` and use the Bridge button.",
  "serial.noConnection": "No connection yet.",
  "serial.placeholder": "type and Enter to send…",
  "serial.bridgeError":
    "Bridge WebSocket error — is `npm run serial-bridge` running?",

  "footer.endpoint": "Firmware",
  "footer.polling": "polling 2s",
  "footer.live": "WebSocket live",

  "theme.toggle": "Switch theme",
  "theme.light": "Light",
  "theme.dark": "Dark",
  "lang.toggle": "中文",
};

const zh: Dict = {
  "app.title": "Radxa Linkr 调试器",
  "app.subtitle": "硬件调试与电源控制器",

  "status.online": "在线",
  "status.offline": "离线",
  "status.auto": "自动",
  "status.live": "实时",
  "status.temp": "温度",
  "status.cpu": "CPU",
  "status.heap": "空闲",
  "status.refresh": "刷新",

  "banner.unreachable": "无法连接开发板",
  "banner.unreachable.detail":
    "无法访问固件 HTTP 接口 — 请连接 USB 线缆，确认 NCM 网卡已在 172.29.203.1 上线，且 dev 代理指向该板。",
  "banner.retry": "重试",

  "loading": "正在加载板子状态…",

  "advanced.title": "高级与恢复",
  "advanced.subtitle": "GPIO、看门狗状态与引导工具",
  "advanced.count": "3 项工具",

  "power.title": "电源输出",
  "power.subtitle": "可控制电源轨",
  "power.none": "未报告任何电源输出。",
  "power.combined.title": "电源与电流",
  "power.combined.subtitle": "电源轨控制与实时电流测量",
  "power.combined.none": "未报告电源轨或电流读数。",
  "power.monitorOnly": "仅监测",
  "power.chart.title": "电源趋势",
  "power.chart.metric": "图表指标",
  "power.chart.current": "电流",
  "power.chart.power": "功率",
  "power.chart.last": "最近",
  "power.chart.now": "现在",
  "power.chart.collecting": "正在采集样本…",
  "power.chart.trend": "趋势",
  "power.locked": "已锁定",
  "power.on": "开",
  "power.off": "关",

  "switch.title": "路由开关",
  "switch.subtitle": "SD 与 USB 多路选择",
  "switch.sd": "SD / TF 卡",
  "switch.sd.desc": "将 microSD 路由到不同目标",
  "switch.sd.target": "目标",
  "switch.sd.usb": "USB 读卡器",
  "switch.usb": "USB 数据",
  "switch.usb.desc": "将目标 USB 路由到 PC 或目标 MCU",
  "switch.usb.pc": "PC",
  "switch.usb.target": "目标",
  "switch.route.sbc": "SBC",
  "switch.route.pc": "PC",

  "adc.title": "电流监控",
  "adc.subtitle": "INA139 每路分流采样",
  "adc.none": "无电流读数。",
  "adc.raw": "原始",
  "adc.disabled": "电源轨已关闭 — 显示为 0",

  "gpio.title": "安全 GPIO",
  "gpio.subtitle": "允许的的用户引脚",
  "gpio.none": "未报告任何安全 GPIO。",
  "gpio.input": "输入",
  "gpio.output": "输出",
  "gpio.high": "高",
  "gpio.low": "低",
  "gpio.set": "设置",
  "gpio.current": "当前",

  "watchdog.title": "看门狗",
  "watchdog.subtitle": "固件监管的自动恢复",
  "watchdog.healthy": "正常",
  "watchdog.unhealthy": "异常",
  "watchdog.armed": "已武装",
  "watchdog.automatic": "自动",
  "watchdog.bootTimeout": "超时进引导",
  "watchdog.supported": "支持",
  "watchdog.timeout": "超时",
  "watchdog.failing": "故障服务",
  "watchdog.note": "看门狗由固件持有，无法手动喂狗。",

  "boot.title": "启动 / 恢复",
  "boot.subtitle": "ROM BOOTSEL 入口",
  "boot.desc": "将板子重启进入 ROM 引导加载程序以烧录固件。CDC ACM ",
  "boot.desc2": " shell 命令是独立的回退方式。",
  "boot.confirm":
    "进入 RP2040/RP2350 BOOTSEL？板子将在约 250 毫秒后重启进入 ROM 引导加载程序。",
  "boot.failed": "BOOTSEL 失败：",
  "boot.enter": "进入 BOOTSEL",
  "boot.rebooting": "重启中…",
  "boot.done": "已请求 BOOTSEL — 板子现在应枚举为大容量存储设备。",

  "serial.title": "目标串口控制台",
  "serial.subtitle": "板载 CH347F 到目标板的 UART",
  "serial.disconnect": "断开",
  "serial.webSerial": "Web 串口",
  "serial.bridge": "桥接",
  "serial.connect": "连接到板载 CH347F 暴露的目标 UART。",
  "serial.webSerialHint":
    " 优先使用 Web Serial（Chromium）；当系统驱动占用端口时改用 Node 桥接作为回退。",
  "serial.noWebSerial":
    " 当前浏览器不支持 Web Serial API — 请运行 `npm run serial-bridge` 并使用桥接按钮。",
  "serial.noConnection": "尚未连接。",
  "serial.placeholder": "输入内容后按回车发送…",
  "serial.bridgeError": "桥接 WebSocket 错误 — 是否正在运行 `npm run serial-bridge`？",

  "footer.endpoint": "固件",
  "footer.polling": "轮询 2 秒",
  "footer.live": "WebSocket 实时",

  "theme.toggle": "切换主题",
  "theme.light": "明亮",
  "theme.dark": "暗色",
  "lang.toggle": "EN",
};

const dicts: Record<Lang, Dict> = { en, zh };

interface I18nContextValue {
  lang: Lang;
  setLang: (l: Lang) => void;
  t: (key: string) => string;
}

const I18nContext = createContext<I18nContextValue | null>(null);

function getInitialLang(): Lang {
  if (typeof localStorage !== "undefined") {
    const saved = localStorage.getItem("lang");
    if (saved === "en" || saved === "zh") return saved;
  }
  if (typeof navigator !== "undefined" && navigator.language.startsWith("zh")) {
    return "zh";
  }
  return "en";
}

export function LanguageProvider({ children }: { children: ReactNode }) {
  const [lang, setLangState] = useState<Lang>(getInitialLang);

  useEffect(() => {
    localStorage.setItem("lang", lang);
    document.documentElement.lang = lang === "zh" ? "zh-CN" : "en";
  }, [lang]);

  const setLang = (l: Lang) => setLangState(l);
  const t = (key: string) => dicts[lang][key] ?? dicts.en[key] ?? key;

  return (
    <I18nContext.Provider value={{ lang, setLang, t }}>{children}</I18nContext.Provider>
  );
}

export function useI18n(): I18nContextValue {
  const ctx = useContext(I18nContext);
  if (!ctx) throw new Error("useI18n must be used within LanguageProvider");
  return ctx;
}
