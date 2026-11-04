# Report server (Cloudflare Worker)

Receives the anonymous compatibility reports that the installer's launcher sends after X-Plane closes
(`installer/app/report.ps1`). Free Cloudflare plan is enough.

**Privacy:** the Worker stores only the whitelisted report fields (see `worker.js`) and the day it arrived.
It never stores IP addresses, request headers or identifiers. Workers Logs stay disabled (`wrangler.toml`).

## Deploy (once)

1. Create a free Cloudflare account, install Node.js, then in this folder:
   ```
   npx wrangler login
   npx wrangler d1 create xpob-reports
   ```
2. Paste the printed `database_id` into `wrangler.toml`.
3. Create the table and deploy:
   ```
   npx wrangler d1 execute xpob-reports --remote --file=schema.sql
   npx wrangler deploy
   ```
4. Wrangler prints the Worker URL, e.g. `https://xpob-reports.<name>.workers.dev`. Put
   `https://xpob-reports.<name>.workers.dev/v1/report` into `$script:ReportEndpoint` in
   `installer/app/report.ps1`, commit, and release. Until then the installer never sends anything.

## Check

- `GET /v1/summary` shows reports per GPU combination and result (public, aggregated).
- Raw rows: `npx wrangler d1 execute xpob-reports --remote --command "SELECT day, body FROM reports ORDER BY id DESC LIMIT 20"`
