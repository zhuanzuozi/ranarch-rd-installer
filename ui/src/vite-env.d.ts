/// <reference types="vite/client" />

/**
 * 构建时注入的常量（见 vite.config.ts 的 define）：
 * 版本号来自 package.json，构建日期取打包当天。
 */
declare const __APP_VERSION__: string;
declare const __BUILD_DATE__: string;
