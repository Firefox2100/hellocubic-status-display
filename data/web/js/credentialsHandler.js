function credentialsHandler() {
  return {
    state: {},
    v: { claude_key: "", codex_token: "", codex_device_id: "", codex_model_label: "" },
    statusMsg: "",
    usage: "",

    async init() {
      try {
        this.state = await (await apiFetch("/api/v1/credentials")).json();
        this.v.codex_model_label = this.state.codex_model_label || "";
      } catch (e) {
        this.statusMsg = "Could not load (is the API token saved?)";
      }
    },

    async send(body) {
      const r = await apiFetch("/api/v1/credentials", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const d = await r.json().catch(() => ({}));
      this.statusMsg = r.ok ? "Saved." : "Error: " + (d.error || d.message || r.status);
      this.v.claude_key = this.v.codex_token = this.v.codex_device_id = "";
      await this.init();
    },

    save() {
      const body = {};
      for (const k of Object.keys(this.v)) {
        if (this.v[k] !== "" && !(k === "codex_model_label" && this.v[k] === this.state.codex_model_label)) {
          body[k] = this.v[k];
        }
      }
      if (!Object.keys(body).length) {
        this.statusMsg = "Nothing to save.";
        return;
      }
      return this.send(body);
    },

    remove(field) {
      return this.send({ [field]: "" });
    },

    async refresh() {
      await apiFetch("/api/v1/usage/refresh", { method: "POST" });
      setTimeout(async () => {
        const d = await (await apiFetch("/api/v1/usage")).json();
        this.usage = JSON.stringify(d, null, 2);
      }, 12000);
      this.statusMsg = "Fetching, result in about 12 seconds...";
    },
  };
}
