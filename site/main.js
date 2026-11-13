// Primary download follows the visitor's OS. macOS has no download, so it points at the honest note.
(() => {
  const btn = document.querySelector("[data-primary]");
  const label = document.querySelector("[data-primary-label]");
  const ua = navigator.userAgent;
  if (!btn || !label) return;
  if (/Android|iPhone|iPad/.test(ua)) return;
  if (/Linux|X11/.test(ua)) {
    btn.href = "https://github.com/oocsng/cppdesk/releases/latest/download/CppDesk-x86_64.AppImage";
    label.textContent = "Download for Linux";
  } else if (/Mac/.test(ua)) {
    btn.href = "#macos";
    label.textContent = "macOS: see status";
  }
})();

// Pick two machine tags: a direct cable joins them and the server slot stays empty.
(() => {
  const net = document.getElementById("net");
  if (!net) return;
  const picks = [...net.querySelectorAll(".pick")];
  const cable = document.getElementById("cable");
  const plugs = [document.getElementById("plug-a"), document.getElementById("plug-b")];
  const empty = document.getElementById("session-empty");
  const live = document.getElementById("session-live");
  const status = document.getElementById("connect-status");
  const announce = document.getElementById("announce");
  const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
  let a = null, b = null;

  // Where the segment between the centres of two tags leaves each tag's edge.
  const edge = (r, to) => {
    const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    const dx = to.x - cx, dy = to.y - cy;
    const t = Math.min(dx ? r.width / 2 / Math.abs(dx) : Infinity, dy ? r.height / 2 / Math.abs(dy) : Infinity);
    return { x: cx + dx * t, y: cy + dy * t };
  };

  const draw = (animate) => {
    if (!a || !b) { cable.setAttribute("d", ""); cable.classList.remove("draw"); plugs.forEach((c) => (c.style.display = "none")); return; }
    const o = net.getBoundingClientRect();
    const rel = (el) => { const r = el.getBoundingClientRect(); return { left: r.left - o.left, top: r.top - o.top, width: r.width, height: r.height }; };
    const ra = rel(a), rb = rel(b);
    const ca = { x: ra.left + ra.width / 2, y: ra.top + ra.height / 2 };
    const cb = { x: rb.left + rb.width / 2, y: rb.top + rb.height / 2 };
    const p = edge(ra, cb), q = edge(rb, ca);
    cable.setAttribute("d", `M${p.x} ${p.y}L${q.x} ${q.y}`);
    [[plugs[0], p], [plugs[1], q]].forEach(([c, pt]) => { c.setAttribute("cx", pt.x); c.setAttribute("cy", pt.y); c.style.display = ""; });
    cable.style.setProperty("--len", Math.hypot(q.x - p.x, q.y - p.y));
    cable.classList.remove("draw");
    if (animate && !reduce) { void cable.getBoundingClientRect(); cable.classList.add("draw"); }
  };

  const render = (animate) => {
    picks.forEach((el) => {
      el.setAttribute("aria-pressed", String(el === a && !b));
      el.classList.toggle("live", !!b && (el === a || el === b));
    });
    const connected = !!(a && b);
    empty.hidden = connected;
    live.hidden = !connected;
    if (connected) {
      document.getElementById("live-a").textContent = a.dataset.id;
      document.getElementById("live-b").textContent = b.dataset.id;
      announce.textContent = `${a.dataset.name} connected to ${b.dataset.name}. Direct link, no server.`;
    } else {
      status.textContent = a ? `${a.dataset.name} selected. Pick the machine to connect to.` : "Pick a machine.";
      announce.textContent = status.textContent;
    }
    draw(animate);
  };

  picks.forEach((el) => el.addEventListener("click", () => {
    if (a && b) { a = el; b = null; }
    else if (!a) a = el;
    else if (el === a) a = null;
    else b = el;
    render(true);
    // The session tag seats below the picked tags; bring it into view so the moment is not missed.
    if (a && b) live.scrollIntoView({ block: "nearest", behavior: reduce ? "auto" : "smooth" });
  }));

  document.getElementById("unplug").addEventListener("click", () => {
    const back = a;
    a = b = null;
    render(false);
    back.focus();
  });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && (a || b)) { a = b = null; render(false); } });
  new ResizeObserver(() => draw(false)).observe(net);
})();
