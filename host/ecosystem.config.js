// pm2 config for the compiler host: `pm2 start ecosystem.config.js` (see README.md).
// Run it from this folder. Set the publish password first, in the same shell:
//   PowerShell:  $env:QUANTUM_PUBLISH_TOKEN = "<your long random string>"
// Without it the host still serves downloads, but publishing is disabled.

const hostEnv = {
  QUANTUM_HOST_PORT: "8080", // plain HTTP, local only; Caddy puts HTTPS in front of it
  QUANTUM_HOST_BIND: "127.0.0.1",
  QUANTUM_HOST_DATA: "./data", // releases and latest.json are stored here
};
if (process.env.QUANTUM_PUBLISH_TOKEN) {
  hostEnv.QUANTUM_PUBLISH_TOKEN = process.env.QUANTUM_PUBLISH_TOKEN;
}

module.exports = {
  apps: [
    {
      name: "quantum-host",
      script: "./quantum-host.exe", // qrun --native host/server.sa quantum-host.exe
      interpreter: "none", // it is a program, not a node script
      cwd: __dirname,
      autorestart: true,
      restart_delay: 2000,
      env: hostEnv,
    },
    {
      name: "quantum-caddy",
      script: "./caddy.exe", // https://caddyserver.com/download (Windows amd64)
      args: "run --config Caddyfile --adapter caddyfile",
      interpreter: "none",
      cwd: __dirname,
      autorestart: true,
      restart_delay: 2000,
    },
  ],
};
