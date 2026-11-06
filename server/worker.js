// XP Optimus Bypass — anonymous compatibility report receiver (Cloudflare Worker + D1).
//
// Privacy: the Worker never stores the IP address, request headers or cookies. It keeps only the whitelisted JSON
// fields below and the day the report arrived. install_id is a random number the installer makes on the user's PC
// (32 hex digits, not derived from the hardware or the user, resettable); it only groups reports of one installation
// and is never shown in the public summary. The IP is used only, in memory, by
// Cloudflare's rate limiter (see wrangler.toml) and is not written anywhere by this code.
//
// POST /v1/report   body: report JSON from installer/app/report.ps1 (max 4 KB)
// GET  /v1/summary  public aggregate: reports per render GPU / Intel GPU / result, median FPS per path

const MAX_BYTES = 4096;
const STR = (v, max) => (typeof v === "string" ? v.replace(/[^\x20-\x7E]/g, "").slice(0, max) : "");
const NUM = (v) => (typeof v === "number" && Number.isFinite(v) && v >= 0 && v < 100000 ? v : null);
const BOOL = (v) => (typeof v === "boolean" ? v : null);
const PATHLIKE = /[A-Za-z]:\\|\\\\|%USERPROFILE%|\/Users\//;

function clean(r) {
  if (!r || typeof r !== "object" || r.schema !== 1) return null;
  const fps = {};
  for (const p of ["bypass", "native", "real-swapchain"]) {
    const f = r.fps && r.fps[p];
    if (f && typeof f === "object")
      fps[p] = { windows: NUM(f.windows), fps: NUM(f.fps), p50_ms: NUM(f.p50_ms), p95_ms: NUM(f.p95_ms),
                 p99_ms: NUM(f.p99_ms), over50_per_300: NUM(f.over50_per_300) };
  }
  const probe = r.probe && typeof r.probe === "object"
    ? { selected: STR(r.probe.selected, 10), native_p50: NUM(r.probe.native_p50), bypass_p50: NUM(r.probe.bypass_p50),
        native_p95: NUM(r.probe.native_p95), bypass_p95: NUM(r.probe.bypass_p95) }
    : null;
  const list = (a, n, max) => (Array.isArray(a) ? a.slice(0, n).map((x) => STR(x, max)).filter((x) => x && !PATHLIKE.test(x)) : []);
  const out = {
    install_id: typeof r.install_id === "string" && /^[0-9a-f]{32}$/.test(r.install_id) ? r.install_id : "",
    layer: STR(r.layer, 20), installer: STR(r.installer, 20), install_mode: STR(r.install_mode, 10),
    windows: STR(r.windows, 20), xplane: STR(r.xplane, 20),
    render_gpu: STR(r.render_gpu, 80), render_driver: STR(r.render_driver, 20),
    intel_gpu: STR(r.intel_gpu, 80), intel_driver: STR(r.intel_driver, 20),
    screens: NUM(r.screens), intel_screens: NUM(r.intel_screens), window_on_intel: BOOL(r.window_on_intel),
    mode: STR(r.mode, 10), path_pref: STR(r.path_pref, 10), test_switches: BOOL(r.test_switches),
    result: STR(r.result, 20), reason: STR(r.reason, 120), mem: STR(r.mem, 60), conv: STR(r.conv, 30),
    probe, fps, overlays: list(r.overlays, 10, 60), errors: list(r.errors, 5, 100),
  };
  for (const k of ["reason", "render_gpu", "intel_gpu", "mem", "conv"]) if (PATHLIKE.test(out[k])) out[k] = "";
  return out;
}

const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), { status, headers: { "content-type": "application/json", "access-control-allow-origin": "*" } });

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "POST" && url.pathname === "/v1/report") {
      if (env.LIMITER) {
        const ip = request.headers.get("cf-connecting-ip") || "unknown";  // in memory only, never stored
        const { success } = await env.LIMITER.limit({ key: ip });
        if (!success) return json({ ok: false, error: "rate limited" }, 429);
      }
      const len = Number(request.headers.get("content-length") || "0");
      if (len > MAX_BYTES) return json({ ok: false, error: "too large" }, 413);
      const text = await request.text();
      if (text.length > MAX_BYTES) return json({ ok: false, error: "too large" }, 413);
      let report;
      try { report = clean(JSON.parse(text)); } catch { report = null; }
      if (!report || !report.layer) return json({ ok: false, error: "invalid report" }, 400);
      const day = new Date().toISOString().slice(0, 10);
      await env.DB.prepare("INSERT INTO reports (day, layer, render_gpu, intel_gpu, result, body) VALUES (?, ?, ?, ?, ?, ?)")
        .bind(day, report.layer, report.render_gpu, report.intel_gpu, report.result, JSON.stringify(report)).run();
      return json({ ok: true });
    }
    if (request.method === "GET" && url.pathname === "/v1/summary") {
      const { results } = await env.DB.prepare(
        "SELECT render_gpu, intel_gpu, result, COUNT(*) AS reports, " +
        "COUNT(DISTINCT NULLIF(json_extract(body, '$.install_id'), '')) AS installs, " +
        "AVG(json_extract(body, '$.fps.bypass.fps')) AS avg_bypass_fps, " +
        "AVG(json_extract(body, '$.fps.native.fps')) AS avg_native_fps, " +
        "AVG(json_extract(body, '$.fps.\"real-swapchain\".fps')) AS avg_real_fps " +
        "FROM reports WHERE json_extract(body, '$.test_switches') IS NOT 1 " +
        "GROUP BY render_gpu, intel_gpu, result ORDER BY reports DESC LIMIT 200").all();
      return json({ ok: true, groups: results });
    }
    return json({ ok: false, error: "not found" }, 404);
  },
};
