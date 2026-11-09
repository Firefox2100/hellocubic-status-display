function screensHandler() {
  return {
    ui: {},
    presence: { mode: "normal", text: "", gif: "" },
    gifs: [],
    statusMsg: "",
    query: "",
    results: [],
    weatherInfo: "",
    night: { mode: "disabled", style: "face", start: "22:00", end: "07:00", brightness: 10 },
    nightInfo: "",

    async init() {
      try {
        this.ui = await (await apiFetch("/api/v1/ui")).json();
        const p = await (await apiFetch("/api/v1/presence")).json();
        this.presence = { mode: p.mode, text: p.text || "", gif: p.gif || "" };
        const g = await (await apiFetch("/api/v1/gif")).json();
        this.gifs = (g.files || []).map((f) => f.name);
        await this.loadWeather();
        await this.loadNight();
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
        "refresh_min", "tls_verify", "show_weather", "fahrenheit", "weather_lat", "weather_lon", "weather_name"]) {
        body[k] = u[k];
      }
      return this.post("/api/v1/ui", body);
    },

    async loadNight() {
      try {
        const n = await (await apiFetch("/api/v1/night")).json();
        this.night = { mode: n.mode, style: n.style, start: n.start, end: n.end, brightness: n.brightness };
        this.nightInfo = `now: ${n.active ? (n.screen_off ? "night, screen off" : "night face") : "day"}` +
          ` (${n.reason}${n.override !== "none" ? ", switched " + n.override + " by hand" : ""})`;
      } catch (e) {
        this.nightInfo = "";
      }
    },

    async saveNight() {
      await this.post("/api/v1/night", this.night);
      await this.loadNight();
    },

    async nightSwitch(action) {
      await this.post("/api/v1/night", { switch: action });
      await this.loadNight();
    },

    async loadWeather() {
      try {
        const w = await (await apiFetch("/api/v1/weather")).json();
        const f = w.fetch || {};
        const lines = [`fetch: ${f.state} (${f.source}), age ${f.age_s}s, next in ${f.next_fetch_in_s}s` +
          (f.error ? `, error: ${f.error}` : ""), `screen: ${w.active_channel}, Home page active: ${w.home_active}`];
        if (w.shown) {
          lines.push(`shown: ${w.shown.temp}°${w.shown.unit} (feels ${w.shown.feels}°), ${w.shown.condition}` +
            (w.shown.rain_hint ? `, ${w.shown.rain_hint}` : ""));
        }
        this.weatherInfo = lines.join("\n");
      } catch (e) {
        this.weatherInfo = "";
      }
    },

    async refreshWeather() {
      await apiFetch("/api/v1/weather/refresh", { method: "POST" });
      this.weatherInfo = "fetching...";
      setTimeout(() => this.loadWeather(), 4000);
    },

    // Place search runs in this browser (the cube has no internet lookup for names).
    async search() {
      this.results = [];
      if (!this.query.trim()) return;
      try {
        const r = await fetch("https://geocoding-api.open-meteo.com/v1/search?count=5&name=" +
          encodeURIComponent(this.query.trim()));
        const d = await r.json();
        this.results = (d.results || []).map((x) => ({
          id: x.id, name: x.name, lat: x.latitude, lon: x.longitude,
          label: [x.name, x.admin1, x.country].filter(Boolean).join(", "),
        }));
        if (!this.results.length) this.statusMsg = "No place found.";
      } catch (e) {
        this.statusMsg = "Place search needs internet access from this browser.";
      }
    },

    pick(r) {
      this.ui.weather_name = r.name.replace(/[^\x20-\x7e]/g, "").slice(0, 24);
      this.ui.weather_lat = Math.round(r.lat * 10000) / 10000;
      this.ui.weather_lon = Math.round(r.lon * 10000) / 10000;
      this.results = [];
    },
  };
}
