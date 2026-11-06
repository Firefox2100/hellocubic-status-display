function screensHandler() {
  return {
    ui: {},
    presence: { mode: "normal", text: "", gif: "" },
    gifs: [],
    statusMsg: "",

    async init() {
      try {
        this.ui = await (await apiFetch("/api/v1/ui")).json();
        const p = await (await apiFetch("/api/v1/presence")).json();
        this.presence = { mode: p.mode, text: p.text || "", gif: p.gif || "" };
        const g = await (await apiFetch("/api/v1/gif")).json();
        this.gifs = (g.files || []).map((f) => f.name);
      } catch (e) {
        this.statusMsg = "Could not load (is the API token saved?)";
      }
    },

    async post(url, body) {
      const r = await apiFetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const d = await r.json().catch(() => ({}));
      this.statusMsg = r.ok ? "Saved." : "Error: " + (d.error || d.message || r.status);
    },

    savePresence() {
      return this.post("/api/v1/presence", this.presence);
    },

    saveUi() {
      const u = this.ui;
      const body = {};
      for (const k of ["mode", "interval_s", "auto_rotate", "clock_24h", "show_claude", "show_codex",
        "show_ai", "show_trend", "show_info", "claude_weekly_hero", "codex_weekly_hero",
        "refresh_min", "tls_verify"]) {
        body[k] = u[k];
      }
      return this.post("/api/v1/ui", body);
    },
  };
}
