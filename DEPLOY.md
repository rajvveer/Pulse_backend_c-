# Deploying the Pulse C++ backend (production, Docker)

This deploys the C++ backend as a Docker container on a Linux server, with Redis
alongside it and (optionally) Caddy for automatic HTTPS. MongoDB stays on Atlas.

> **Heads up:** the image rebuilds the binary **from source** on Linux (vcpkg
> compiles Drogon, mongo-cxx-driver, etc.). The **first** build takes ~20–40 min
> and needs a server with **≥ 2 vCPU and ≥ 4 GB RAM** (8 GB is comfortable — the
> C++ compile is memory-hungry). After the first build, BuildKit caches the deps.
> A tiny 1 GB VPS will OOM during the build — build on a bigger box (or locally)
> and push the image, then run it on the small box.

---

## 0. What you need

- A Linux server (VPS / cloud VM) with a public IP. Ubuntu 22.04/24.04 is fine.
- A domain name for HTTPS (e.g. `api.yourdomain.com`) with an **A record** pointed
  at the server's IP. (Optional — you can run plain HTTP on the IP to start.)
- Your secrets: the same values from your local `.env` (Mongo URI, JWT secrets,
  LiveKit keys, Firebase, Brevo, Cloudinary, Tenor).
- Docker + the compose plugin on the server (install below).

---

## 1. Install Docker on the server

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER      # so you can run docker without sudo
newgrp docker                       # apply the group now (or log out/in)
docker --version && docker compose version
```

---

## 2. Get the code onto the server

Copy the `pulse-backend-cpp` directory to the server. Either:

```bash
# from your machine (adjust paths/host):
rsync -av --exclude build --exclude vcpkg_installed --exclude '.env' \
  ./pulse-backend-cpp/  user@SERVER_IP:/opt/pulse-backend/
```

…or `git clone` it if it's in a repo. The `.dockerignore` already keeps the
Windows `build/`, DLLs, and `.env` out of the image context, so a plain copy is
fine too.

```bash
cd /opt/pulse-backend
```

---

## 3. Configure secrets

```bash
cp .env.production.example .env.production
nano .env.production        # fill in EVERY CHANGE_ME / placeholder
```

Must-haves for the server to even start (`NODE_ENV=production` fails fast
without them):
- `MONGO_URI` — **keep `/test` in the path** (real data lives in DB `test`).
- `JWT_SECRET`, `JWT_REFRESH_SECRET`, `TEMP_JWT_SECRET` — generate with
  `openssl rand -hex 48`. (You can reuse your existing dev values so current
  logins keep working, or rotate them — rotating logs everyone out.)
- `LIVEKIT_API_KEY` / `LIVEKIT_API_SECRET` / `LIVEKIT_WS_URL` — for calling.
- `SERVER_URL` — your public URL (e.g. `https://api.yourdomain.com`).

Authentication providers are fail-closed feature flags:
- Set `ENABLE_FIREBASE_AUTH=true` only after configuring the Firebase service
  account fields (or mounting its JSON file and setting
  `FIREBASE_SERVICE_ACCOUNT_PATH`).
- Set `ENABLE_GOOGLE_LOGIN=true` only after setting `GOOGLE_CLIENT_ID` or the
  comma-separated `GOOGLE_OAUTH_CLIENT_IDS` allowlist.

`REDIS_*` are set automatically by compose to the `redis` service — leave them
commented in `.env.production`.

> **Firebase note:** the inline `FIREBASE_PRIVATE_KEY` must keep its `\n`
> escapes exactly as in your dev `.env` (the server un-escapes them). Or mount a
> service-account JSON and set `FIREBASE_SERVICE_ACCOUNT_PATH` instead.

---

## 4. Build + run (HTTP first, to confirm it works)

```bash
docker compose up -d --build       # first build is the slow one (~20–40 min)
docker compose logs -f backend     # watch startup
```

You want to see, with **no** "Missing required environment variables" warning:

```
🚀 Pulse Backend Server running on port 3000
🌍 Environment: production
✅ Redis connected successfully
```

Test it:
```bash
curl http://127.0.0.1:3000/health          # {"status":"OK",...}
curl http://127.0.0.1:3000/health/ready    # checks Mongo + Redis
```

