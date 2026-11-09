import { defineConfig } from 'vite';
export default defineConfig({base:'./', server:{proxy:{'/api':'http://127.0.0.1:8787'}}, build:{chunkSizeWarningLimit:1100}});
