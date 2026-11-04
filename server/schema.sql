-- Only the whitelisted report fields and the day it arrived. No IP, no headers, no identifiers.
CREATE TABLE IF NOT EXISTS reports (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  day TEXT NOT NULL,          -- YYYY-MM-DD (no time of day)
  layer TEXT NOT NULL,
  render_gpu TEXT,
  intel_gpu TEXT,
  result TEXT,
  body TEXT NOT NULL          -- the cleaned report JSON
);
CREATE INDEX IF NOT EXISTS reports_gpu ON reports (render_gpu, intel_gpu, result);
