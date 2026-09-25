import { defineConfig } from 'vite';
import { resolve } from 'path';
import { readFileSync } from 'fs';

// 版本号取自 package.json（单一来源），构建日期取打包当天 —— 两者由 define 注入到前端，
// 这样「关于」里的版本 / 日期永远和真实产物一致，不用手改。
const pkg = JSON.parse(readFileSync(resolve(__dirname, 'package.json'), 'utf-8')) as { version: string };
// 用本地日期而不是 UTC：凌晨构建时两者会差一天
const now = new Date();
const buildDate = [
  now.getFullYear(),
  String(now.getMonth() + 1).padStart(2, '0'),
  String(now.getDate()).padStart(2, '0'),
].join('-');

export default defineConfig({
  root: '.',
  base: './',
  publicDir: 'public',
  define: {
    __APP_VERSION__: JSON.stringify(pkg.version),
    __BUILD_DATE__: JSON.stringify(buildDate),
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    rollupOptions: {
      input: {
        main: resolve(__dirname, 'index.html'),
      },
    },
  },
  resolve: {
    alias: {
      '@': resolve(__dirname, 'src'),
    },
  },
  server: {
    port: 5173,
  },
});
