import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8080',
        configure(proxy) {
          proxy.on('proxyReq', proxyRequest => {
            proxyRequest.setHeader('Origin', 'http://127.0.0.1:8080')
          })
        },
      },
      '/measurements.json': 'http://127.0.0.1:8080',
    },
  },
})
