function eventsHandler() {
  const blank = () => ({ id: "", kind: "card", title: "", body: "", value: "", icon: "", level: "info",
    ttl: 0, mode: "busy", text: "", gif: "", repeat: "", at: "", time: "09:00", days: "", day: 1 });
  return {
    f: blank(),
    events: [],
    summary: "",
    statusMsg: "",
    icons: ["email", "event", "meeting", "chat", "call", "reminder", "task", "alert"],

    async init() {
      await this.load();
    },

    async load() {
      try {
        const s = await (await apiFetch("/api/v1/events/status")).json();
        this.summary = `${s.count ?? "?"} of ${s.capacity ?? 64} stored` +
          (s.time_synced === false ? "; clock not synced yet, nothing fires until it is" : "") +
          (s.next_id ? `; next: ${s.next_id}` : "");
        const d = await (await apiFetch("/api/v1/events?limit=64")).json();
        this.events = d.events || [];
      } catch (e) {
        this.statusMsg = "Could not load (is the API token saved?)";
      }
    },

    describe(e) {
      return e.mode ? `status: ${e.mode}${e.text ? " (" + e.text + ")" : ""}` : e.title;
    },

    when(e) {
      if (e.repeat) {
        const days = e.days ? " " + e.days.join(",") : e.day ? " day " + e.day : "";
        return `${e.repeat}${days} ${e.time}`;
      }
      return e.at ? new Date(e.at * 1000).toLocaleString() : "";
    },

    reset() {
      this.f = blank();
    },

    edit(e) {
      const f = blank();
      f.id = e.id;
      if (e.mode) {
        f.kind = "mode";
        f.mode = e.mode;
        f.text = e.text || "";
        f.gif = e.gif || "";
      } else {
        f.title = e.title || "";
        f.body = e.body || "";
        f.value = e.value || "";
        f.icon = e.icon || "";
        f.level = e.kind || "info";
        f.ttl = e.ttl_s || 0;
      }
      if (e.repeat) {
        f.repeat = e.repeat;
        f.time = e.time || "09:00";
        f.days = (e.days || []).join(",");
        f.day = e.day || 1;
      } else if (e.at) {
        const d = new Date(e.at * 1000);
        d.setMinutes(d.getMinutes() - d.getTimezoneOffset());
        f.at = d.toISOString().slice(0, 16);
      }
      this.f = f;
      window.scrollTo({ top: 0, behavior: "smooth" });
    },

    build() {
      const f = this.f;
      const e = { id: f.id.trim() };
      if (f.kind === "mode") {
        e.mode = f.mode;
        if (f.text) e.text = f.text;
        if (f.mode === "relaxing" && f.gif) e.gif = f.gif;
      } else {
        e.title = f.title;
        for (const [k, v] of [["body", f.body], ["value", f.value], ["icon", f.icon]]) if (v) e[k] = v;
        e.kind = f.level;
        if (f.ttl) e.ttl_s = f.ttl;
      }
      if (f.repeat) {
        e.repeat = f.repeat;
        e.time = f.time;
        if (f.repeat === "weekly") e.days = f.days.split(",").map((s) => s.trim()).filter(Boolean);
        if (f.repeat === "monthly") e.day = f.day;
      } else {
        if (!f.at) throw new Error("pick a date and time");
        e.at = Math.floor(new Date(f.at).getTime() / 1000);
      }
      return e;
    },

    async save() {
      let body;
      try {
        body = this.build();
      } catch (err) {
        this.statusMsg = err.message;
        return;
      }
      const r = await apiFetch("/api/v1/events", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const d = await r.json().catch(() => ({}));
      this.statusMsg = r.ok ? "Saved." : "Error: " + (d.error || d.message || r.status);
      if (r.ok) await this.load();
    },

    async remove(id) {
      await apiFetch("/api/v1/events?id=" + encodeURIComponent(id), { method: "DELETE" });
      await this.load();
    },

    async fire(id) {
      await apiFetch("/api/v1/events/fire?id=" + encodeURIComponent(id), { method: "POST" });
      this.statusMsg = "Fired.";
    },
  };
}
