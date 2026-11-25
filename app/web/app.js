/* Film Cockpit — 60fps Glass Cockpit Canvas Editor & Scanner Orchestrator */
(() => {
  "use strict";

  const STRIP_COLORS = ["#ff4dff", "#ffa31a", "#4da6ff", "#b18cff", "#ff6b8b", "#d9b38c"];
  const FORMAT_SPECS = {
    half_frame: { wMm: 24.0, hMm: 17.6, pitchMm: 19.0, count: 12 },
    full_frame: { wMm: 24.0, hMm: 36.0, pitchMm: 38.0, count: 6 },
  };
  const DEFAULT_DEVELOP = {
    enabled: true,
    profile: "frontier",
    exposure: 0.0,
    contrast: 1.0,
    blacks: 0.0,
    temp: 0.0,
    tint: 0.0,
    saturation: 1.0,
    chroma_nr: 0.55,
    luma_nr: 0.15,
    sharpen: 0.25,
    auto_wb: true,
  };

  // ---------- Application State ----------
  const state = {
    scans: [],
    activeScan: null,       // scan metadata object
    strips: [],             // [{index, x0, x1, y0, y1, angle_deg, color}]
    frames: [],             // [{id, strip_index, cx, cy, width, height, angle_deg, inset_pct, develop}]
    selectedId: null,
    activeStripIndex: 0,
    undoStack: [],
    redoStack: [],
    backends: {},
    lastCompletedJobId: null,
    lastLogKey: "",

    // Mode & Develop lab state
    uiMode: "crop",         // "crop" | "develop"
    filmBase: null,         // [r, g, b] linear 0..1 sampled from inter-frame gaps
    devIrPrev: false,
    devBefore: false,
    wbPickMode: false,
    devReqSeq: 0,
    devTimer: null,

    // View controls
    invertPreview: false,
    showIrMask: false,
    showIrRaw: false,
    irSens: 0.5,
    insetPct: 0.0,
    skipEmpty: true,
    drawMode: false,

    // Canvas transform (maps full-res scan coords -> CSS pixels)
    viewRot: 0,             // 0, 90, 180, 270 deg CW viewport rotation
    zoom: 1.0,
    panX: 0.0,
    panY: 0.0,
    img: null,              // HTMLImageElement (overview RGB or IR raw)
    maskImg: null,          // HTMLImageElement (overview IR dust mask PNG)
    hiResTile: null,        // {img, rx, ry, rw, rh, key} crisp native-res viewport tile
    hiResMask: null,        // {img, rx, ry, rw, rh, key} crisp native-res IR mask tile
    hiResReqSeq: 0,
    hiResTimer: null,
    delRawArmed: false,
    delRawTimer: null,
    cursorImgX: null,
    cursorImgY: null,

    // Pointer interaction
    spaceDown: false,
    drag: null,             // {type: 'pan'|'move'|'resize'|'rotate'|'draw', ...}
  };

  // ---------- DOM Shortcuts ----------
  const $ = (id) => document.getElementById(id);
  const canvas = $("stage");
  const ctx = canvas.getContext("2d");
  const vpWrap = $("viewport-wrap");

  let toastTimer = null;
  function toast(msg, durationMs = 2600) {
    const el = $("toast");
    el.textContent = msg;
    el.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { el.hidden = true; }, durationMs);
  }

  function setStatusMsg(msg) {
    $("status-msg").textContent = msg;
  }

  function getToggle(id) {
    return $(id).getAttribute("aria-pressed") === "true";
  }
  function setToggle(id, val) {
    $(id).setAttribute("aria-pressed", val ? "true" : "false");
  }

  // ---------- Undo / Redo History (100 steps, zero no-op snapshots) ----------
  function captureSnapshot() {
    return {
      framesJson: JSON.stringify(state.frames),
      selectedId: state.selectedId,
      insetPct: state.insetPct,
    };
  }
  function syncHistoryButtons() {
    const uBtn = $("btn-undo");
    const rBtn = $("btn-redo");
    if (uBtn) {
      uBtn.disabled = state.undoStack.length === 0;
      uBtn.title = `Undo last edit (Ctrl+Z) · ${state.undoStack.length} step(s)`;
    }
    if (rBtn) {
      rBtn.disabled = state.redoStack.length === 0;
      rBtn.title = `Redo edit (Ctrl+Y / Ctrl+Shift+Z) · ${state.redoStack.length} step(s)`;
    }
  }
  function pushUndo(customSnap = null) {
    const snap = customSnap || captureSnapshot();
    const top = state.undoStack[state.undoStack.length - 1];
    if (top && top.framesJson === snap.framesJson && top.insetPct === snap.insetPct) return;
    state.undoStack.push(snap);
    if (state.undoStack.length > 100) state.undoStack.shift();
    state.redoStack = [];
    syncHistoryButtons();
    scheduleAutoSave();
  }
  function applySnapshot(snap) {
    state.frames = JSON.parse(snap.framesJson);
    state.insetPct = typeof snap.insetPct === "number" ? snap.insetPct : state.insetPct;
    $("in-inset").value = String(state.insetPct);
    $("val-inset").textContent = `${state.insetPct.toFixed(1)}%`;
    state.selectedId = (snap.selectedId && state.frames.some((f) => f.id === snap.selectedId))
      ? snap.selectedId
      : (state.frames[0]?.id || null);
    syncHistoryButtons();
    syncUI();
    render();
    if (state.uiMode === "develop") {
      syncDevelopUI();
      scheduleDevelopPreview(40);
    }
    scheduleAutoSave();
  }
  function undo() {
    if (!state.undoStack.length) {
      toast("NOTHING TO UNDO");
      return;
    }
    const cur = captureSnapshot();
    const prev = state.undoStack.pop();
    state.redoStack.push(cur);
    if (state.redoStack.length > 100) state.redoStack.shift();
    applySnapshot(prev);
    toast(`UNDONE (${state.undoStack.length} LEFT)`);
  }
  function redo() {
    if (!state.redoStack.length) {
      toast("NOTHING TO REDO");
      return;
    }
    const cur = captureSnapshot();
    const next = state.redoStack.pop();
    state.undoStack.push(cur);
    applySnapshot(next);
    toast(`REDONE (${state.redoStack.length} LEFT)`);
  }

  // ---------- API Helpers ----------
  async function apiGet(url) {
    const res = await fetch(url);
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
    return data;
  }
  async function apiPost(url, body) {
    const res = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body || {}),
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
    return data;
  }

  // ---------- Coordinate Math (supports 0/90/180/270 deg viewport rotation) ----------
  function viewCS() {
    const r = (state.viewRot * Math.PI) / 180;
    return [Math.round(Math.cos(r)), Math.round(Math.sin(r))];
  }
  function scanToScreen(ix, iy) {
    const [c, s] = viewCS();
    const rx = ix * c - iy * s;
    const ry = ix * s + iy * c;
    return [rx * state.zoom + state.panX, ry * state.zoom + state.panY];
  }
  function screenToScan(sx, sy) {
    const [c, s] = viewCS();
    const rx = (sx - state.panX) / state.zoom;
    const ry = (sy - state.panY) / state.zoom;
    return [rx * c + ry * s, -rx * s + ry * c];
  }
  function screenDeltaToScan(dsx, dsy) {
    const [c, s] = viewCS();
    return [dsx * c + dsy * s, -dsx * s + dsy * c];
  }
  function getScanDims() {
    if (!state.activeScan) return [1, 1];
    return [state.activeScan.width || 1, state.activeScan.height || 1];
  }

  function centerScanPoint(ix, iy, targetZoom) {
    const rect = vpWrap.getBoundingClientRect();
    const [c, s] = viewCS();
    const rx = ix * c - iy * s;
    const ry = ix * s + iy * c;
    state.zoom = Math.min(8.0, Math.max(0.02, targetZoom));
    state.panX = rect.width * 0.5 - rx * state.zoom;
    state.panY = rect.height * 0.5 - ry * state.zoom;
  }

  function fitViewport() {
    if (!state.activeScan) return;
    const rect = vpWrap.getBoundingClientRect();
    const [sw, sh] = getScanDims();
    const ew = (state.viewRot % 180 === 0) ? sw : sh;
    const eh = (state.viewRot % 180 === 0) ? sh : sw;
    const pad = 28;
    const z = Math.min((rect.width - pad * 2) / ew, (rect.height - pad * 2) / eh);
    centerScanPoint(sw * 0.5, sh * 0.5, z);
    state.hiResTile = null;
    state.hiResMask = null;
    updateHud();
    render();
    scheduleHiResTile();
  }

  function zoomToFrame(f) {
    if (!state.activeScan || !f) return;
    const rect = vpWrap.getBoundingClientRect();
    const ew = (state.viewRot % 180 === 0) ? f.width : f.height;
    const eh = (state.viewRot % 180 === 0) ? f.height : f.width;
    const z = Math.min((rect.width * 0.78) / Math.max(16, ew), (rect.height * 0.78) / Math.max(16, eh));
    centerScanPoint(f.cx, f.cy, z);
    updateHud();
    render();
    scheduleHiResTile();
  }

  function stepFrame(delta) {
    if (!state.frames.length) return;
    let idx = state.frames.findIndex((f) => f.id === state.selectedId);
    if (idx < 0) idx = delta > 0 ? 0 : state.frames.length - 1;
    else idx = (idx + delta + state.frames.length) % state.frames.length;
    const f = state.frames[idx];
    state.selectedId = f.id;
    state.activeStripIndex = f.strip_index || 0;
    syncUI();
    if (state.uiMode === "develop") {
      syncDevelopUI();
      scheduleDevelopPreview(10);
    } else {
      zoomToFrame(f);
    }
  }

  function rotateViewport(deltaDeg = 90) {
    if (!state.activeScan) return;
    const rect = vpWrap.getBoundingClientRect();
    const [cx, cy] = screenToScan(rect.width * 0.5, rect.height * 0.5);
    state.viewRot = (state.viewRot + deltaDeg + 360) % 360;
    const sel = getSelectedFrame();
    if (sel) {
      zoomToFrame(sel);
    } else {
      centerScanPoint(cx, cy, state.zoom);
      updateHud();
      render();
      scheduleHiResTile();
    }
    toast(`VIEW ROTATION: ${state.viewRot} DEG`);
  }

  function zoom100() {
    if (!state.activeScan) return;
    const rect = vpWrap.getBoundingClientRect();
    const [ix, iy] = screenToScan(rect.width * 0.5, rect.height * 0.5);
    centerScanPoint(ix, iy, 1.0);
    updateHud();
    render();
    scheduleHiResTile();
  }

  function zoomAt(screenX, screenY, factor) {
    if (!state.activeScan) return;
    const [ix, iy] = screenToScan(screenX, screenY);
    const [c, s] = viewCS();
    state.zoom = Math.min(8.0, Math.max(0.03, state.zoom * factor));
    const rx = ix * c - iy * s;
    const ry = ix * s + iy * c;
    state.panX = screenX - rx * state.zoom;
    state.panY = screenY - ry * state.zoom;
    updateHud();
    render();
    scheduleHiResTile();
  }

  // ---------- Native-Res 1:1 Viewport Tile Loader ----------
  function scheduleHiResTile() {
    clearTimeout(state.hiResTimer);
    if (!state.activeScan) return;
    const [sw, sh] = getScanDims();
    const dpr = window.devicePixelRatio || 1;
    const overviewScale = Math.min(1.0, 1800 / Math.max(sw, sh));
    // Only fetch a crisp native region tile when zoomed beyond overview resolution
    if (state.zoom * dpr <= overviewScale * 1.15) {
      if (state.hiResTile || state.hiResMask) {
        state.hiResTile = null;
        state.hiResMask = null;
        updateHud();
        render();
      }
      return;
    }
    state.hiResTimer = setTimeout(fetchHiResTile, 100);
  }

  function fetchHiResTile() {
    if (!state.activeScan) return;
    const [sw, sh] = getScanDims();
    const rect = vpWrap.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;

    // Transform all 4 viewport corners into scan coordinates (works at any viewRot!)
    const corners = [
      screenToScan(0, 0),
      screenToScan(rect.width, 0),
      screenToScan(rect.width, rect.height),
      screenToScan(0, rect.height),
    ];
    const xs = corners.map((p) => p[0]);
    const ys = corners.map((p) => p[1]);
    const x0 = Math.min(...xs), x1 = Math.max(...xs);
    const y0 = Math.min(...ys), y1 = Math.max(...ys);

    const mx = (x1 - x0) * 0.06;
    const my = (y1 - y0) * 0.06;
    const rx = Math.max(0, Math.floor(x0 - mx));
    const ry = Math.max(0, Math.floor(y0 - my));
    const rx1 = Math.min(sw, Math.ceil(x1 + mx));
    const ry1 = Math.min(sh, Math.ceil(y1 + my));
    const rw = rx1 - rx;
    const rh = ry1 - ry;
    if (rw < 8 || rh < 8) return;

    const desiredW = Math.min(rw, Math.ceil(rw * state.zoom * dpr), 1800);
    const desiredH = Math.min(rh, Math.ceil(rh * state.zoom * dpr), 1800);
    const outW = Math.max(16, desiredW);
    const outH = Math.max(16, desiredH);

    const name = encodeURIComponent(state.activeScan.name);
    const filmType = $("in-filmtype").value;
    const mode = state.invertPreview ? filmType : "raw";
    const kind = state.showIrRaw ? "ir_raw" : "rgb";
    const seq = ++state.hiResReqSeq;
    const tileKey = `${state.activeScan.name}|${kind}|${mode}|${rx},${ry},${rw},${rh}`;

    const tImg = new Image();
    tImg.onload = () => {
      if (seq !== state.hiResReqSeq) return;
      state.hiResTile = { img: tImg, rx, ry, rw, rh, key: tileKey };
      updateHud();
      render();
    };
    tImg.src = `/api/scan/${name}/region?kind=${kind}&mode=${encodeURIComponent(mode)}&x=${rx}&y=${ry}&w=${rw}&h=${rh}&out_w=${outW}&out_h=${outH}`;

    if (state.showIrMask && state.activeScan.has_ir) {
      const mImg = new Image();
      mImg.onload = () => {
        if (seq !== state.hiResReqSeq) return;
        state.hiResMask = { img: mImg, rx, ry, rw, rh, key: tileKey };
        render();
      };
      mImg.src = `/api/scan/${name}/region?kind=ir_mask&mode=raw&ir_sens=${state.irSens.toFixed(2)}&x=${rx}&y=${ry}&w=${rw}&h=${rh}&out_w=${outW}&out_h=${outH}`;
    } else {
      state.hiResMask = null;
    }
  }

  // ---------- Preview Loading ----------
  function loadPreviewImages() {
    state.hiResTile = null;
    state.hiResMask = null;
    if (!state.activeScan) {
      state.img = null;
      state.maskImg = null;
      render();
      return;
    }
    const name = encodeURIComponent(state.activeScan.name);
    const filmType = $("in-filmtype").value;
    const mode = state.invertPreview ? filmType : "raw";
    const endpoint = state.showIrRaw
      ? `/api/scan/${name}/ir_raw?max_dim=1800`
      : `/api/scan/${name}/preview?mode=${encodeURIComponent(mode)}&max_dim=1800`;

    setStatusMsg("LOADING PREVIEW...");
    const img = new Image();
    img.onload = () => {
      state.img = img;
      setStatusMsg("STANDALONE · NO LLM REQUIRED");
      updateHud();
      render();
      scheduleHiResTile();
    };
    img.onerror = () => {
      toast("FAILED TO LOAD PREVIEW");
      setStatusMsg("PREVIEW ERROR");
    };
    img.src = endpoint;

    if (state.showIrMask && state.activeScan.has_ir) {
      const mImg = new Image();
      mImg.onload = () => {
        state.maskImg = mImg;
        render();
      };
      mImg.src = `/api/scan/${name}/ir_mask?ir_sens=${state.irSens.toFixed(2)}&max_dim=1800`;
    } else {
      state.maskImg = null;
    }
  }

  // ---------- Scan Selection & Detection ----------
  function closeScan() {
    flushAutoSave();
    state.activeScan = null;
    state.selectedId = null;
    state.undoStack = [];
    state.redoStack = [];
    state.frames = [];
    state.strips = [];
    state.filmBase = null;
    state.img = null;
    state.maskImg = null;
    state.hiResTile = null;
    state.hiResMask = null;
    $("scan-meta").textContent = "NO SCAN LOADED";
    $("tog-irmask").disabled = true;
    $("tog-irraw").disabled = true;
    if (state.uiMode === "develop") setUiMode("crop");
    syncHistoryButtons();
    syncUI();
    render();
  }

  async function refreshScans(selectName = null, autoOpen = false) {
    const data = await apiGet("/api/scans");
    state.scans = data.scans || [];
    const sel = $("scan-select");
    const curActive = state.activeScan && state.activeScan.name;
    sel.innerHTML = "";

    const blankOpt = document.createElement("option");
    blankOpt.value = "";
    blankOpt.textContent = state.scans.length ? "(SELECT SCAN...)" : "(NO SCANS YET)";
    sel.appendChild(blankOpt);

    for (const s of state.scans) {
      const opt = document.createElement("option");
      opt.value = s.name;
      const irTag = s.has_ir ? " · IR" : "";
      opt.textContent = `${s.name} (${s.dpi || "?"}dpi${irTag})`;
      sel.appendChild(opt);
    }

    // Explicit open requested (e.g. user selected one)
    if (autoOpen && selectName) {
      const target = state.scans.find((s) => s.name === selectName);
      if (target) {
        sel.value = target.name;
        await openScan(target);
        return;
      }
    }

    // Preserve currently active scan if it still exists; otherwise stay blank
    if (curActive && state.scans.some((s) => s.name === curActive)) {
      sel.value = curActive;
    } else {
      sel.value = "";
      if (state.activeScan) closeScan();
      else { syncUI(); render(); }
    }
  }

  async function openScan(scan) {
    if (state.activeScan && state.activeScan.name !== scan.name) {
      flushAutoSave();
    }
    state.activeScan = scan;
    state.selectedId = null;
    state.undoStack = [];
    state.redoStack = [];
    state.frames = [];
    state.strips = [];
    state.filmBase = null;
    syncHistoryButtons();

    const mb = ((scan.size_bytes || 0) / (1024 * 1024)).toFixed(1);
    const irTxt = scan.has_ir ? "64-BIT HDRi (RGB+IR)" : `${scan.bits || 16}-BIT RGB`;
    $("scan-meta").textContent = `${scan.width}x${scan.height} px · ${scan.dpi} DPI · ${irTxt} · ${mb} MB`;
    $("tog-irmask").disabled = !scan.has_ir;
    $("tog-irraw").disabled = !scan.has_ir;
    if (!scan.has_ir) {
      state.showIrMask = false;
      state.showIrRaw = false;
      setToggle("tog-irmask", false);
      setToggle("tog-irraw", false);
    }

    loadPreviewImages();
    fitViewport();

    // Try loading saved layout first; otherwise auto-detect
    try {
      const layout = await apiGet(`/api/scan/${encodeURIComponent(scan.name)}/layout`);
      if (layout && Array.isArray(layout.frames) && layout.frames.length) {
        state.strips = layout.strips || [];
        state.frames = layout.frames;
        state.filmBase = Array.isArray(layout.film_base) ? layout.film_base : null;
        if (typeof layout.camera === "string" && !$("in-camera").value) $("in-camera").value = layout.camera;
        if (typeof layout.film_stock === "string" && !$("in-stock").value) $("in-stock").value = layout.film_stock;
        if (typeof layout.inset_pct === "number") {
          state.insetPct = layout.inset_pct;
          $("in-inset").value = String(state.insetPct);
          $("val-inset").textContent = `${state.insetPct.toFixed(1)}%`;
        }
        syncUI();
        render();
        if (state.uiMode === "develop") {
          syncDevelopUI();
          scheduleDevelopPreview(10);
        }
        toast(`LOADED SAVED LAYOUT (${state.frames.length} FRAMES)`);
        return;
      }
    } catch (_) { /* ignore */ }

    await runAutoDetect();
  }

  async function runAutoDetect() {
    if (!state.activeScan) return;
    setStatusMsg("DETECTING STRIPS & FRAMES...");
    try {
      const res = await apiPost(`/api/scan/${encodeURIComponent(state.activeScan.name)}/detect`, {
        film_format: $("in-format").value,
        film_type: $("in-filmtype").value,
        inset_pct: state.insetPct,
        skip_empty: state.skipEmpty,
      });
      pushUndo();
      state.strips = res.strips || [];
      state.frames = res.frames || [];
      state.filmBase = null;
      state.selectedId = state.frames.length ? state.frames[0].id : null;
      syncUI();
      render();
      toast(`DETECTED ${state.strips.length} STRIPS · ${state.frames.length} FRAMES (${res.elapsed_ms} ms)`);
      setStatusMsg("STANDALONE · NO LLM REQUIRED");
    } catch (err) {
      toast(`DETECT ERR: ${err.message}`);
      setStatusMsg("DETECT ERROR");
    }
  }

  let autoSaveTimer = null;
  let autoSaveDirty = false;

  function buildLayoutPayload() {
    if (!state.activeScan) return null;
    return {
      scan_name: state.activeScan.name,
      film_format: $("in-format").value,
      film_type: $("in-filmtype").value,
      inset_pct: state.insetPct,
      film_base: state.filmBase,
      camera: $("in-camera").value.trim(),
      film_stock: $("in-stock").value.trim(),
      strips: state.strips,
      frames: state.frames,
    };
  }

  function scheduleAutoSave() {
    if (!state.activeScan) return;
    autoSaveDirty = true;
    clearTimeout(autoSaveTimer);
    setStatusMsg("UNSAVED CHANGES...");
    const targetName = state.activeScan.name;
    autoSaveTimer = setTimeout(async () => {
      autoSaveTimer = null;
      if (!state.activeScan || state.activeScan.name !== targetName) return;
      await saveLayout(true, true);
    }, 900);
  }

  function flushAutoSave() {
    if (!autoSaveDirty || !state.activeScan) return;
    clearTimeout(autoSaveTimer);
    autoSaveTimer = null;
    autoSaveDirty = false;
    const payload = buildLayoutPayload();
    if (!payload) return;
    const url = `/api/scan/${encodeURIComponent(state.activeScan.name)}/layout`;
    try {
      const blob = new Blob([JSON.stringify(payload)], { type: "application/json" });
      if (!navigator.sendBeacon || !navigator.sendBeacon(url, blob)) {
        fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload), keepalive: true }).catch(() => {});
      }
    } catch (_) { /* ignore */ }
  }

  window.addEventListener("pagehide", flushAutoSave);

  async function saveLayout(silent = false, isAuto = false) {
    const payload = buildLayoutPayload();
    if (!payload) return;
    clearTimeout(autoSaveTimer);
    autoSaveTimer = null;
    autoSaveDirty = false;
    try {
      await apiPost(`/api/scan/${encodeURIComponent(payload.scan_name)}/layout`, payload);
      setStatusMsg(isAuto ? `AUTOSAVED (${state.frames.length} FRAMES)` : `SAVED (${state.frames.length} FRAMES)`);
      if (!silent) toast(`SAVED LAYOUT (${state.frames.length} FRAMES)`);
    } catch (err) {
      if (!silent) toast(`SAVE ERR: ${err.message}`);
      setStatusMsg("SAVE ERROR");
    }
  }

  // ---------- Frame Manipulation Helpers ----------
  function nextFrameId() {
    let maxN = 0;
    for (const f of state.frames) {
      const m = /^f(\d+)$/.exec(f.id || "");
      if (m) maxN = Math.max(maxN, parseInt(m[1], 10));
    }
    return `f${String(maxN + 1).padStart(3, "0")}`;
  }

  function getSelectedFrame() {
    return state.frames.find((f) => f.id === state.selectedId) || null;
  }

  function deleteSelectedFrame() {
    if (!state.selectedId) return;
    const idx = state.frames.findIndex((f) => f.id === state.selectedId);
    if (idx < 0) return;
    pushUndo();
    state.frames.splice(idx, 1);
    state.selectedId = state.frames[Math.min(idx, state.frames.length - 1)]?.id || null;
    syncUI();
    render();
  }

  function nudgeFrameOrStrip(dx, dy, dAngle, wholeStrip = false, stripIdxOverride = null) {
    if (!state.activeScan) return;
    if (wholeStrip || stripIdxOverride !== null) {
      const sel = getSelectedFrame();
      const sIdx = stripIdxOverride !== null ? stripIdxOverride : (sel ? sel.strip_index : state.activeStripIndex);
      const targets = state.frames.filter((f) => f.strip_index === sIdx);
      if (!targets.length) return;
      pushUndo();
      for (const f of targets) {
        f.cx += dx;
        f.cy += dy;
        f.angle_deg = round3((f.angle_deg || 0) + dAngle);
      }
      syncUI();
      render();
      return;
    }
    const f = getSelectedFrame();
    if (!f) return;
    pushUndo();
    f.cx += dx;
    f.cy += dy;
    f.angle_deg = round3((f.angle_deg || 0) + dAngle);
    syncUI();
    render();
  }

  function nudgeScreen(dsx, dsy, dAngle, wholeStrip = false, stripIdxOverride = null) {
    const [dx, dy] = screenDeltaToScan(dsx, dsy);
    nudgeFrameOrStrip(dx, dy, dAngle, wholeStrip, stripIdxOverride);
  }

  function rotateSelectedExport90(delta) {
    const f = getSelectedFrame();
    if (!f) return;
    pushUndo();
    f.rot90 = (((f.rot90 || 0) + delta) % 360 + 360) % 360;
    syncUI();
    render();
    if (state.uiMode === "develop") scheduleDevelopPreview(10);
    toast(`${f.id.toUpperCase()} EXPORT ORIENTATION: ${f.rot90} DEG`);
  }

  function stampStripGrid(stripIdx = null) {
    if (!state.activeScan) return;
    const dpi = state.activeScan.dpi || 300;
    const pxPerMm = dpi / 25.4;
    const spec = FORMAT_SPECS[$("in-format").value] || FORMAT_SPECS.half_frame;
    const [sw, sh] = getScanDims();

    let cx = sw * 0.5;
    let y0 = sh * 0.04;
    let y1 = sh * 0.96;
    let angle = 0.0;
    let targetStripIdx = stripIdx;

    if (targetStripIdx === null) {
      const sel = getSelectedFrame();
      targetStripIdx = sel ? sel.strip_index : state.activeStripIndex;
    }
    const st = state.strips.find((s) => s.index === targetStripIdx);
    if (st) {
      cx = 0.5 * (st.x0 + st.x1);
      y0 = st.y0;
      y1 = st.y1;
      angle = st.angle_deg || 0;
    } else {
      targetStripIdx = state.strips.length;
    }

    const fw = spec.wMm * pxPerMm;
    const fh = spec.hMm * pxPerMm;
    const pitch = spec.pitchMm * pxPerMm;

    pushUndo();
    // Replace existing frames on that strip if triggered on a known strip
    state.frames = state.frames.filter((f) => f.strip_index !== targetStripIdx);
    let y = y0 + fh * 0.5 + 1.5 * pxPerMm;
    let added = 0;
    while (y + fh * 0.35 <= y1 && added < spec.count) {
      state.frames.push({
        id: nextFrameId(),
        strip_index: targetStripIdx,
        cx: round1(cx),
        cy: round1(y),
        width: round1(fw),
        height: round1(fh),
        angle_deg: angle,
        inset_pct: state.insetPct,
      });
      y += pitch;
      added++;
    }
    syncUI();
    render();
    toast(`STAMPED ${added} FRAMES ON STRIP ${targetStripIdx + 1}`);
  }

  function duplicateFrameBelow() {
    const f = getSelectedFrame();
    if (!f || !state.activeScan) return;
    const dpi = state.activeScan.dpi || 300;
    const spec = FORMAT_SPECS[$("in-format").value] || FORMAT_SPECS.half_frame;
    const pitch = spec.pitchMm * (dpi / 25.4);
    pushUndo();
    const copy = {
      ...f,
      id: nextFrameId(),
      cy: round1(f.cy + pitch),
    };
    state.frames.push(copy);
    state.selectedId = copy.id;
    syncUI();
    render();
  }

  function round1(v) { return Math.round(v * 10) / 10; }
  function round3(v) { return Math.round(v * 1000) / 1000; }

  // ---------- Frame Geometry & Hit Testing ----------
  function frameLocalPoint(f, ix, iy) {
    const rad = -(f.angle_deg || 0) * Math.PI / 180;
    const dx = ix - f.cx;
    const dy = iy - f.cy;
    return [dx * Math.cos(rad) - dy * Math.sin(rad), dx * Math.sin(rad) + dy * Math.cos(rad)];
  }

  function localToScanPoint(f, lx, ly) {
    const rad = (f.angle_deg || 0) * Math.PI / 180;
    return [
      f.cx + lx * Math.cos(rad) - ly * Math.sin(rad),
      f.cy + lx * Math.sin(rad) + ly * Math.cos(rad),
    ];
  }

  function getInsetScale(f) {
    return Math.max(0.1, 1.0 - 2.0 * ((f.inset_pct ?? state.insetPct) / 100.0));
  }

  function hitTest(screenX, screenY) {
    const [ix, iy] = screenToScan(screenX, screenY);
    const sel = getSelectedFrame();
    // 1. Check handles on selected frame's actual exported (inset) box first
    if (sel) {
      const sc = getInsetScale(sel);
      const hw = sel.width * sc * 0.5;
      const hh = sel.height * sc * 0.5;
      const handlePts = [
        ["nw", -hw, -hh], ["n", 0, -hh], ["ne", hw, -hh],
        ["e", hw, 0], ["se", hw, hh], ["s", 0, hh],
        ["sw", -hw, hh], ["w", -hw, 0],
      ];
      // Rotation knob sits 22px above top-center in screen pixels
      const rotOffImg = 22 / state.zoom;
      const [rx, ry] = localToScanPoint(sel, 0, -hh - rotOffImg);
      const [rsx, rsy] = scanToScreen(rx, ry);
      if (Math.hypot(screenX - rsx, screenY - rsy) <= 9) {
        return { kind: "rotate", frame: sel };
      }
      for (const [name, lx, ly] of handlePts) {
        const [px, py] = localToScanPoint(sel, lx, ly);
        const [psx, psy] = scanToScreen(px, py);
        if (Math.abs(screenX - psx) <= 7 && Math.abs(screenY - psy) <= 7) {
          return { kind: "resize", handle: name, frame: sel };
        }
      }
    }
    // 2. Check frame bodies (top-most / last drawn first)
    for (let i = state.frames.length - 1; i >= 0; i--) {
      const f = state.frames[i];
      const sc = getInsetScale(f);
      const [lx, ly] = frameLocalPoint(f, ix, iy);
      if (Math.abs(lx) <= f.width * sc * 0.5 && Math.abs(ly) <= f.height * sc * 0.5) {
        return { kind: "body", frame: f };
      }
    }
    return null;
  }

  // ---------- 60fps Canvas Renderer ----------
  function resizeCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const rect = vpWrap.getBoundingClientRect();
    const w = Math.max(1, Math.floor(rect.width * dpr));
    const h = Math.max(1, Math.floor(rect.height * dpr));
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
  }

  function render() {
    resizeCanvas();
    const dpr = window.devicePixelRatio || 1;
    const rect = vpWrap.getBoundingClientRect();

    ctx.save();
    ctx.scale(dpr, dpr);
    ctx.fillStyle = "#000000";
    ctx.fillRect(0, 0, rect.width, rect.height);

    if (!state.activeScan) {
      ctx.fillStyle = "#808080";
      ctx.font = '13px "Ioskeley Mono", monospace';
      ctx.textAlign = "center";
      ctx.fillText("NO SCAN LOADED — CLICK SCAN HOLDER OR PICK A SCAN", rect.width * 0.5, rect.height * 0.5);
      ctx.restore();
      return;
    }

    const [sw, sh] = getScanDims();
    const viewRad = (state.viewRot * Math.PI) / 180;

    // 1. Base scan overview + crisp native-res viewport tile + IR mask (in rotated scan space)
    ctx.save();
    ctx.translate(state.panX, state.panY);
    ctx.scale(state.zoom, state.zoom);
    ctx.rotate(viewRad);

    if (state.img) {
      ctx.imageSmoothingEnabled = state.zoom < 2.5;
      ctx.drawImage(state.img, 0, 0, sw, sh);
    } else {
      ctx.strokeStyle = "#404040";
      ctx.lineWidth = 1 / state.zoom;
      ctx.strokeRect(0, 0, sw, sh);
    }
    if (state.hiResTile && state.hiResTile.img) {
      const t = state.hiResTile;
      ctx.imageSmoothingEnabled = state.zoom < 2.5;
      ctx.drawImage(t.img, t.rx, t.ry, t.rw, t.rh);
    }
    if (state.showIrMask) {
      if (state.hiResMask && state.hiResMask.img) {
        const m = state.hiResMask;
        ctx.drawImage(m.img, m.rx, m.ry, m.rw, m.rh);
      } else if (state.maskImg) {
        ctx.drawImage(state.maskImg, 0, 0, sw, sh);
      }
    }
    ctx.restore();

    // 3. Draw all frames (outer pre-inset reference + solid exported crop box)
    ctx.font = '700 11px "Ioskeley Mono", monospace';
    ctx.textBaseline = "top";
    const rotArrows = { 0: "", 90: " >", 180: " v", 270: " <" };

    state.frames.forEach((f, idx) => {
      const isSel = f.id === state.selectedId;
      const stripColor = STRIP_COLORS[(f.strip_index || 0) % STRIP_COLORS.length];
      const strokeCol = isSel ? "#00ffff" : stripColor;

      const [scx, scy] = scanToScreen(f.cx, f.cy);
      const rw = f.width * state.zoom;
      const rh = f.height * state.zoom;
      const insetPct = f.inset_pct ?? state.insetPct;
      const insetScale = getInsetScale(f);
      const iw = rw * insetScale;
      const ih = rh * insetScale;

      ctx.save();
      ctx.translate(scx, scy);
      ctx.rotate((((f.angle_deg || 0) + state.viewRot) * Math.PI) / 180);

      // Faint outer pre-inset reference box (only shown when inset > 0)
      if (insetPct > 0.01) {
        ctx.strokeStyle = isSel ? "rgba(0,255,255,0.35)" : "rgba(255,255,255,0.20)";
        ctx.lineWidth = 1;
        ctx.setLineDash([4, 3]);
        ctx.strokeRect(-rw * 0.5, -rh * 0.5, rw, rh);
        ctx.setLineDash([]);
      }

      // Exact exported crop box (solid, zero interior tint so image borders stay crystal clear)
      ctx.strokeStyle = strokeCol;
      ctx.lineWidth = isSel ? 1.5 : 1.25;
      ctx.strokeRect(-iw * 0.5, -ih * 0.5, iw, ih);

      // Corner badge with frame number (+ export rotation hint if non-zero)
      const r90 = ((f.rot90 || 0) % 360 + 360) % 360;
      const label = String(idx + 1).padStart(2, "0") + (rotArrows[r90] || "");
      const bx = -iw * 0.5;
      const by = -ih * 0.5;
      ctx.fillStyle = "rgba(8, 8, 8, 0.85)";
      ctx.fillRect(bx, by, label.length * 7 + 8, 15);
      ctx.fillStyle = isSel ? "#00ffff" : stripColor;
      ctx.fillText(label, bx + 4, by + 2);

      // Handles directly on the actual exported crop border
      if (isSel) {
        const hs = 6;
        const pts = [
          [-iw * 0.5, -ih * 0.5], [0, -ih * 0.5], [iw * 0.5, -ih * 0.5],
          [iw * 0.5, 0], [iw * 0.5, ih * 0.5], [0, ih * 0.5],
          [-iw * 0.5, ih * 0.5], [-iw * 0.5, 0],
        ];
        ctx.fillStyle = "#080808";
        ctx.strokeStyle = "#00ffff";
        ctx.lineWidth = 1.5;
        for (const [hx, hy] of pts) {
          ctx.fillRect(hx - hs * 0.5, hy - hs * 0.5, hs, hs);
          ctx.strokeRect(hx - hs * 0.5, hy - hs * 0.5, hs, hs);
        }
        // Rotation handle stem
        ctx.beginPath();
        ctx.moveTo(0, -ih * 0.5);
        ctx.lineTo(0, -ih * 0.5 - 22);
        ctx.stroke();
        ctx.fillStyle = "#ffff00";
        ctx.fillRect(-4, -ih * 0.5 - 26, 8, 8);
      }

      ctx.restore();
    });

    // 4. Active manual box drawing preview (aligned with scan axes)
    if (state.drag && state.drag.type === "draw") {
      const d = state.drag;
      const p0 = scanToScreen(Math.min(d.x0, d.x1), Math.min(d.y0, d.y1));
      const p1 = scanToScreen(Math.max(d.x0, d.x1), Math.max(d.y0, d.y1));
      const sx0 = Math.min(p0[0], p1[0]), sy0 = Math.min(p0[1], p1[1]);
      const sx1 = Math.max(p0[0], p1[0]), sy1 = Math.max(p0[1], p1[1]);
      ctx.strokeStyle = "#ffff00";
      ctx.lineWidth = 1.5;
      ctx.strokeRect(sx0, sy0, sx1 - sx0, sy1 - sy0);
      ctx.fillStyle = "rgba(255,255,0,0.12)";
      ctx.fillRect(sx0, sy0, sx1 - sx0, sy1 - sy0);
    }

    ctx.restore();
  }

  // ---------- Pointer Interaction ----------
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    const rect = canvas.getBoundingClientRect();
    const factor = e.deltaY < 0 ? 1.16 : 1 / 1.16;
    zoomAt(e.clientX - rect.left, e.clientY - rect.top, factor);
  }, { passive: false });

  canvas.addEventListener("dblclick", (e) => {
    if (!state.activeScan) return;
    const rect = canvas.getBoundingClientRect();
    const hit = hitTest(e.clientX - rect.left, e.clientY - rect.top);
    if (hit && hit.frame) {
      state.selectedId = hit.frame.id;
      state.activeStripIndex = hit.frame.strip_index || 0;
      syncUI();
      zoomToFrame(hit.frame);
    }
  });

  canvas.addEventListener("contextmenu", (e) => {
    e.preventDefault();
    const rect = canvas.getBoundingClientRect();
    const hit = hitTest(e.clientX - rect.left, e.clientY - rect.top);
    if (hit && hit.frame) {
      state.selectedId = hit.frame.id;
      deleteSelectedFrame();
      toast("DELETED FRAME (CTRL+Z TO UNDO)");
    }
  });

  canvas.addEventListener("pointerdown", (e) => {
    if (!state.activeScan) return;
    const rect = canvas.getBoundingClientRect();
    const sx = e.clientX - rect.left;
    const sy = e.clientY - rect.top;
    const [ix, iy] = screenToScan(sx, sy);

    canvas.setPointerCapture(e.pointerId);

    // Middle click or Space+Left click -> pan
    if (e.button === 1 || (e.button === 0 && state.spaceDown)) {
      state.drag = { type: "pan", startSx: sx, startSy: sy, origPanX: state.panX, origPanY: state.panY };
      vpWrap.classList.add("panning");
      return;
    }
    if (e.button !== 0) return;

    // Manual draw mode
    if (state.drawMode) {
      state.drag = { type: "draw", x0: ix, y0: iy, x1: ix, y1: iy };
      return;
    }

    const hit = hitTest(sx, sy);
    if (!hit) {
      state.selectedId = null;
      // Clicking empty canvas drags to pan
      state.drag = { type: "pan", startSx: sx, startSy: sy, origPanX: state.panX, origPanY: state.panY };
      vpWrap.classList.add("panning");
      syncUI();
      render();
      return;
    }

    state.selectedId = hit.frame.id;
    state.activeStripIndex = hit.frame.strip_index || 0;
    const beforeSnap = captureSnapshot();

    if (hit.kind === "rotate") {
      state.drag = { type: "rotate", frame: hit.frame, beforeSnap };
    } else if (hit.kind === "resize") {
      state.drag = {
        type: "resize",
        handle: hit.handle,
        frame: hit.frame,
        orig: { ...hit.frame },
        startIx: ix,
        startIy: iy,
        beforeSnap,
      };
    } else {
      // Shift+Drag moves entire strip together
      const targets = e.shiftKey
        ? state.frames.filter((f) => f.strip_index === hit.frame.strip_index)
        : [hit.frame];
      state.drag = {
        type: "move",
        startIx: ix,
        startIy: iy,
        origins: targets.map((f) => ({ f, cx: f.cx, cy: f.cy })),
        beforeSnap,
      };
    }
    syncUI();
    render();
  });

  canvas.addEventListener("pointermove", (e) => {
    const rect = canvas.getBoundingClientRect();
    const sx = e.clientX - rect.left;
    const sy = e.clientY - rect.top;
    const [ix, iy] = screenToScan(sx, sy);
    state.cursorImgX = ix;
    state.cursorImgY = iy;
    updateHud();

    if (!state.drag) return;
    const d = state.drag;

    if (d.type === "pan") {
      state.panX = d.origPanX + (sx - d.startSx);
      state.panY = d.origPanY + (sy - d.startSy);
      render();
    } else if (d.type === "move") {
      const dx = ix - d.startIx;
      const dy = iy - d.startIy;
      for (const item of d.origins) {
        item.f.cx = round1(item.cx + dx);
        item.f.cy = round1(item.cy + dy);
      }
      updateSelectedKV();
      render();
    } else if (d.type === "rotate") {
      const ang = Math.atan2(iy - d.frame.cy, ix - d.frame.cx) * (180 / Math.PI) + 90;
      d.frame.angle_deg = round3(Math.max(-45, Math.min(45, ang)));
      updateSelectedKV();
      render();
    } else if (d.type === "resize") {
      const f = d.frame;
      const orig = d.orig;
      const sc = getInsetScale(orig);
      const rad = -(orig.angle_deg || 0) * Math.PI / 180;
      const ddx = ix - d.startIx;
      const ddy = iy - d.startIy;
      const dlx = ddx * Math.cos(rad) - ddy * Math.sin(rad);
      const dly = ddx * Math.sin(rad) + ddy * Math.cos(rad);

      // Operate directly in exported (inset) box coordinates so handles track the cursor 1:1
      let left = -orig.width * sc * 0.5;
      let right = orig.width * sc * 0.5;
      let top = -orig.height * sc * 0.5;
      let bottom = orig.height * sc * 0.5;

      if (d.handle.includes("w")) left += dlx;
      if (d.handle.includes("e")) right += dlx;
      if (d.handle.includes("n")) top += dly;
      if (d.handle.includes("s")) bottom += dly;

      const nw = Math.max(16, (right - left) / sc);
      const nh = Math.max(16, (bottom - top) / sc);
      const shiftX = (left + right) * 0.5;
      const shiftY = (top + bottom) * 0.5;
      const [ncx, ncy] = localToScanPoint(orig, shiftX, shiftY);

      f.cx = round1(ncx);
      f.cy = round1(ncy);
      f.width = round1(nw);
      f.height = round1(nh);
      updateSelectedKV();
      render();
    } else if (d.type === "draw") {
      d.x1 = ix;
      d.y1 = iy;
      render();
    }
  });

  canvas.addEventListener("pointerup", () => {
    if (!state.drag) return;
    const d = state.drag;
    if (d.type === "draw") {
      const w = Math.abs(d.x1 - d.x0);
      const h = Math.abs(d.y1 - d.y0);
      if (w >= 16 && h >= 16) {
        pushUndo();
        const cx = 0.5 * (d.x0 + d.x1);
        const cy = 0.5 * (d.y0 + d.y1);
        // Assign nearest strip index
        let sIdx = state.activeStripIndex;
        for (const st of state.strips) {
          if (cx >= st.x0 - 20 && cx <= st.x1 + 20) { sIdx = st.index; break; }
        }
        const nf = {
          id: nextFrameId(),
          strip_index: sIdx,
          cx: round1(cx),
          cy: round1(cy),
          width: round1(w),
          height: round1(h),
          angle_deg: 0.0,
          inset_pct: state.insetPct,
        };
        state.frames.push(nf);
        state.selectedId = nf.id;
        state.drawMode = false;
        setToggle("btn-draw-mode", false);
        vpWrap.classList.add("mode-select");
      }
    } else if (d.beforeSnap && JSON.stringify(state.frames) !== d.beforeSnap.framesJson) {
      pushUndo(d.beforeSnap);
    }
    const wasPan = d.type === "pan";
    state.drag = null;
    vpWrap.classList.remove("panning");
    syncUI();
    render();
    if (wasPan) scheduleHiResTile();
  });

  // ---------- UI Sync ----------
  function updateHud() {
    $("hud-mode").textContent = state.drawMode ? "DRAW BOX" : "SELECT";
    const hdSuffix = state.hiResTile ? " · 1:1 HD" : "";
    const rotSuffix = state.viewRot ? ` · ROT ${state.viewRot}°` : "";
    $("hud-view").textContent = (state.showIrRaw
      ? "RAW IR"
      : (state.invertPreview ? `POS (${$("in-filmtype").value.toUpperCase()})` : "RAW")) + hdSuffix + rotSuffix;
    $("hud-count").textContent = String(state.frames.length);
    $("hud-zoom").textContent = `${Math.round(state.zoom * 100)}%`;
    const rotBtn = $("btn-view-rot");
    if (rotBtn) {
      rotBtn.innerHTML = state.viewRot
        ? `ROT ${state.viewRot}&deg; <span class="key">L</span>`
        : `ROT VIEW <span class="key">L</span>`;
    }

    if (state.activeScan && state.cursorImgX !== null) {
      const dpi = state.activeScan.dpi || 300;
      const xmm = (state.cursorImgX / dpi) * 25.4;
      const ymm = (state.cursorImgY / dpi) * 25.4;
      $("hud-xy").textContent = `${Math.round(state.cursorImgX)},${Math.round(state.cursorImgY)} px (${xmm.toFixed(1)},${ymm.toFixed(1)} mm)`;
    } else {
      $("hud-xy").textContent = "--";
    }

    const sel = getSelectedFrame();
    if (sel && state.activeScan) {
      const dpi = state.activeScan.dpi || 300;
      const wmm = (sel.width / dpi) * 25.4;
      const hmm = (sel.height / dpi) * 25.4;
      $("hud-sel").textContent = `${sel.id.toUpperCase()} · ${wmm.toFixed(1)}x${hmm.toFixed(1)} mm`;
    } else {
      $("hud-sel").textContent = "NONE";
    }
  }

  function updateSelectedKV() {
    const f = getSelectedFrame();
    if (!f || !state.activeScan) {
      $("kv-id").textContent = "--";
      $("kv-mm").textContent = "--";
      $("kv-px").textContent = "--";
      $("kv-xy").textContent = "--";
      $("kv-ang").textContent = "0.00 deg";
      $("kv-rot90").textContent = "0 deg";
      $("kv-ins").textContent = `${state.insetPct.toFixed(1)}%`;
      updateHud();
      return;
    }
    const dpi = state.activeScan.dpi || 300;
    const scale = Math.max(0.1, 1.0 - 2.0 * ((f.inset_pct ?? state.insetPct) / 100.0));
    const wmm = (f.width * scale / dpi) * 25.4;
    const hmm = (f.height * scale / dpi) * 25.4;
    $("kv-id").textContent = `${f.id.toUpperCase()} (STRIP ${(f.strip_index || 0) + 1})`;
    $("kv-mm").textContent = `${wmm.toFixed(1)} x ${hmm.toFixed(1)} mm`;
    $("kv-px").textContent = `${Math.round(f.width * scale)} x ${Math.round(f.height * scale)} px`;
    $("kv-xy").textContent = `${Math.round(f.cx)}, ${Math.round(f.cy)}`;
    $("kv-ang").textContent = `${(f.angle_deg || 0).toFixed(2)} deg`;
    $("kv-rot90").textContent = `${((f.rot90 || 0) % 360 + 360) % 360} deg`;
    $("kv-ins").textContent = `${(f.inset_pct ?? state.insetPct).toFixed(1)}%`;
    updateHud();
  }

  function syncStripsUI() {
    const box = $("strips-container");
    box.innerHTML = "";
    const stripIndices = new Set(state.strips.map((s) => s.index));
    for (const f of state.frames) stripIndices.add(f.strip_index || 0);
    const sorted = Array.from(stripIndices).sort((a, b) => a - b);

    if (!sorted.length) {
      box.innerHTML = '<div class="dim" style="font-size:12px;">NO STRIPS DETECTED</div>';
      return;
    }

    const stepPx = state.activeScan ? Math.max(2, Math.round(state.activeScan.dpi / 100)) : 4;
    for (const idx of sorted) {
      const col = STRIP_COLORS[idx % STRIP_COLORS.length];
      const cnt = state.frames.filter((f) => (f.strip_index || 0) === idx).length;
      const card = document.createElement("div");
      card.className = "strip-card" + (state.activeStripIndex === idx ? " active" : "");
      card.innerHTML = `
        <div class="strip-head">
          <span><span class="sw" style="--c:${col}"></span>STRIP ${idx + 1}</span>
          <span class="val">${cnt} FRM</span>
        </div>
        <div class="nudge-grid">
          <button data-act="l" title="Nudge strip left">&larr;</button>
          <button data-act="r" title="Nudge strip right">&rarr;</button>
          <button data-act="u" title="Nudge strip up">&uarr;</button>
          <button data-act="d" title="Nudge strip down">&darr;</button>
          <button data-act="ccw" title="Tilt strip -0.2 deg">[</button>
          <button data-act="cw" title="Tilt strip +0.2 deg">]</button>
        </div>
      `;
      card.addEventListener("click", () => { state.activeStripIndex = idx; syncStripsUI(); });
      card.querySelectorAll("button").forEach((btn) => {
        btn.addEventListener("click", (ev) => {
          ev.stopPropagation();
          state.activeStripIndex = idx;
          const mult = ev.shiftKey ? 5 : 1;
          const act = btn.dataset.act;
          if (act === "l") nudgeScreen(-stepPx * mult, 0, 0, true, idx);
          if (act === "r") nudgeScreen(stepPx * mult, 0, 0, true, idx);
          if (act === "u") nudgeScreen(0, -stepPx * mult, 0, true, idx);
          if (act === "d") nudgeScreen(0, stepPx * mult, 0, true, idx);
          if (act === "ccw") nudgeFrameOrStrip(0, 0, -0.2 * mult, true, idx);
          if (act === "cw") nudgeFrameOrStrip(0, 0, 0.2 * mult, true, idx);
        });
      });
      box.appendChild(card);
    }
  }

  function syncFrameListUI() {
    const list = $("frame-list");
    $("frame-list-count").textContent = String(state.frames.length);
    list.innerHTML = "";
    const dpi = (state.activeScan && state.activeScan.dpi) || 300;
    state.frames.forEach((f, idx) => {
      const col = STRIP_COLORS[(f.strip_index || 0) % STRIP_COLORS.length];
      const row = document.createElement("div");
      row.className = "frame-row" + (f.id === state.selectedId ? " selected" : "");
      const wmm = ((f.width / dpi) * 25.4).toFixed(0);
      const hmm = ((f.height / dpi) * 25.4).toFixed(0);
      row.innerHTML = `
        <span class="sw" style="--c:${col};margin:0;"></span>
        <span>#${String(idx + 1).padStart(2, "0")}</span>
        <span class="dim">${wmm}x${hmm}mm</span>
        <span>${(f.angle_deg || 0).toFixed(1)}&deg;</span>
      `;
      row.addEventListener("click", () => {
        state.selectedId = f.id;
        state.activeStripIndex = f.strip_index || 0;
        syncUI();
        render();
      });
      row.addEventListener("dblclick", () => {
        state.selectedId = f.id;
        state.activeStripIndex = f.strip_index || 0;
        syncUI();
        zoomToFrame(f);
      });
      list.appendChild(row);
    });
  }

  function syncUI() {
    updateSelectedKV();
    syncStripsUI();
    syncFrameListUI();
  }

  // ---------- Scanner Job & Live SSE ----------
  function updateJobUI(job, scanner) {
    const usbTag = $("usb-tag");
    if (scanner && scanner.connected) {
      usbTag.textContent = scanner.accessible ? "READY" : "NO PERM";
      usbTag.className = "status-tag " + (scanner.accessible ? "ok" : "warn");
    } else {
      usbTag.textContent = "OFFLINE";
      usbTag.className = "status-tag err";
    }

    const jtag = $("job-tag");
    const st = (job && job.status) || "idle";
    jtag.textContent = st.toUpperCase();
    jtag.className = "status-tag " + (
      st === "running" ? "run" :
      st === "done" ? "ok" :
      st === "error" ? "err" :
      st === "cancelled" ? "warn" : "ok"
    );
    const elapsedSec = (job && job.elapsed_sec) || 0;
    const fmtElapsed = elapsedSec >= 60
      ? `${Math.floor(elapsedSec / 60)}m ${String(Math.floor(elapsedSec % 60)).padStart(2, "0")}s`
      : `${elapsedSec.toFixed(1)}s`;
    $("job-elapsed").textContent = fmtElapsed;
    const pct = Math.min(100, Math.max(0, (job && job.progress) || 0));
    $("scan-bar").style.width = `${pct}%`;
    const pctEl = $("scan-pct");
    if (pctEl) pctEl.textContent = st === "idle" ? "0%" : `${Math.round(pct)}%`;
    const stageTxt = (job && job.stage) || "READY";
    $("scan-stage").textContent = stageTxt;
    $("scan-stage").title = stageTxt;

    const running = st === "running";
    $("btn-scan").disabled = running;
    $("btn-cancel-scan").disabled = !running;

    if (job && Array.isArray(job.logs)) {
      const logKey = job.logs.join("\n");
      if (logKey !== state.lastLogKey) {
        state.lastLogKey = logKey;
        const logBox = $("scan-logs");
        const nearBottom = (logBox.scrollHeight - logBox.scrollTop - logBox.clientHeight) < 28;
        logBox.innerHTML = job.logs.map((line) => {
          const cls = line.includes("ERR:") ? "log-err" : (line.includes("Saved ") ? "log-ok" : "");
          return `<div class="${cls}">${escapeHtml(line)}</div>`;
        }).join("");
        if (nearBottom) logBox.scrollTop = logBox.scrollHeight;
      }
    }

    if (st === "done" && job.output_file && job.job_id !== state.lastCompletedJobId) {
      state.lastCompletedJobId = job.job_id;
      toast(`SCAN COMPLETE: ${job.output_file} — READY IN SCAN DROPDOWN`, 5000);
      refreshScans(null, false);
    }
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }

  function connectSSE() {
    const es = new EventSource("/api/events");
    es.onmessage = (ev) => {
      try {
        const data = JSON.parse(ev.data);
        updateJobUI(data.job, data.scanner);
      } catch (_) {}
    };
    es.onerror = () => {
      es.close();
      setTimeout(connectSSE, 3000);
    };
  }

  // ---------- Export Action ----------
  async function runExport() {
    if (!state.activeScan || !state.frames.length) {
      toast("NO FRAMES TO EXPORT");
      return;
    }
    await saveLayout(true);
    const outBox = $("export-status");
    const outTxt = $("export-status-text");
    outBox.hidden = false;
    outBox.className = "callout info";
    outBox.querySelector(".tag").textContent = "RUN";
    outTxt.textContent = `EXPORTING ${state.frames.length} FRAMES (16-BIT)...`;
    $("btn-export").disabled = true;
    $("btn-export-top").disabled = true;

    try {
      const fallbackStem = state.activeScan.name.replace(/\.(tif|tiff)$/i, "");
      const res = await apiPost("/api/export", {
        scan_name: state.activeScan.name,
        frames: state.frames,
        out_dir: $("in-outdir").value.trim(),
        roll_name: $("in-roll").value.trim() || fallbackStem || "roll",
        apply_ir_clean: getToggle("tog-exp-ir"),
        ir_sensitivity: state.irSens,
        gamma_22: getToggle("tog-exp-gamma"),
        export_raw_tiff: getToggle("tog-exp-raw"),
        export_dev_tiff: getToggle("tog-exp-dev-tiff"),
        export_dev_jpg: getToggle("tog-exp-proof"),
        jpg_max_dim: parseInt($("in-jpg-size").value, 10) || 0,
        camera: $("in-camera").value.trim(),
        film_stock: $("in-stock").value.trim(),
        film_base: state.filmBase,
        film_type: $("in-filmtype").value,
      });
      outBox.className = "callout ok";
      outBox.querySelector(".tag").textContent = "OK";
      outTxt.textContent = `EXPORTED ${res.count} FRAMES IN ${res.elapsed_ms} MS -> ${res.out_dir}`;
      toast(`EXPORTED ${res.count} FRAMES IN ${res.elapsed_ms} MS`);
    } catch (err) {
      outBox.className = "callout err";
      outBox.querySelector(".tag").textContent = "ERR";
      outTxt.textContent = err.message;
      toast(`EXPORT ERR: ${err.message}`);
    } finally {
      $("btn-export").disabled = false;
      $("btn-export-top").disabled = false;
    }
  }

  // ---------- Wire Controls ----------
  function bindToggle(id, onChange) {
    $(id).addEventListener("click", () => {
      const next = !getToggle(id);
      setToggle(id, next);
      if (onChange) onChange(next);
    });
  }

  function syncDpiLabels() {
    const irOn = getToggle("tog-ir");
    const isSf = $("in-backend").value === "silverfast";
    const sel = $("in-dpi");
    for (const opt of sel.options) {
      if (opt.value === "3000") opt.textContent = (isSf && irOn) ? "3000 DPI (MAX 1-PASS HDRi)" : "3000 DPI";
      if (opt.value === "3200") opt.textContent = (isSf && irOn) ? "3200 DPI (AUTO-SPLIT 2x)" : "3200 DPI (1-PASS)";
      if (opt.value === "4800") opt.textContent = isSf ? (irOn ? "4800 DPI (AUTO-SPLIT 4x)" : "4800 DPI (AUTO-SPLIT 2x)") : "4800 DPI";
    }
  }

  bindToggle("tog-me");
  bindToggle("tog-ir", syncDpiLabels);
  bindToggle("tog-exp-ir");
  bindToggle("tog-exp-gamma");
  bindToggle("tog-exp-raw");
  bindToggle("tog-exp-dev-tiff");
  bindToggle("tog-exp-proof");
  bindToggle("tog-skip-empty", (v) => { state.skipEmpty = v; });

  // ---------- Film Lab Develop Mode Controller ----------
  function ensureFrameDevelop(f) {
    if (!f) return { ...DEFAULT_DEVELOP };
    if (!f.develop || typeof f.develop !== "object") {
      f.develop = { ...DEFAULT_DEVELOP };
    } else {
      f.develop = { ...DEFAULT_DEVELOP, ...f.develop };
    }
    return f.develop;
  }

  function updateBaseReadout() {
    const el = $("dev-base-readout");
    if (!el) return;
    if (state.filmBase && state.filmBase.length === 3) {
      const [r, g, b] = state.filmBase.map((v) => Math.round(v * 100));
      el.textContent = `BASE ${r}/${g}/${b}`;
    } else {
      el.textContent = "BASE: AUTO";
    }
  }

  async function sampleFilmBase(silent = false) {
    if (!state.activeScan || state.frames.length < 2) return;
    try {
      const res = await apiPost(`/api/scan/${encodeURIComponent(state.activeScan.name)}/film_base`, {
        frames: state.frames,
      });
      if (res.ok && Array.isArray(res.base)) {
        state.filmBase = res.base;
        updateBaseReadout();
        scheduleAutoSave();
        if (!silent) toast(`SAMPLED FILM BASE: ${res.base.map((v) => v.toFixed(2)).join(" / ")}`);
        if (state.uiMode === "develop") scheduleDevelopPreview(20);
      } else if (!silent) {
        toast("COULD NOT SAMPLE INTER-FRAME REBATE");
      }
    } catch (err) {
      if (!silent) toast(`BASE ERR: ${err.message}`);
    }
  }

  function syncDevelopUI() {
    const f = getSelectedFrame();
    const d = ensureFrameDevelop(f);
    $("dev-profile").value = d.profile || "frontier";
    setToggle("dev-autowb", d.auto_wb !== false);
    setToggle("dev-wb-pick", state.wbPickMode);
    setToggle("dev-ir-prev", state.devIrPrev);
    setToggle("dev-before", state.devBefore);

    $("dev-exp").value = String(d.exposure);
    $("val-dev-exp").textContent = (d.exposure >= 0 ? "+" : "") + Number(d.exposure).toFixed(2);
    $("dev-con").value = String(d.contrast);
    $("val-dev-con").textContent = Number(d.contrast).toFixed(2);
    $("dev-blk").value = String(d.blacks);
    $("val-dev-blk").textContent = (d.blacks >= 0 ? "+" : "") + Number(d.blacks).toFixed(2);
    $("dev-temp").value = String(d.temp);
    $("val-dev-temp").textContent = (d.temp >= 0 ? "+" : "") + Number(d.temp).toFixed(2);
    $("dev-tint").value = String(d.tint);
    $("val-dev-tint").textContent = (d.tint >= 0 ? "+" : "") + Number(d.tint).toFixed(2);
    $("dev-sat").value = String(d.saturation);
    $("val-dev-sat").textContent = Number(d.saturation).toFixed(2);
    $("dev-cnr").value = String(d.chroma_nr);
    $("val-dev-cnr").textContent = `${Math.round(d.chroma_nr * 100)}%`;
    $("dev-lnr").value = String(d.luma_nr);
    $("val-dev-lnr").textContent = `${Math.round(d.luma_nr * 100)}%`;
    $("dev-shp").value = String(d.sharpen);
    $("val-dev-shp").textContent = `${Math.round(d.sharpen * 100)}%`;

    updateBaseReadout();
    const idx = f ? state.frames.findIndex((x) => x.id === f.id) + 1 : 0;
    $("dev-hud-frame").textContent = f ? `#${String(idx).padStart(2, "0")} / ${state.frames.length}` : "NONE";
    $("dev-hud-prof").textContent = state.devBefore ? "BEFORE (LEGACY PROOF)" : String(d.profile || "frontier").toUpperCase();
    $("dev-stage-wrap").classList.toggle("wb-pick", state.wbPickMode);
  }

  function setUiMode(mode) {
    if (mode === "develop" && (!state.activeScan || !state.frames.length)) {
      toast("LOAD A SCAN & DETECT FRAMES FIRST");
      return;
    }
    state.uiMode = mode;
    const isDev = mode === "develop";
    $("tab-crop").setAttribute("aria-selected", isDev ? "false" : "true");
    $("tab-develop").setAttribute("aria-selected", isDev ? "true" : "false");
    $("pane-crop").hidden = isDev;
    $("pane-develop").hidden = !isDev;
    $("dev-stage-wrap").hidden = !isDev;
    if (!isDev) {
      state.wbPickMode = false;
      state.devBefore = false;
      render();
      return;
    }
    if (!getSelectedFrame() && state.frames.length) {
      state.selectedId = state.frames[0].id;
      syncUI();
    }
    syncDevelopUI();
    if (!state.filmBase && $("in-filmtype").value !== "slide") {
      sampleFilmBase(true);
    }
    scheduleDevelopPreview(10);
  }

  function scheduleDevelopPreview(delayMs = 70) {
    if (state.uiMode !== "develop" || !state.activeScan) return;
    clearTimeout(state.devTimer);
    state.devTimer = setTimeout(fetchDevelopPreview, delayMs);
  }

  async function fetchDevelopPreview() {
    const f = getSelectedFrame();
    if (!state.activeScan || !f || state.uiMode !== "develop") return;
    const seq = ++state.devReqSeq;
    const d = ensureFrameDevelop(f);
    const devPayload = state.devBefore ? { ...d, enabled: false } : { ...d, enabled: true };
    $("dev-hud-state").textContent = "RENDERING...";
    const t0 = performance.now();
    try {
      const res = await fetch(`/api/scan/${encodeURIComponent(state.activeScan.name)}/develop_preview`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          frame: f,
          develop: devPayload,
          film_base: state.filmBase,
          film_type: $("in-filmtype").value,
          apply_ir_clean: state.devIrPrev && !state.devBefore,
          ir_sensitivity: state.irSens,
          max_dim: 1050,
        }),
      });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const blob = await res.blob();
      if (seq !== state.devReqSeq) return;
      const url = URL.createObjectURL(blob);
      const im = new Image();
      im.onload = () => {
        URL.revokeObjectURL(url);
        if (seq !== state.devReqSeq) return;
        const dc = $("dev-canvas");
        dc.width = im.naturalWidth;
        dc.height = im.naturalHeight;
        dc.getContext("2d").drawImage(im, 0, 0);
        const dt = Math.round(performance.now() - t0);
        $("dev-hud-state").textContent = `${im.naturalWidth}x${im.naturalHeight} · ${dt} ms`;
      };
      im.src = url;
    } catch (err) {
      if (seq === state.devReqSeq) {
        $("dev-hud-state").textContent = `ERR: ${err.message}`;
      }
    }
  }

  $("tab-crop").addEventListener("click", () => setUiMode("crop"));
  $("tab-develop").addEventListener("click", () => setUiMode("develop"));
  $("dev-prev").addEventListener("click", () => stepFrame(-1));
  $("dev-next").addEventListener("click", () => stepFrame(1));
  $("dev-rot-ccw").addEventListener("click", () => rotateSelectedExport90(-90));
  $("dev-rot-cw").addEventListener("click", () => rotateSelectedExport90(90));
  $("dev-sample-base").addEventListener("click", () => sampleFilmBase(false));

  $("dev-profile").addEventListener("change", () => {
    const f = getSelectedFrame();
    if (!f) return;
    pushUndo();
    ensureFrameDevelop(f).profile = $("dev-profile").value;
    syncDevelopUI();
    scheduleDevelopPreview(10);
  });

  bindToggle("dev-autowb", (v) => {
    const f = getSelectedFrame();
    if (!f) return;
    pushUndo();
    ensureFrameDevelop(f).auto_wb = v;
    syncDevelopUI();
    scheduleDevelopPreview(10);
  });
  bindToggle("dev-wb-pick", (v) => {
    state.wbPickMode = v;
    $("dev-stage-wrap").classList.toggle("wb-pick", v);
    if (v) toast("CLICK A NEUTRAL GRAY AREA IN THE PREVIEW TO BALANCE WB");
  });
  bindToggle("dev-ir-prev", (v) => {
    state.devIrPrev = v;
    scheduleDevelopPreview(10);
  });
  bindToggle("dev-before", (v) => {
    state.devBefore = v;
    syncDevelopUI();
    scheduleDevelopPreview(10);
  });

  // WB Eyedropper click on live developed canvas
  $("dev-canvas").addEventListener("click", (e) => {
    if (!state.wbPickMode || state.uiMode !== "develop") return;
    const f = getSelectedFrame();
    if (!f) return;
    const dc = $("dev-canvas");
    const rect = dc.getBoundingClientRect();
    const px = Math.max(2, Math.min(dc.width - 3, Math.round(((e.clientX - rect.left) / rect.width) * dc.width)));
    const py = Math.max(2, Math.min(dc.height - 3, Math.round(((e.clientY - rect.top) / rect.height) * dc.height)));
    // Average a 5x5 patch so grain doesn't jitter the WB sample
    const data = dc.getContext("2d").getImageData(px - 2, py - 2, 5, 5).data;
    let sr = 0, sg = 0, sb = 0, cnt = 0;
    for (let i = 0; i < data.length; i += 4) { sr += data[i]; sg += data[i + 1]; sb += data[i + 2]; cnt++; }
    const r = sr / (cnt * 255), g = sg / (cnt * 255), b = sb / (cnt * 255);
    const mean = (r + g + b) / 3;
    if (mean < 0.03 || mean > 0.97) {
      toast("PICK A MIDTONE GRAY (NOT PURE BLACK/WHITE)");
      return;
    }
    pushUndo();
    const d = ensureFrameDevelop(f);
    // Nudge Temp (R-B) and Tint (G vs R/B) to neutralize the clicked gray sample
    d.temp = Math.max(-1.0, Math.min(1.0, round3(d.temp + (b - r) * 1.6)));
    d.tint = Math.max(-1.0, Math.min(1.0, round3(d.tint + (g - 0.5 * (r + b)) * 1.8)));
    state.wbPickMode = false;
    syncDevelopUI();
    scheduleDevelopPreview(10);
    toast(`WB BALANCED (TEMP ${d.temp.toFixed(2)}, TINT ${d.tint.toFixed(2)})`);
  });

  // Wire all 9 Film Lab sliders with single-step Undo on drag end + live ~70ms preview
  const DEV_SLIDERS = [
    ["dev-exp", "exposure"],
    ["dev-con", "contrast"],
    ["dev-blk", "blacks"],
    ["dev-temp", "temp"],
    ["dev-tint", "tint"],
    ["dev-sat", "saturation"],
    ["dev-cnr", "chroma_nr"],
    ["dev-lnr", "luma_nr"],
    ["dev-shp", "sharpen"],
  ];
  for (const [elId, prop] of DEV_SLIDERS) {
    let beforeSnap = null;
    const el = $(elId);
    el.addEventListener("pointerdown", () => { beforeSnap = captureSnapshot(); });
    el.addEventListener("focus", () => { if (!beforeSnap) beforeSnap = captureSnapshot(); });
    el.addEventListener("input", () => {
      const f = getSelectedFrame();
      if (!f) return;
      ensureFrameDevelop(f)[prop] = parseFloat(el.value);
      syncDevelopUI();
      scheduleDevelopPreview(75);
    });
    el.addEventListener("change", () => {
      if (beforeSnap) {
        pushUndo(beforeSnap);
        beforeSnap = null;
      }
    });
  }

  function applyDevelopToScope(scope) {
    const src = getSelectedFrame();
    if (!src) return;
    const dCopy = { ...ensureFrameDevelop(src) };
    pushUndo();
    let cnt = 0;
    for (const f of state.frames) {
      if (scope === "all" || f.strip_index === src.strip_index) {
        f.develop = { ...dCopy };
        cnt++;
      }
    }
    toast(`APPLIED LOOK TO ${cnt} FRAMES (${scope.toUpperCase()})`);
  }
  $("dev-apply-strip").addEventListener("click", () => applyDevelopToScope("strip"));
  $("dev-apply-all").addEventListener("click", () => applyDevelopToScope("all"));
  $("dev-reset").addEventListener("click", () => {
    const f = getSelectedFrame();
    if (!f) return;
    pushUndo();
    f.develop = { ...DEFAULT_DEVELOP };
    syncDevelopUI();
    scheduleDevelopPreview(10);
    toast("RESET FRAME DEVELOP SETTINGS");
  });

  bindToggle("tog-invert", (v) => {
    state.invertPreview = v;
    if (v) { state.showIrRaw = false; setToggle("tog-irraw", false); }
    loadPreviewImages();
  });
  bindToggle("tog-irmask", (v) => {
    state.showIrMask = v;
    loadPreviewImages();
  });
  bindToggle("tog-irraw", (v) => {
    state.showIrRaw = v;
    loadPreviewImages();
  });
  bindToggle("btn-draw-mode", (v) => {
    state.drawMode = v;
    vpWrap.classList.toggle("mode-select", !v);
    updateHud();
  });

  $("in-backend").addEventListener("change", () => {
    const info = state.backends[$("in-backend").value];
    $("backend-note").textContent = info ? info.note : "";
    syncDpiLabels();
  });

  $("in-filmtype").addEventListener("change", () => {
    // Auto-enable Multi-Exposure for Slide film by default
    if ($("in-filmtype").value === "slide") setToggle("tog-me", true);
    if ($("in-filmtype").value === "bw_neg") {
      setToggle("tog-ir", false);
      setToggle("tog-exp-ir", false);
    } else {
      setToggle("tog-ir", true);
      setToggle("tog-exp-ir", true);
    }
    syncDpiLabels();
    if (state.invertPreview) loadPreviewImages();
  });
  syncDpiLabels();

  let insetBeforeSnap = null;
  $("in-inset").addEventListener("pointerdown", () => { insetBeforeSnap = captureSnapshot(); });
  $("in-inset").addEventListener("focus", () => { if (!insetBeforeSnap) insetBeforeSnap = captureSnapshot(); });
  $("in-inset").addEventListener("input", (e) => {
    state.insetPct = parseFloat(e.target.value);
    $("val-inset").textContent = `${state.insetPct.toFixed(1)}%`;
    for (const f of state.frames) f.inset_pct = state.insetPct;
    updateSelectedKV();
    render();
  });
  $("in-inset").addEventListener("change", () => {
    if (insetBeforeSnap) {
      pushUndo(insetBeforeSnap);
      insetBeforeSnap = null;
    }
  });

  let irSensTimer = null;
  $("in-irsens").addEventListener("input", (e) => {
    state.irSens = parseFloat(e.target.value);
    $("val-irsens").textContent = state.irSens.toFixed(2);
    if (state.showIrMask) {
      clearTimeout(irSensTimer);
      irSensTimer = setTimeout(loadPreviewImages, 150);
    }
  });

  $("scan-select").addEventListener("change", (e) => {
    if (!e.target.value) {
      closeScan();
      return;
    }
    const found = state.scans.find((s) => s.name === e.target.value);
    if (found) openScan(found);
  });
  $("btn-refresh").addEventListener("click", () => refreshScans());
  $("btn-detect").addEventListener("click", runAutoDetect);
  $("btn-clear-frames").addEventListener("click", () => {
    if (!state.frames.length) return;
    pushUndo();
    state.frames = [];
    state.selectedId = null;
    syncUI();
    render();
  });
  $("btn-view-rot").addEventListener("click", (e) => rotateViewport(e.shiftKey ? -90 : 90));
  $("btn-prev-frame").addEventListener("click", () => stepFrame(-1));
  $("btn-next-frame").addEventListener("click", () => stepFrame(1));
  $("btn-zoom-sel").addEventListener("click", () => {
    const f = getSelectedFrame() || state.frames[0];
    if (f) {
      state.selectedId = f.id;
      syncUI();
      zoomToFrame(f);
    }
  });
  $("btn-fit").addEventListener("click", fitViewport);
  $("btn-100").addEventListener("click", zoom100);
  $("btn-undo").addEventListener("click", undo);
  $("btn-redo").addEventListener("click", redo);
  $("btn-add-grid").addEventListener("click", () => stampStripGrid());
  $("btn-del-frame").addEventListener("click", deleteSelectedFrame);
  $("btn-exp-ccw").addEventListener("click", () => rotateSelectedExport90(-90));
  $("btn-exp-cw").addEventListener("click", () => rotateSelectedExport90(90));
  $("btn-dup-frame").addEventListener("click", duplicateFrameBelow);
  $("btn-rot-reset").addEventListener("click", () => {
    const f = getSelectedFrame();
    if (!f) return;
    pushUndo();
    f.angle_deg = 0.0;
    syncUI();
    render();
  });
  $("btn-save-layout").addEventListener("click", () => saveLayout(false));
  $("btn-del-raw").addEventListener("click", async () => {
    if (!state.activeScan) return;
    const btn = $("btn-del-raw");
    if (!state.delRawArmed) {
      state.delRawArmed = true;
      btn.textContent = "CONFIRM DEL";
      btn.classList.add("danger-on");
      clearTimeout(state.delRawTimer);
      state.delRawTimer = setTimeout(() => {
        state.delRawArmed = false;
        btn.textContent = "DEL RAW";
        btn.classList.remove("danger-on");
      }, 4000);
      return;
    }
    clearTimeout(state.delRawTimer);
    state.delRawArmed = false;
    btn.textContent = "DEL RAW";
    btn.classList.remove("danger-on");
    try {
      const name = state.activeScan.name;
      await apiPost(`/api/scan/${encodeURIComponent(name)}/delete`, {});
      toast(`DELETED RAW SCAN: ${name}`);
      await refreshScans();
    } catch (err) {
      toast(`DELETE ERR: ${err.message}`);
    }
  });
  $("btn-export").addEventListener("click", runExport);
  $("btn-export-top").addEventListener("click", runExport);

  document.querySelectorAll("[data-nudge]").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      const step = state.activeScan ? Math.max(2, Math.round(state.activeScan.dpi / 150)) : 2;
      const mult = e.shiftKey ? 5 : 1;
      const dir = btn.dataset.nudge;
      if (dir === "left") nudgeScreen(-step * mult, 0, 0, e.altKey);
      if (dir === "right") nudgeScreen(step * mult, 0, 0, e.altKey);
      if (dir === "up") nudgeScreen(0, -step * mult, 0, e.altKey);
      if (dir === "down") nudgeScreen(0, step * mult, 0, e.altKey);
      if (dir === "ccw") nudgeFrameOrStrip(0, 0, -0.2 * mult, e.altKey);
      if (dir === "cw") nudgeFrameOrStrip(0, 0, 0.2 * mult, e.altKey);
    });
  });

  $("btn-scan").addEventListener("click", async () => {
    try {
      const res = await apiPost("/api/scan/start", {
        backend: $("in-backend").value,
        roll_name: $("in-roll").value.trim() || "roll",
        dpi: parseInt($("in-dpi").value, 10),
        film_format: $("in-format").value,
        film_type: $("in-filmtype").value,
        multi_exposure: getToggle("tog-me"),
        ir_enabled: getToggle("tog-ir"),
      });
      updateJobUI(res.job, null);
      toast(`STARTED ${$("in-backend").value.toUpperCase()} SCAN`);
    } catch (err) {
      toast(`SCAN ERR: ${err.message}`);
    }
  });

  $("btn-cancel-scan").addEventListener("click", async () => {
    try {
      await apiPost("/api/scan/cancel", {});
      toast("CANCELLING SCAN CLEANLY");
    } catch (err) {
      toast(`CANCEL ERR: ${err.message}`);
    }
  });

  // ---------- Keyboard Shortcuts ----------
  window.addEventListener("keydown", (e) => {
    if (e.target && ["INPUT", "SELECT", "TEXTAREA"].includes(e.target.tagName)) return;
    if (e.code === "Space") { state.spaceDown = true; vpWrap.classList.add("panning"); e.preventDefault(); return; }

    const key = e.key.toLowerCase();
    if (e.key === "Tab") {
      e.preventDefault();
      setUiMode(state.uiMode === "crop" ? "develop" : "crop");
      return;
    }
    if (e.key === "\\" && state.uiMode === "develop" && !e.repeat) {
      state.devBefore = true;
      syncDevelopUI();
      scheduleDevelopPreview(5);
      return;
    }
    if ((e.ctrlKey || e.metaKey) && key === "z" && e.shiftKey) { e.preventDefault(); redo(); return; }
    if ((e.ctrlKey || e.metaKey) && key === "y") { e.preventDefault(); redo(); return; }
    if ((e.ctrlKey || e.metaKey) && key === "z") { e.preventDefault(); undo(); return; }
    if ((e.ctrlKey || e.metaKey) && key === "s") { e.preventDefault(); saveLayout(false); return; }
    if (key === "l") { rotateViewport(e.shiftKey ? -90 : 90); return; }
    if (key === "n") { stepFrame(1); return; }
    if (key === "p") { stepFrame(-1); return; }
    if (key === "z") {
      const f = getSelectedFrame() || state.frames[0];
      if (f) { state.selectedId = f.id; syncUI(); zoomToFrame(f); }
      return;
    }
    if (key === "q") { rotateSelectedExport90(-90); return; }
    if (key === "w") { rotateSelectedExport90(90); return; }
    if (key === "f") { fitViewport(); return; }
    if (key === "1") { zoom100(); return; }
    if (key === "i") { $("tog-invert").click(); return; }
    if (key === "m" && !$("tog-irmask").disabled) { $("tog-irmask").click(); return; }
    if (key === "r" && !$("tog-irraw").disabled) { $("tog-irraw").click(); return; }
    if (key === "d") { runAutoDetect(); return; }
    if (key === "b") { $("btn-draw-mode").click(); return; }
    if (key === "g") { stampStripGrid(); return; }
    if (key === "e") { runExport(); return; }
    if (e.key === "Delete" || e.key === "Backspace") { deleteSelectedFrame(); return; }
    if (e.key === "Escape") {
      if (state.drawMode) $("btn-draw-mode").click();
      return;
    }

    const step = state.activeScan ? Math.max(2, Math.round(state.activeScan.dpi / 150)) : 3;
    if (e.key === "ArrowLeft") { e.preventDefault(); nudgeScreen(-step, 0, 0, e.shiftKey); }
    if (e.key === "ArrowRight") { e.preventDefault(); nudgeScreen(step, 0, 0, e.shiftKey); }
    if (e.key === "ArrowUp") { e.preventDefault(); nudgeScreen(0, -step, 0, e.shiftKey); }
    if (e.key === "ArrowDown") { e.preventDefault(); nudgeScreen(0, step, 0, e.shiftKey); }
    if (e.key === "[") { nudgeFrameOrStrip(0, 0, e.shiftKey ? -1.0 : -0.2, false); }
    if (e.key === "]") { nudgeFrameOrStrip(0, 0, e.shiftKey ? 1.0 : 0.2, false); }
  });

  window.addEventListener("keyup", (e) => {
    if (e.code === "Space") { state.spaceDown = false; vpWrap.classList.remove("panning"); }
    if (e.key === "\\" && state.uiMode === "develop" && state.devBefore) {
      state.devBefore = false;
      syncDevelopUI();
      scheduleDevelopPreview(5);
    }
  });

  window.addEventListener("resize", () => render());

  // ---------- ImGui-Style Resizable Dock Splitters ----------
  function initSplitters() {
    const ws = document.querySelector(".workspace");
    if (!ws) return;
    const DEF_L = 340;
    const DEF_R = 270;

    function applyWidths(lw, rw) {
      ws.style.setProperty("--left-w", `${Math.round(lw)}px`);
      ws.style.setProperty("--right-w", `${Math.round(rw)}px`);
      render();
    }

    let leftW = parseInt(localStorage.getItem("fc_left_w") || String(DEF_L), 10) || DEF_L;
    let rightW = parseInt(localStorage.getItem("fc_right_w") || String(DEF_R), 10) || DEF_R;
    leftW = Math.min(720, Math.max(200, leftW));
    rightW = Math.min(580, Math.max(200, rightW));
    applyWidths(leftW, rightW);

    function wireSplitter(el, side) {
      if (!el) return;
      let dragging = false;
      let startX = 0;
      let origW = 0;

      el.addEventListener("pointerdown", (e) => {
        if (e.button !== 0) return;
        dragging = true;
        startX = e.clientX;
        origW = side === "left" ? leftW : rightW;
        el.classList.add("dragging");
        el.setPointerCapture(e.pointerId);
        e.preventDefault();
      });

      el.addEventListener("pointermove", (e) => {
        if (!dragging) return;
        const dx = e.clientX - startX;
        const maxW = Math.max(360, Math.floor(ws.getBoundingClientRect().width * 0.48));
        if (side === "left") {
          leftW = Math.min(maxW, Math.max(200, origW + dx));
        } else {
          rightW = Math.min(maxW, Math.max(200, origW - dx));
        }
        applyWidths(leftW, rightW);
      });

      const endDrag = () => {
        if (!dragging) return;
        dragging = false;
        el.classList.remove("dragging");
        localStorage.setItem("fc_left_w", String(Math.round(leftW)));
        localStorage.setItem("fc_right_w", String(Math.round(rightW)));
        scheduleHiResTile();
      };
      el.addEventListener("pointerup", endDrag);
      el.addEventListener("pointercancel", endDrag);

      el.addEventListener("dblclick", () => {
        if (side === "left") leftW = DEF_L;
        else rightW = DEF_R;
        applyWidths(leftW, rightW);
        localStorage.setItem("fc_left_w", String(leftW));
        localStorage.setItem("fc_right_w", String(rightW));
        scheduleHiResTile();
      });
    }

    wireSplitter($("split-left"), "left");
    wireSplitter($("split-right"), "right");
  }

  // ---------- Boot ----------
  async function boot() {
    initSplitters();
    try {
      const st = await apiGet("/api/status");
      state.backends = st.backends || {};
      $("in-outdir").value = st.default_export_dir || "";
      const bInfo = state.backends[$("in-backend").value];
      $("backend-note").textContent = bInfo ? bInfo.note : "";
      updateJobUI(st.job, st.scanner);
      await refreshScans(null, false);
      connectSSE();
    } catch (err) {
      toast(`INIT ERR: ${err.message}`);
    }
  }

  boot();
})();