If `/health/ready` is OK, the server is fully live (DB + Redis reachable).

---

## 5. Add HTTPS (Caddy — automatic certs)

Calling + the app should run over **HTTPS/WSS** in production (mobile OSes and
WebRTC expect it). Caddy does this with zero cert hassle.

1. Edit `Caddyfile` — replace `api.yourdomain.com` with your real domain (DNS A
   record must already point at the server).
2. The compose file binds backend port 3000 to host loopback only. Do not change
   that binding to `0.0.0.0`; public traffic must enter through Caddy HTTPS.
3. Bring it up with the TLS profile:

```bash
docker compose --profile tls up -d --build
```

Caddy fetches a Let's Encrypt cert automatically and proxies `https://api.yourdomain.com`
→ backend:3000. WebSockets (`/ws` — chat + call signaling) pass through with no
extra config. Verify:

```bash
curl https://api.yourdomain.com/health
```

> Open ports 80 and 443 on the server's firewall / cloud security group.

---

## 6. Point the mobile app at the deployed backend

In `pulse-frontend/app.json`, change the dev URLs to your domain:

```jsonc
"extra": {
  "apiUrl": "https://api.yourdomain.com/api/v1",
  "socketUrl": "https://api.yourdomain.com",   // app converts http(s)->ws(s) for /ws
  "livekitWsUrl": "wss://pulse-1fgmfs6y.livekit.cloud",
  ...
}
```

Then rebuild the app (the dev client / a release build) so it bundles the new
URLs. Now the phones reach the backend from anywhere (cellular included), not
just your LAN.

---

## 7. Operating it

```bash
docker compose ps                       # status
docker compose logs -f backend          # tail logs
docker compose restart backend          # restart after an env change
docker compose up -d --build backend    # rebuild after a code change
docker compose down                     # stop everything (keeps volumes)
```

`restart: unless-stopped` means the stack comes back automatically after a
server reboot (Docker starts on boot). Redis data persists in the `redis-data`
volume.

---

## 8. Troubleshooting

**`Missing required environment variables` / all REST calls return 401**
The container can't see your JWT secrets. Confirm `.env.production` is filled in
and referenced (`env_file` in compose). This is the #1 gotcha — the server boots
with random fallback secrets when the env is missing, so every token fails to
verify (`AUTH_FAILED`). WebSockets may still connect; REST 401s.

**Outbound HTTPS fails with `BadServerAddress` (email OTP / Twilio / Cloudinary)**
This is the c-ares DNS issue we hit on Windows. It normally does **not** recur in
a Linux container (c-ares reads `/etc/resolv.conf`). If it does, rebuild Drogon's
trantor without c-ares via a vcpkg overlay port:
1. Create `vcpkg-overlays/trantor/` with a `portfile.cmake` that passes
   `-DBUILD_C-ARES=OFF` to trantor's CMake (copy the upstream trantor port and
   add the flag), plus its `vcpkg.json`.
2. Add `COPY vcpkg-overlays ./vcpkg-overlays` before the `vcpkg install` step and
   pass `--overlay-ports=/app/vcpkg-overlays` to both the install and the cmake
   configure (`-DVCPKG_OVERLAY_PORTS=...`).
LiveKit/FCM are unaffected either way.

**Build OOMs / killed**
Not enough RAM for the C++ compile. Use a ≥ 4 GB build box, or build the image on
a bigger machine and `docker push` it to a registry, then `docker pull` on the
small server and run with a compose file that uses `image:` instead of `build:`.

**`/health/ready` returns 503**
Mongo or Redis unreachable. Check `MONGO_URI` (and that Atlas allows the server's
IP — add it to the Atlas Network Access allowlist), and `docker compose logs redis`.

---

## Quick reference

| Action | Command |
|---|---|
| Build + start (HTTP) | `docker compose up -d --build` |
| Build + start (HTTPS) | `docker compose --profile tls up -d --build` |
| Logs | `docker compose logs -f backend` |
| Restart | `docker compose restart backend` |
| Stop | `docker compose down` |
| Health | `curl https://api.yourdomain.com/health/ready` |
