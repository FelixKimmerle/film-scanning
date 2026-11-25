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
    highlights: 0.0,
    blacks: 0.0,
    temp: 0.0,
    tint: 0.0,
    saturation: 1.0,
    chroma_nr: 0.55,
    luma_nr: 0.15,
    sharpen: 0.25,
    auto_wb: true,
    ir_sensitivity: 0.50,
    ir_dilate: -1,          // -1 = AUTO (3px at 3200 DPI)
    ir_auto_shift: true,
    ir_shift_x: 0,
    ir_shift_y: 0,
  };

  // ---------- Application State ----------
  const state = {
    scans: [],
    activeScan: null,       // scan metadata object
    strips: [],             // [{index, x0, x1, y0, y1, angle_deg, color}]
    frames: [],             // [{id, strip_index, cx, cy, width, height, angle_deg, inset_pct, develop}]
    selectedId: null,
    selectedIds: new Set(), // multi-selection in Crop mode for targeted E export
    activeStripIndex: 0,
    undoStack: [],
    redoStack: [],
    backends: {},
    lastCompletedJobId: null,
    lastLogKey: "",

    // Mode & Develop lab state
    uiMode: "crop",         // "crop" | "develop"
    filmBase: null,         // [r, g, b] linear 0..1 sampled from inter-frame gaps
    devIrPrev: true,
    devBefore: false,
    wbPickMode: false,
    devReqSeq: 0,
    devTimer: null,
    devImg: null,           // HTMLImageElement of current developed frame
    devImgKey: "",          // frameId + rot90 to auto-fit on frame/orientation change
    devImgFullRes: false,   // true when fetched at native full resolution (max_dim = 0)
    devNatW: 1,             // exact exported native pixel width
    devNatH: 1,             // exact exported native pixel height
    devZoom: 1.0,           // CSS px per native frame pixel (1.0 = exact 1:1)
    devPanX: 0.0,
    devPanY: 0.0,
    devDrag: null,
    devUpgradeTimer: null,
    lastIrStats: { shift_x: 0, shift_y: 0, dust_px: 0, auto_dilate: 3 },
    copiedDevelop: null,    // clipboard for Ctrl+C / Ctrl+V develop look across frames/selection
    devCache: new Map(),    // LRU cache of pre-rendered Develop preview images for instant arrow stepping
    devPrefetchInflight: new Set(),
    irPaintMode: false,     // manual spot/scratch heal brush in Develop mode
    irEraseMode: false,
    irBrushPx: 14,          // brush radius in native scan pixels
    devCursorSx: null,
    devCursorSy: null,
    devStroke: null,        // active paint stroke { dabs: [{u,v,r,erase}], erase, beforeSnap }

    // Roll archive & event tagging state
    startIndex: 1,
    reverseOrder: false,
    knownEvents: [],        // existing event slugs discovered under <out_dir>/events/

    // View controls
    invertPreview: false,
    showIrMask: false,
    showIrRaw: false,
    irSens: 0.5,
    insetPct: 0.0,
    skipEmpty: true,
    drawMode: false,
    selectMode: false,

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
    setSingleSelection(state.selectedId);
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

  function setSingleSelection(id) {
    state.selectedId = id || null;
    state.selectedIds = new Set(id ? [id] : []);
  }

  function pruneSelection() {
    const valid = new Set(state.frames.map((f) => f.id));
    for (const id of Array.from(state.selectedIds)) {
      if (!valid.has(id)) state.selectedIds.delete(id);
    }
    if (state.selectedId && !valid.has(state.selectedId)) {
      state.selectedId = Array.from(state.selectedIds)[0] || state.frames[0]?.id || null;
    }
    if (state.selectedId && !state.selectedIds.has(state.selectedId)) {
      state.selectedIds.add(state.selectedId);
    }
  }

  function syncExportButtons() {
    const btnMain = $("btn-export");
    const btnTop = $("btn-export-top");
    if (!btnMain || !btnTop) return;
    if (state.uiMode === "develop") {
      const f = getSelectedFrame();
      const idx = f ? state.frames.indexOf(f) : -1;
      const numTxt = idx >= 0 ? `#${formatFrameNum(getFrameExportNum(idx))}` : "FRAME";
      btnMain.innerHTML = `EXPORT ${numTxt} <span class="key">E</span>`;
      btnTop.innerHTML = `EXP ${numTxt} <span class="key">E</span>`;
      return;
    }
    const cnt = state.selectedIds.size;
    if (cnt === 1 && state.selectedId) {
      const idx = state.frames.findIndex((f) => f.id === state.selectedId);
      const numTxt = idx >= 0 ? `#${formatFrameNum(getFrameExportNum(idx))}` : "1 SEL";
      btnMain.innerHTML = `EXPORT ${numTxt} <span class="key">E</span>`;
      btnTop.innerHTML = `EXP ${numTxt} <span class="key">E</span>`;
    } else if (cnt > 1) {
      btnMain.innerHTML = `EXPORT ${cnt} SEL <span class="key">E</span>`;
      btnTop.innerHTML = `EXP ${cnt} SEL <span class="key">E</span>`;
    } else {
      btnMain.innerHTML = `EXPORT SEL <span class="key">E</span>`;
      btnTop.innerHTML = `EXP SEL <span class="key">E</span>`;
    }
  }

  function stepFrame(delta, extendSelection = false) {
    if (!state.frames.length) return;
    let idx = state.frames.findIndex((f) => f.id === state.selectedId);
    if (idx < 0) idx = delta > 0 ? 0 : state.frames.length - 1;
    else idx = (idx + delta + state.frames.length) % state.frames.length;
    const f = state.frames[idx];
    if (extendSelection && state.uiMode === "crop") {
      if (state.selectedId) state.selectedIds.add(state.selectedId);
      state.selectedId = f.id;
      state.selectedIds.add(f.id);
    } else {
      setSingleSelection(f.id);
    }
    state.activeStripIndex = f.strip_index || 0;
    syncUI();
    if (state.uiMode === "develop") {
      syncDevelopUI();
      scheduleDevelopPreview(10);
    } else {
      zoomToFrame(f);
    }
  }

  function selectStrip(stripIdx) {
    const targets = state.frames.filter((f) => (f.strip_index || 0) === stripIdx);
    if (!targets.length) {
      toast(`STRIP ${stripIdx + 1} HAS NO FRAMES`);
      return;
    }
    state.selectedIds = new Set(targets.map((f) => f.id));
    state.selectedId = targets[0].id;
    state.activeStripIndex = stripIdx;
    syncUI();
    render();
    toast(`SELECTED STRIP ${stripIdx + 1} (${targets.length} FRAMES · ALT+SHIFT+HJKL TO MOVE)`);
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
    setSingleSelection(null);
    state.undoStack = [];
    state.redoStack = [];
    state.frames = [];
    state.strips = [];
    state.filmBase = null;
    state.img = null;
    state.maskImg = null;
    state.hiResTile = null;
    state.hiResMask = null;
    state.devImg = null;
    state.devImgKey = "";
    state.devCache.clear();
    state.devPrefetchInflight.clear();
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
    setSingleSelection(null);
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
    if ($("dev-ir-mask")) $("dev-ir-mask").disabled = !scan.has_ir;
    if ($("dev-ir-prev")) $("dev-ir-prev").disabled = !scan.has_ir;
    if (!scan.has_ir) {
      state.showIrMask = false;
      state.showIrRaw = false;
      setToggle("tog-irmask", false);
      setToggle("tog-irraw", false);
      if ($("dev-ir-mask")) setToggle("dev-ir-mask", false);
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
        if (typeof layout.start_index === "number" && layout.start_index >= 1) {
          state.startIndex = Math.round(layout.start_index);
          $("in-start-num").value = String(state.startIndex);
        } else {
          state.startIndex = 1;
          $("in-start-num").value = "1";
        }
        state.reverseOrder = Boolean(layout.reverse_order);
        setToggle("tog-rev-order", state.reverseOrder);
        if (typeof layout.event_link_mode === "string") {
          $("in-event-mode").value = layout.event_link_mode;
        }
        if (typeof layout.inset_pct === "number") {
          state.insetPct = layout.inset_pct;
          $("in-inset").value = String(state.insetPct);
          $("val-inset").textContent = `${state.insetPct.toFixed(1)}%`;
        }
        syncEventSuggestions();
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
      setSingleSelection(state.frames.length ? state.frames[0].id : null);
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
      start_index: state.startIndex,
      reverse_order: state.reverseOrder,
      event_link_mode: $("in-event-mode").value,
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

  function getFrameExportNum(idx) {
    const base = Math.max(1, parseInt(state.startIndex, 10) || 1);
    if (state.reverseOrder) {
      return base + Math.max(0, state.frames.length - 1 - idx);
    }
    return base + idx;
  }

  function formatFrameNum(num) {
    const maxNum = Math.max(1, state.startIndex) + Math.max(0, state.frames.length - 1);
    const pad = maxNum >= 100 ? 3 : 2;
    return String(num).padStart(pad, "0");
  }

  function syncEventSuggestions() {
    const dl = $("event-suggestions");
    if (!dl) return;
    const set = new Set(state.knownEvents || []);
    for (const f of state.frames) {
      if (!f.event) continue;
      for (const part of String(f.event).split(",")) {
        const t = part.trim();
        if (t) set.add(t);
      }
    }
    dl.innerHTML = "";
    for (const ev of Array.from(set).sort()) {
      const opt = document.createElement("option");
      opt.value = ev;
      dl.appendChild(opt);
    }
  }

  async function refreshArchiveEvents() {
    const dir = $("in-outdir").value.trim();
    if (!dir) return;
    try {
      const res = await apiGet(`/api/archive/events?out_dir=${encodeURIComponent(dir)}`);
      if (Array.isArray(res.events)) {
        state.knownEvents = res.events;
        syncEventSuggestions();
      }
    } catch (_) { /* ignore */ }
  }

  function deleteSelectedFrame(wholeSelection = false) {
    if (wholeSelection && state.selectedIds.size > 1) {
      const delSet = new Set(state.selectedIds);
      pushUndo();
      state.frames = state.frames.filter((f) => !delSet.has(f.id));
      setSingleSelection(state.frames[0]?.id || null);
      syncUI();
      render();
      toast(`DELETED ${delSet.size} SELECTED FRAMES (U TO UNDO)`);
      return;
    }
    if (!state.selectedId) return;
    const idx = state.frames.findIndex((f) => f.id === state.selectedId);
    if (idx < 0) return;
    pushUndo();
    state.frames.splice(idx, 1);
    setSingleSelection(state.frames[Math.min(idx, state.frames.length - 1)]?.id || null);
    syncUI();
    render();
  }

  function nudgeFrameOrStrip(dx, dy, dAngle, wholeSelection = false, stripIdxOverride = null) {
    if (!state.activeScan) return;
    if (stripIdxOverride !== null) {
      const targets = state.frames.filter((f) => (f.strip_index || 0) === stripIdxOverride);
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
    if (wholeSelection) {
      let targets = state.frames.filter((f) => state.selectedIds.has(f.id));
      if (targets.length <= 1) {
        const sel = getSelectedFrame();
        const sIdx = sel ? (sel.strip_index || 0) : state.activeStripIndex;
        targets = state.frames.filter((f) => (f.strip_index || 0) === sIdx);
      }
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

  function nudgeScreen(dsx, dsy, dAngle, wholeSelection = false, stripIdxOverride = null) {
    const [dx, dy] = screenDeltaToScan(dsx, dsy);
    nudgeFrameOrStrip(dx, dy, dAngle, wholeSelection, stripIdxOverride);
  }

  function rotateSelectedExport90(delta, wholeSelection = false) {
    const targets = (wholeSelection && state.selectedIds.size > 1)
      ? state.frames.filter((f) => state.selectedIds.has(f.id))
      : (getSelectedFrame() ? [getSelectedFrame()] : []);
    if (!targets.length) return;
    pushUndo();
    const cw = ((delta % 360) + 360) % 360 === 90;
    for (const f of targets) {
      f.rot90 = (((f.rot90 || 0) + delta) % 360 + 360) % 360;
      if (Array.isArray(f.ir_spots) && f.ir_spots.length) {
        f.ir_spots = f.ir_spots.map((sp) => ({
          ...sp,
          u: round4(cw ? (1.0 - sp.v) : sp.v),
          v: round4(cw ? sp.u : (1.0 - sp.u)),
        }));
      }
    }
    syncUI();
    render();
    if (state.uiMode === "develop") scheduleDevelopPreview(10);
    if (targets.length > 1) {
      toast(`ROTATED ${targets.length} FRAMES ${delta > 0 ? "+90" : "-90"} DEG`);
    } else {
      toast(`${targets[0].id.toUpperCase()} EXPORT ORIENTATION: ${targets[0].rot90} DEG`);
    }
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
  function round4(v) { return Math.round(v * 10000) / 10000; }

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

  const ROTATE_CURSOR =
    `url("data:image/svg+xml;utf8,` +
    `<svg xmlns='http://www.w3.org/2000/svg' width='22' height='22' viewBox='0 0 24 24'>` +
    `<path d='M17.5 7.5A7 7 0 0 0 6.5 7.5' fill='none' stroke='%23000' stroke-width='4' stroke-linecap='round'/>` +
    `<path d='M17.5 7.5A7 7 0 0 0 6.5 7.5' fill='none' stroke='%2300ffff' stroke-width='2' stroke-linecap='round'/>` +
    `<polygon points='19,3 20,9.5 13.5,8.5' fill='%2300ffff' stroke='%23000' stroke-width='1.2'/>` +
    `<polygon points='5,3 4,9.5 10.5,8.5' fill='%2300ffff' stroke='%23000' stroke-width='1.2'/>` +
    `</svg>") 11 11, alias`;

  function resizeCursorFor(handle, totalDeg) {
    const baseMap = { e: 0, se: 45, s: 90, sw: 135, w: 180, nw: 225, n: 270, ne: 315 };
    const ang = (((baseMap[handle] ?? 0) + totalDeg) % 180 + 180) % 180;
    const sector = Math.round(ang / 45) % 4;
    return ["ew-resize", "nwse-resize", "ns-resize", "nesw-resize"][sector];
  }

  function updateCanvasCursor(hit) {
    if (state.drag) {
      if (state.drag.type === "pan") { canvas.style.cursor = "grabbing"; return; }
      if (state.drag.type === "rotate") { canvas.style.cursor = ROTATE_CURSOR; return; }
      if (state.drag.type === "resize") {
        canvas.style.cursor = resizeCursorFor(state.drag.handle, state.viewRot + (state.drag.frame.angle_deg || 0));
        return;
      }
      if (state.drag.type === "move") { canvas.style.cursor = "move"; return; }
      if (state.drag.type === "draw" || state.drag.type === "select") { canvas.style.cursor = "crosshair"; return; }
    }
    if (state.spaceDown) { canvas.style.cursor = "grab"; return; }
    if (state.drawMode || state.selectMode) { canvas.style.cursor = "crosshair"; return; }
    if (!hit) { canvas.style.cursor = "default"; return; }
    if (hit.kind === "rotate") { canvas.style.cursor = ROTATE_CURSOR; return; }
    if (hit.kind === "resize") {
      canvas.style.cursor = resizeCursorFor(hit.handle, state.viewRot + (hit.frame.angle_deg || 0));
      return;
    }
    if (hit.kind === "body") { canvas.style.cursor = "move"; return; }
    canvas.style.cursor = "default";
  }

  function hitTest(screenX, screenY) {
    const [ix, iy] = screenToScan(screenX, screenY);
    const sel = getSelectedFrame();
    // 1. Check corners, full border edges, and outer corner rotation zones on selected frame
    if (sel) {
      const sc = getInsetScale(sel);
      const hw = sel.width * sc * 0.5;
      const hh = sel.height * sc * 0.5;
      const [lx, ly] = frameLocalPoint(sel, ix, iy);
      const slx = lx * state.zoom;
      const sly = ly * state.zoom;
      const shw = hw * state.zoom;
      const shh = hh * state.zoom;

      // 1a. Corner resize handles (8px screen radius)
      const corners = [
        ["nw", -shw, -shh],
        ["ne", shw, -shh],
        ["se", shw, shh],
        ["sw", -shw, shh],
      ];
      for (const [name, cx, cy] of corners) {
        if (Math.abs(slx - cx) <= 8 && Math.abs(sly - cy) <= 8) {
          return { kind: "resize", handle: name, frame: sel };
        }
      }

      // 1b. Drag ANYWHERE along the 4 borders (7px screen tolerance) to scale that edge
      const edgeTol = 7;
      if (Math.abs(sly + shh) <= edgeTol && slx >= -shw - 4 && slx <= shw + 4) {
        return { kind: "resize", handle: "n", frame: sel };
      }
      if (Math.abs(sly - shh) <= edgeTol && slx >= -shw - 4 && slx <= shw + 4) {
        return { kind: "resize", handle: "s", frame: sel };
      }
      if (Math.abs(slx + shw) <= edgeTol && sly >= -shh - 4 && sly <= shh + 4) {
        return { kind: "resize", handle: "w", frame: sel };
      }
      if (Math.abs(slx - shw) <= edgeTol && sly >= -shh - 4 && sly <= shh + 4) {
        return { kind: "resize", handle: "e", frame: sel };
      }

      // 1c. Top rotation knob OR outer corner rotation zone (8..22px just outside any of the 4 corners)
      if (Math.hypot(slx, sly - (-shh - 22)) <= 9) {
        return { kind: "rotate", frame: sel };
      }
      const isOutsideBox = (Math.abs(slx) > shw - 1) || (Math.abs(sly) > shh - 1);
      if (isOutsideBox) {
        for (const [, cx, cy] of corners) {
          if (Math.hypot(slx - cx, sly - cy) <= 22) {
            return { kind: "rotate", frame: sel };
          }
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
      const isPrimary = f.id === state.selectedId;
      const isMulti = state.selectedIds.has(f.id);
      const isSel = isPrimary || isMulti;
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

      // Corner badge with export frame number (+ export rotation hint + event indicator)
      const r90 = ((f.rot90 || 0) % 360 + 360) % 360;
      const expNum = getFrameExportNum(idx);
      const hasEv = Boolean(f.event && String(f.event).trim());
      const label = formatFrameNum(expNum) + (rotArrows[r90] || "");
      const bx = -iw * 0.5;
      const by = -ih * 0.5;
      const badgeW = label.length * 7 + (hasEv ? 14 : 8);
      ctx.fillStyle = "rgba(8, 8, 8, 0.85)";
      ctx.fillRect(bx, by, badgeW, 15);
      ctx.fillStyle = isSel ? "#00ffff" : stripColor;
      ctx.fillText(label, bx + 4, by + 2);
      if (hasEv) {
        ctx.fillStyle = "#00ff88";
        ctx.fillRect(bx + badgeW - 6, by + 5, 4, 4);
      }

      // Clean corner handles on primary selected frame (entire border is draggable to resize; outside corners rotate)
      if (isPrimary) {
        const hs = 6;
        const pts = [
          [-iw * 0.5, -ih * 0.5], [iw * 0.5, -ih * 0.5],
          [iw * 0.5, ih * 0.5], [-iw * 0.5, ih * 0.5],
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

    // 4. Active manual box drawing OR marquee selection box preview
    if (state.drag && (state.drag.type === "draw" || state.drag.type === "select")) {
      const d = state.drag;
      const p0 = scanToScreen(Math.min(d.x0, d.x1), Math.min(d.y0, d.y1));
      const p1 = scanToScreen(Math.max(d.x0, d.x1), Math.max(d.y0, d.y1));
      const sx0 = Math.min(p0[0], p1[0]), sy0 = Math.min(p0[1], p1[1]);
      const sx1 = Math.max(p0[0], p1[0]), sy1 = Math.max(p0[1], p1[1]);
      const isSelBox = d.type === "select";
      ctx.strokeStyle = isSelBox ? "#00ffff" : "#ffff00";
      ctx.lineWidth = 1.5;
      if (isSelBox) ctx.setLineDash([5, 3]);
      ctx.strokeRect(sx0, sy0, sx1 - sx0, sy1 - sy0);
      ctx.setLineDash([]);
      ctx.fillStyle = isSelBox ? "rgba(0,255,255,0.12)" : "rgba(255,255,0,0.12)";
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
  });

  canvas.addEventListener("pointerdown", (e) => {
    if (!state.activeScan) return;
    const rect = canvas.getBoundingClientRect();
    const sx = e.clientX - rect.left;
    const sy = e.clientY - rect.top;
    const [ix, iy] = screenToScan(sx, sy);

    canvas.setPointerCapture(e.pointerId);

    // Middle click, Right click (ParaView/3D style), or Space+Left click -> pan
    if (e.button === 1 || e.button === 2 || (e.button === 0 && state.spaceDown)) {
      state.drag = { type: "pan", startSx: sx, startSy: sy, origPanX: state.panX, origPanY: state.panY };
      vpWrap.classList.add("panning");
      updateCanvasCursor(null);
      return;
    }
    if (e.button !== 0) return;

    // Manual draw mode
    if (state.drawMode) {
      state.drag = { type: "draw", x0: ix, y0: iy, x1: ix, y1: iy };
      return;
    }

    // Marquee box selection mode
    if (state.selectMode) {
      state.drag = { type: "select", x0: ix, y0: iy, x1: ix, y1: iy, additive: e.shiftKey || e.ctrlKey || e.metaKey };
      return;
    }

    const hit = hitTest(sx, sy);
    if (!hit) {
      if (!e.ctrlKey && !e.metaKey) setSingleSelection(null);
      // Clicking empty canvas drags to pan
      state.drag = { type: "pan", startSx: sx, startSy: sy, origPanX: state.panX, origPanY: state.panY };
      vpWrap.classList.add("panning");
      syncUI();
      render();
      return;
    }

    // Ctrl/Meta+Click toggles frame membership in multi-selection
    if ((e.ctrlKey || e.metaKey) && hit.kind === "body") {
      if (state.selectedIds.has(hit.frame.id)) {
        state.selectedIds.delete(hit.frame.id);
        if (state.selectedId === hit.frame.id) {
          state.selectedId = Array.from(state.selectedIds)[0] || null;
        }
      } else {
        state.selectedIds.add(hit.frame.id);
        state.selectedId = hit.frame.id;
      }
      state.activeStripIndex = hit.frame.strip_index || 0;
      syncUI();
      render();
      return;
    }

    if (hit.kind === "body") {
      setSingleSelection(hit.frame.id);
    } else {
      state.selectedId = hit.frame.id;
      state.selectedIds.add(hit.frame.id);
    }
    state.activeStripIndex = hit.frame.strip_index || 0;
    const beforeSnap = captureSnapshot();

    if (hit.kind === "rotate") {
      state.drag = {
        type: "rotate",
        frame: hit.frame,
        origAngle: hit.frame.angle_deg || 0,
        startPointerAngle: Math.atan2(iy - hit.frame.cy, ix - hit.frame.cx) * (180 / Math.PI),
        beforeSnap,
      };
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

    if (!state.drag) {
      updateCanvasCursor(hitTest(sx, sy));
      return;
    }
    const d = state.drag;
    updateCanvasCursor(null);

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
      const curAngle = Math.atan2(iy - d.frame.cy, ix - d.frame.cx) * (180 / Math.PI);
      let delta = curAngle - d.startPointerAngle;
      if (delta > 180) delta -= 360;
      if (delta < -180) delta += 360;
      d.frame.angle_deg = round3(Math.max(-45, Math.min(45, d.origAngle + delta)));
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
    } else if (d.type === "draw" || d.type === "select") {
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
        setSingleSelection(nf.id);
        state.drawMode = false;
        setToggle("btn-draw-mode", false);
        vpWrap.classList.add("mode-select");
      }
    } else if (d.type === "select") {
      const minX = Math.min(d.x0, d.x1), maxX = Math.max(d.x0, d.x1);
      const minY = Math.min(d.y0, d.y1), maxY = Math.max(d.y0, d.y1);
      if (!d.additive) state.selectedIds.clear();
      let firstHit = null;
      for (const f of state.frames) {
        if (f.cx >= minX && f.cx <= maxX && f.cy >= minY && f.cy <= maxY) {
          state.selectedIds.add(f.id);
          if (!firstHit) firstHit = f.id;
        }
      }
      state.selectedId = firstHit || Array.from(state.selectedIds)[0] || null;
      if (state.selectedIds.size) {
        toast(`SELECTED ${state.selectedIds.size} FRAMES (PRESS E TO EXPORT)`);
      }
    } else if (d.beforeSnap && JSON.stringify(state.frames) !== d.beforeSnap.framesJson) {
      pushUndo(d.beforeSnap);
    }
    const wasPan = d.type === "pan";
    state.drag = null;
    vpWrap.classList.remove("panning");
    updateCanvasCursor(null);
    syncUI();
    render();
    if (wasPan) scheduleHiResTile();
  });

  // ---------- UI Sync ----------
  function updateHud() {
    $("hud-mode").textContent = state.drawMode ? "DRAW BOX" : (state.selectMode ? "BOX SEL" : "SELECT");
    const hdSuffix = state.hiResTile ? " · 1:1 HD" : "";
    const rotSuffix = state.viewRot ? ` · ROT ${state.viewRot}°` : "";
    $("hud-view").textContent = (state.showIrRaw
      ? "RAW IR"
      : (state.invertPreview ? `POS (${$("in-filmtype").value.toUpperCase()})` : "RAW")) + hdSuffix + rotSuffix;
    const selN = state.selectedIds.size;
    $("hud-count").textContent = selN > 1 ? `${selN}/${state.frames.length}` : String(state.frames.length);
    $("hud-zoom").textContent = `${Math.round(state.zoom * 100)}%`;
    const rotBtn = $("btn-view-rot");
    if (rotBtn) {
      rotBtn.innerHTML = state.viewRot
        ? `ROT ${state.viewRot}&deg; <span class="key">O</span>`
        : `ROT VIEW <span class="key">O</span>`;
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
      const sIdx = state.frames.indexOf(sel);
      const numStr = sIdx >= 0 ? `#${formatFrameNum(getFrameExportNum(sIdx))}` : sel.id.toUpperCase();
      const evPart = sel.event ? ` · ${sel.event}` : "";
      $("hud-sel").textContent = `${numStr} · ${wmm.toFixed(1)}x${hmm.toFixed(1)} mm${evPart}`;
    } else {
      $("hud-sel").textContent = "NONE";
    }
  }

  function syncEventInputs(f) {
    const val = (f && f.event) ? String(f.event) : "";
    const cIn = $("in-frame-event");
    const dIn = $("dev-frame-event");
    if (cIn && document.activeElement !== cIn) cIn.value = val;
    if (dIn && document.activeElement !== dIn) dIn.value = val;
    if (cIn) cIn.disabled = !f;
    if (dIn) dIn.disabled = !f;
  }

  function updateSelectedKV() {
    const f = getSelectedFrame();
    syncEventInputs(f);
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
    const fIdx = state.frames.indexOf(f);
    const numStr = fIdx >= 0 ? `#${formatFrameNum(getFrameExportNum(fIdx))}` : f.id.toUpperCase();
    $("kv-id").textContent = `${numStr} (STRIP ${(f.strip_index || 0) + 1})`;
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

    // Fine surgical step (~0.056 mm -> 7px @ 3200 DPI, 5px @ 2400 DPI, 2px @ 600 DPI); Shift = 4x coarse
    const stepPx = state.activeScan ? Math.max(1, Math.round(state.activeScan.dpi / 450)) : 2;
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
          <button data-act="ccw" title="Tilt strip -0.1 deg">[</button>
          <button data-act="cw" title="Tilt strip +0.1 deg">]</button>
        </div>
      `;
      card.addEventListener("click", () => { state.activeStripIndex = idx; syncStripsUI(); });
      card.querySelectorAll("button").forEach((btn) => {
        btn.addEventListener("click", (ev) => {
          ev.stopPropagation();
          state.activeStripIndex = idx;
          const mult = ev.shiftKey ? 4 : 1;
          const act = btn.dataset.act;
          if (act === "l") nudgeScreen(-stepPx * mult, 0, 0, true, idx);
          if (act === "r") nudgeScreen(stepPx * mult, 0, 0, true, idx);
          if (act === "u") nudgeScreen(0, -stepPx * mult, 0, true, idx);
          if (act === "d") nudgeScreen(0, stepPx * mult, 0, true, idx);
          if (act === "ccw") nudgeFrameOrStrip(0, 0, -0.1 * mult, true, idx);
          if (act === "cw") nudgeFrameOrStrip(0, 0, 0.1 * mult, true, idx);
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
      const isPrimary = f.id === state.selectedId;
      const isMulti = state.selectedIds.has(f.id);
      row.className = "frame-row" + (isPrimary ? " selected" : (isMulti ? " multi-selected" : ""));
      const wmm = ((f.width / dpi) * 25.4).toFixed(0);
      const hmm = ((f.height / dpi) * 25.4).toFixed(0);
      const expNum = getFrameExportNum(idx);
      const evTxt = (f.event || "").trim();
      const midHtml = evTxt
        ? `<span class="frame-ev" title="${escapeHtml(evTxt)}">${escapeHtml(evTxt)}</span>`
        : `<span class="dim frame-meta">${wmm}x${hmm}mm</span>`;
      row.innerHTML = `
        <span class="sw" style="--c:${col};margin:0;"></span>
        <span>#${formatFrameNum(expNum)}</span>
        ${midHtml}
        <span>${(f.angle_deg || 0).toFixed(1)}&deg;</span>
      `;
      row.addEventListener("click", (ev) => {
        if (ev.shiftKey && state.selectedId) {
          const a = state.frames.findIndex((x) => x.id === state.selectedId);
          if (a >= 0) {
            const lo = Math.min(a, idx), hi = Math.max(a, idx);
            if (!ev.ctrlKey && !ev.metaKey) state.selectedIds.clear();
            for (let k = lo; k <= hi; k++) state.selectedIds.add(state.frames[k].id);
            state.selectedId = f.id;
          }
        } else if (ev.ctrlKey || ev.metaKey) {
          if (state.selectedIds.has(f.id)) {
            state.selectedIds.delete(f.id);
            if (state.selectedId === f.id) state.selectedId = Array.from(state.selectedIds)[0] || null;
          } else {
            state.selectedIds.add(f.id);
            state.selectedId = f.id;
          }
        } else {
          setSingleSelection(f.id);
        }
        state.activeStripIndex = f.strip_index || 0;
        syncUI();
        render();
      });
      row.addEventListener("dblclick", () => {
        setSingleSelection(f.id);
        state.activeStripIndex = f.strip_index || 0;
        syncUI();
        zoomToFrame(f);
      });
      list.appendChild(row);
    });
  }

  function syncUI() {
    pruneSelection();
    updateSelectedKV();
    syncStripsUI();
    syncFrameListUI();
    syncExportButtons();
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

  // ---------- Export Action (scope: "selected" = current frame in Develop / selection in Crop; "all" = full roll) ----------
  async function runExport(scope = "selected") {
    if (!state.activeScan || !state.frames.length) {
      toast("NO FRAMES TO EXPORT");
      return;
    }

    // Build list of frames to export, ALWAYS tagging each with its true roll export_index!
    let targetFrames = [];
    if (scope === "all") {
      targetFrames = state.frames.map((f, i) => ({ ...f, export_index: getFrameExportNum(i) }));
    } else if (state.uiMode === "develop") {
      const f = getSelectedFrame();
      if (!f) {
        toast("NO ACTIVE FRAME TO EXPORT");
        return;
      }
      const origIdx = state.frames.indexOf(f);
      targetFrames = [{ ...f, export_index: getFrameExportNum(origIdx) }];
    } else {
      // Crop mode: export multi-selection (or single selected frame)
      state.frames.forEach((f, i) => {
        if (state.selectedIds.has(f.id)) {
          targetFrames.push({ ...f, export_index: getFrameExportNum(i) });
        }
      });
      if (!targetFrames.length) {
        toast("SELECT FRAME(S) FIRST — OR PRESS SHIFT+E TO EXPORT ALL");
        return;
      }
    }

    // Sort chronologically by export_index so backend processes & reports in clean roll order
    targetFrames.sort((a, b) => a.export_index - b.export_index);

    await saveLayout(true);
    const outBox = $("export-status");
    const outTxt = $("export-status-text");
    outBox.hidden = false;
    outBox.className = "callout info";
    outBox.querySelector(".tag").textContent = "RUN";
    outTxt.textContent = `EXPORTING ${targetFrames.length} FRAME${targetFrames.length > 1 ? "S" : ""} (16-BIT)...`;
    $("btn-export").disabled = true;
    $("btn-export-top").disabled = true;
    $("btn-export-all").disabled = true;
    $("btn-export-all-top").disabled = true;

    try {
      const fallbackStem = state.activeScan.name.replace(/\.(tif|tiff)$/i, "");
      const res = await apiPost("/api/export", {
        scan_name: state.activeScan.name,
        frames: targetFrames,
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
        start_index: state.startIndex,
        reverse_order: false, // explicit export_index already accounts for startIndex & reverseOrder!
        archive_layout: true,
        event_link_mode: $("in-event-mode").value,
      });
      const evCnt = Array.isArray(res.events_updated) ? res.events_updated.length : 0;
      const evMsg = evCnt ? ` · ${evCnt} EVENT${evCnt > 1 ? "S" : ""}` : "";
      const secStr = res.elapsed_ms >= 1000 ? `${(res.elapsed_ms / 1000).toFixed(2)}s` : `${res.elapsed_ms} ms`;
      outBox.className = "callout ok";
      outBox.querySelector(".tag").textContent = "OK";
      outTxt.textContent = `EXPORTED ${res.count} FRAME${res.count > 1 ? "S" : ""}${evMsg} IN ${secStr} -> ${res.out_dir}`;
      toast(`EXPORTED ${res.count} FRAME${res.count > 1 ? "S" : ""}${evMsg} IN ${secStr}`);
      refreshArchiveEvents();
    } catch (err) {
      outBox.className = "callout err";
      outBox.querySelector(".tag").textContent = "ERR";
      outTxt.textContent = err.message;
      toast(`EXPORT ERR: ${err.message}`);
    } finally {
      $("btn-export").disabled = false;
      $("btn-export-top").disabled = false;
      $("btn-export-all").disabled = false;
      $("btn-export-all-top").disabled = false;
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
  bindToggle("tog-rev-order", (v) => {
    state.reverseOrder = v;
    syncUI();
    render();
    if (state.uiMode === "develop") syncDevelopUI();
    scheduleAutoSave();
    toast(v ? "FRAME NUMBERING REVERSED" : "FRAME NUMBERING NORMAL");
  });

  $("in-start-num").addEventListener("input", () => {
    const val = Math.max(1, Math.min(999, parseInt($("in-start-num").value, 10) || 1));
    state.startIndex = val;
    syncUI();
    render();
    if (state.uiMode === "develop") syncDevelopUI();
    scheduleAutoSave();
  });
  $("in-event-mode").addEventListener("change", () => scheduleAutoSave());
  $("in-outdir").addEventListener("change", () => refreshArchiveEvents());
  $("in-camera").addEventListener("change", () => scheduleAutoSave());
  $("in-stock").addEventListener("change", () => scheduleAutoSave());

  // ---------- Event / Carousel Tagging ----------
  function bindEventInput(inputId) {
    const el = $(inputId);
    if (!el) return;
    let beforeSnap = null;
    el.addEventListener("focus", () => { beforeSnap = captureSnapshot(); });
    el.addEventListener("input", () => {
      const f = getSelectedFrame();
      if (!f) return;
      f.event = el.value.trim();
      const other = $(inputId === "in-frame-event" ? "dev-frame-event" : "in-frame-event");
      if (other) other.value = el.value;
      syncEventSuggestions();
      syncFrameListUI();
      updateHud();
      if (state.uiMode === "develop") syncDevelopUI();
      render();
      scheduleAutoSave();
    });
    el.addEventListener("change", () => {
      if (beforeSnap) {
        pushUndo(beforeSnap);
        beforeSnap = null;
      }
    });
  }
  bindEventInput("in-frame-event");
  bindEventInput("dev-frame-event");

  function tagEventScope(scope, inputId) {
    const f = getSelectedFrame();
    if (!f) {
      toast("SELECT A FRAME FIRST");
      return;
    }
    const val = ($(inputId)?.value || f.event || "").trim();
    const selIdx = state.frames.indexOf(f);
    if (selIdx < 0) return;
    const selNum = getFrameExportNum(selIdx);
    pushUndo();
    let cnt = 0;
    state.frames.forEach((item, i) => {
      if (scope === "strip") {
        if ((item.strip_index || 0) === (f.strip_index || 0)) {
          item.event = val;
          cnt++;
        }
      } else if (scope === "from") {
        if (getFrameExportNum(i) >= selNum) {
          item.event = val;
          cnt++;
        }
      }
    });
    syncEventSuggestions();
    syncUI();
    render();
    if (state.uiMode === "develop") syncDevelopUI();
    toast(val ? `TAGGED ${cnt} FRAMES -> ${val}` : `CLEARED EVENT ON ${cnt} FRAMES`);
  }

  function clearSelectedEvent() {
    const f = getSelectedFrame();
    if (!f || !f.event) return;
    pushUndo();
    f.event = "";
    syncEventSuggestions();
    syncUI();
    render();
    if (state.uiMode === "develop") syncDevelopUI();
    toast("CLEARED FRAME EVENT TAG");
  }

  $("btn-ev-from").addEventListener("click", () => tagEventScope("from", "in-frame-event"));
  $("btn-ev-strip").addEventListener("click", () => tagEventScope("strip", "in-frame-event"));
  $("btn-ev-clear").addEventListener("click", clearSelectedEvent);
  $("dev-ev-from").addEventListener("click", () => tagEventScope("from", "dev-frame-event"));
  $("dev-ev-strip").addEventListener("click", () => tagEventScope("strip", "dev-frame-event"));
  $("dev-ev-clear").addEventListener("click", clearSelectedEvent);

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
    $("dev-hi").value = String(d.highlights);
    $("val-dev-hi").textContent = (d.highlights >= 0 ? "+" : "") + Number(d.highlights).toFixed(2);
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

    // Sync IR Dust controls in Develop panel
    const hasIr = Boolean(state.activeScan && state.activeScan.has_ir);
    setToggle("dev-ir-mask", state.showIrMask && hasIr);
    const irSensVal = (typeof d.ir_sensitivity === "number" && d.ir_sensitivity >= 0) ? d.ir_sensitivity : state.irSens;
    $("dev-ir-sens").value = String(irSensVal);
    $("val-dev-ir-sens").textContent = Number(irSensVal).toFixed(2);
    const irDil = (typeof d.ir_dilate === "number") ? Math.round(d.ir_dilate) : -1;
    $("dev-ir-dilate").value = String(irDil);
    const autoDil = (state.lastIrStats && state.lastIrStats.auto_dilate) || 3;
    $("val-dev-ir-dilate").textContent = irDil < 0 ? `AUTO (${autoDil}px)` : `${irDil} px`;
    const autoSh = d.ir_auto_shift !== false;
    setToggle("dev-ir-autoshift", autoSh);
    $("dev-ir-dx").disabled = autoSh || !hasIr;
    $("dev-ir-dy").disabled = autoSh || !hasIr;
    if (autoSh) {
      $("dev-ir-dx").value = String(state.lastIrStats ? state.lastIrStats.shift_x : 0);
      $("dev-ir-dy").value = String(state.lastIrStats ? state.lastIrStats.shift_y : 0);
    } else {
      $("dev-ir-dx").value = String(d.ir_shift_x || 0);
      $("dev-ir-dy").value = String(d.ir_shift_y || 0);
    }
    const shiftEl = $("dev-ir-shift-readout");
    if (shiftEl) {
      if (!hasIr) {
        shiftEl.textContent = "NO IR";
      } else {
        const sx = autoSh ? (state.lastIrStats?.shift_x ?? 0) : (d.ir_shift_x || 0);
        const sy = autoSh ? (state.lastIrStats?.shift_y ?? 0) : (d.ir_shift_y || 0);
        const fmt = (v) => (v > 0 ? `+${v}` : String(v));
        const px = state.lastIrStats?.dust_px || 0;
        const pxTxt = px > 0 ? ` · ${px}px` : "";
        shiftEl.textContent = `${autoSh ? "AUTO" : "MAN"} (${fmt(sx)},${fmt(sy)})${pxTxt}`;
      }
    }

    updateBaseReadout();
    syncEventInputs(f);
    syncExportButtons();
    const idx = f ? state.frames.findIndex((x) => x.id === f.id) : -1;
    const numTxt = idx >= 0 ? `#${formatFrameNum(getFrameExportNum(idx))}` : "NONE";
    const evSuffix = (f && f.event) ? ` · ${f.event}` : "";
    $("dev-hud-frame").textContent = idx >= 0 ? `${numTxt} / ${state.frames.length}${evSuffix}` : "NONE";
    $("dev-hud-prof").textContent = state.devBefore ? "BEFORE (LEGACY PROOF)" : String(d.profile || "frontier").toUpperCase();
    const nSpots = (f && Array.isArray(f.ir_spots)) ? f.ir_spots.length : 0;
    setToggle("dev-ir-paint", state.irPaintMode);
    setToggle("dev-ir-erase", state.irEraseMode);
    $("dev-ir-brush").value = String(state.irBrushPx);
    $("val-dev-ir-brush").textContent = `${state.irBrushPx}px`;
    const clrBtn = $("dev-ir-clear-spots");
    if (clrBtn) {
      clrBtn.textContent = nSpots > 0 ? `CLR (${nSpots})` : "CLR";
      clrBtn.disabled = nSpots === 0;
    }
    $("dev-stage-wrap").classList.toggle("wb-pick", state.wbPickMode);
    $("dev-stage-wrap").classList.toggle("paint-mode", state.irPaintMode && !state.wbPickMode && !state.spaceDown);
  }

  function syncCropFromDevelop(f) {
    if (!f || !state.devImg) {
      if (f) {
        state.viewRot = (((f.rot90 || 0) % 360) + 360) % 360;
        zoomToFrame(f);
      } else {
        render();
      }
      return;
    }
    // 1:1 pixel-locked handoff: frame center & zoom stay at the exact same screen pixels!
    state.viewRot = (((f.rot90 || 0) % 360) + 360) % 360;
    const csx = state.devPanX + state.devNatW * 0.5 * state.devZoom;
    const csy = state.devPanY + state.devNatH * 0.5 * state.devZoom;
    const [c, s] = viewCS();
    const rx = f.cx * c - f.cy * s;
    const ry = f.cx * s + f.cy * c;
    state.zoom = Math.min(8.0, Math.max(0.02, state.devZoom));
    state.panX = csx - rx * state.zoom;
    state.panY = csy - ry * state.zoom;
    updateHud();
    render();
    scheduleHiResTile();
  }

  function trySyncDevelopFromCrop(f) {
    if (!f || !state.activeScan) return false;
    const rot90 = (((f.rot90 || 0) % 360) + 360) % 360;
    if (state.viewRot !== rot90) return false;
    const [nw, nh] = getFrameNativeDims(f);
    const wrap = $("dev-stage-wrap");
    const rect = wrap.getBoundingClientRect();
    // Only pixel-lock if the user was actually zoomed into this frame (not viewing the full 48-frame holder)
    const onScreenSpan = Math.max(nw, nh) * state.zoom;
    if (onScreenSpan < Math.min(rect.width, rect.height) * 0.35) return false;
    const [csx, csy] = scanToScreen(f.cx, f.cy);
    // Make sure the frame actually overlaps the viewport
    const hwS = nw * 0.5 * state.zoom, hhS = nh * 0.5 * state.zoom;
    if (csx + hwS < 0 || csx - hwS > rect.width || csy + hhS < 0 || csy - hhS > rect.height) return false;

    state.devNatW = nw;
    state.devNatH = nh;
    state.devZoom = Math.min(8.0, Math.max(0.08, state.zoom));
    state.devPanX = csx - nw * 0.5 * state.devZoom;
    state.devPanY = csy - nh * 0.5 * state.devZoom;
    return true;
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
    syncExportButtons();
    if (!isDev) {
      state.wbPickMode = false;
      state.irPaintMode = false;
      state.devStroke = null;
      state.devBefore = false;
      syncCropFromDevelop(getSelectedFrame());
      return;
    }
    if (!getSelectedFrame() && state.frames.length) {
      setSingleSelection(state.frames[0].id);
      syncUI();
    }
    const sel = getSelectedFrame();
    const lockedFromCrop = trySyncDevelopFromCrop(sel);
    syncDevelopUI();
    if (!state.filmBase && $("in-filmtype").value !== "slide") {
      sampleFilmBase(true);
    }
    clearTimeout(state.devTimer);
    clearTimeout(state.devUpgradeTimer);
    fetchDevelopPreview(false, lockedFromCrop);
  }

  function getFrameNativeDims(f) {
    if (!f) return [1, 1];
    const sc = getInsetScale(f);
    let w = Math.max(8, Math.round(f.width * sc));
    let h = Math.max(8, Math.round(f.height * sc));
    const r90 = (((f.rot90 || 0) % 360) + 360) % 360;
    if (r90 === 90 || r90 === 270) [w, h] = [h, w];
    return [w, h];
  }

  function devNeedsFullRes() {
    const longest = Math.max(state.devNatW, state.devNatH);
    if (longest <= 1050) return false;
    const dpr = window.devicePixelRatio || 1;
    const previewScale = 1050 / longest;
    return state.devZoom * dpr > previewScale * 1.15;
  }

  function renderDevelopCanvas() {
    if (state.uiMode !== "develop") return;
    const wrap = $("dev-stage-wrap");
    const dc = $("dev-canvas");
    if (!wrap || !dc) return;
    const rect = wrap.getBoundingClientRect();
    if (rect.width < 4 || rect.height < 4) return;
    const dpr = window.devicePixelRatio || 1;
    const cw = Math.max(1, Math.floor(rect.width * dpr));
    const ch = Math.max(1, Math.floor(rect.height * dpr));
    if (dc.width !== cw || dc.height !== ch) {
      dc.width = cw;
      dc.height = ch;
    }

    const dctx = dc.getContext("2d");
    dctx.save();
    dctx.scale(dpr, dpr);
    dctx.fillStyle = "#050505";
    dctx.fillRect(0, 0, rect.width, rect.height);

    if (state.devImg) {
      dctx.save();
      dctx.translate(state.devPanX, state.devPanY);
      dctx.scale(state.devZoom, state.devZoom);
      dctx.imageSmoothingEnabled = state.devZoom < 2.5;
      dctx.drawImage(state.devImg, 0, 0, state.devNatW, state.devNatH);
      dctx.strokeStyle = "#333333";
      dctx.lineWidth = 1 / state.devZoom;
      dctx.strokeRect(0, 0, state.devNatW, state.devNatH);

      // Live active paint stroke feedback (before C++ inpainting completes on pointerup)
      if (state.devStroke && state.devStroke.dabs.length) {
        const scaleMax = Math.max(state.devNatW, state.devNatH);
        dctx.fillStyle = state.devStroke.erase ? "rgba(0, 255, 136, 0.55)" : "rgba(255, 77, 255, 0.65)";
        for (const sp of state.devStroke.dabs) {
          dctx.beginPath();
          dctx.arc(sp.u * state.devNatW, sp.v * state.devNatH, sp.r * scaleMax, 0, Math.PI * 2);
          dctx.fill();
        }
      }
      dctx.restore();
    }

    // Live scaled brush cursor in Develop Heal/Erase mode
    if (state.irPaintMode && !state.wbPickMode && !state.spaceDown && !state.devDrag && state.devCursorSx !== null) {
      const rScr = Math.max(3, state.irBrushPx * state.devZoom);
      const isEr = state.devStroke ? state.devStroke.erase : state.irEraseMode;
      dctx.save();
      dctx.beginPath();
      dctx.arc(state.devCursorSx, state.devCursorSy, rScr, 0, Math.PI * 2);
      dctx.strokeStyle = "rgba(0, 0, 0, 0.85)";
      dctx.lineWidth = 3;
      dctx.stroke();
      dctx.strokeStyle = isEr ? "#ffa31a" : "#00ffff";
      dctx.lineWidth = 1.5;
      dctx.stroke();
      // Tiny center dot
      dctx.fillStyle = isEr ? "#ffa31a" : "#00ffff";
      dctx.fillRect(state.devCursorSx - 1, state.devCursorSy - 1, 2, 2);
      dctx.restore();
    }
    dctx.restore();

    const zEl = $("dev-hud-zoom");
    if (zEl) {
      const pct = Math.round(state.devZoom * 100);
      zEl.textContent = `${pct}%${state.devImgFullRes ? " · 1:1 HD" : ""}`;
    }
  }

  function fitDevelopView() {
    const f = getSelectedFrame();
    if (!f) return;
    const [nw, nh] = getFrameNativeDims(f);
    state.devNatW = nw;
    state.devNatH = nh;
    const wrap = $("dev-stage-wrap");
    const rect = wrap.getBoundingClientRect();
    const pad = 28;
    const z = Math.min(
      Math.max(40, rect.width - pad * 2) / nw,
      Math.max(40, rect.height - pad * 2) / nh
    );
    state.devZoom = Math.min(8.0, Math.max(0.05, z));
    state.devPanX = (rect.width - nw * state.devZoom) * 0.5;
    state.devPanY = (rect.height - nh * state.devZoom) * 0.5;
    renderDevelopCanvas();
  }

  function zoomDevelopAt(sx, sy, factor) {
    if (!state.devImg) return;
    const ix = (sx - state.devPanX) / state.devZoom;
    const iy = (sy - state.devPanY) / state.devZoom;
    state.devZoom = Math.min(8.0, Math.max(0.08, state.devZoom * factor));
    state.devPanX = sx - ix * state.devZoom;
    state.devPanY = sy - iy * state.devZoom;
    renderDevelopCanvas();
    if (devNeedsFullRes() && !state.devImgFullRes) {
      clearTimeout(state.devUpgradeTimer);
      state.devUpgradeTimer = setTimeout(() => fetchDevelopPreview(true), 120);
    }
  }

  function zoomDevelop100(sx = null, sy = null) {
    const f = getSelectedFrame();
    if (!f) return;
    const wrap = $("dev-stage-wrap");
    const rect = wrap.getBoundingClientRect();
    const cx = sx !== null ? sx : rect.width * 0.5;
    const cy = sy !== null ? sy : rect.height * 0.5;
    const ix = state.devImg ? (cx - state.devPanX) / state.devZoom : state.devNatW * 0.5;
    const iy = state.devImg ? (cy - state.devPanY) / state.devZoom : state.devNatH * 0.5;
    state.devZoom = 1.0;
    state.devPanX = rect.width * 0.5 - ix;
    state.devPanY = rect.height * 0.5 - iy;
    renderDevelopCanvas();
    if (!state.devImgFullRes && Math.max(state.devNatW, state.devNatH) > 1050) {
      fetchDevelopPreview(true);
    }
  }

  function scheduleDevelopPreview(delayMs = 70) {
    if (state.uiMode !== "develop" || !state.activeScan) return;
    clearTimeout(state.devTimer);
    clearTimeout(state.devUpgradeTimer);
    state.devTimer = setTimeout(() => fetchDevelopPreview(false), delayMs);
  }

  function buildDevPreviewReq(f, reqMaxDim) {
    const d = ensureFrameDevelop(f);
    const devPayload = state.devBefore ? { ...d, enabled: false } : { ...d, enabled: true };
    const hasIr = Boolean(state.activeScan && state.activeScan.has_ir);
    const spotsForReq = state.devBefore ? [] : (f.ir_spots || []);
    const framePayload = state.devBefore ? { ...f, ir_spots: [] } : f;
    const body = {
      frame: framePayload,
      develop: devPayload,
      film_base: state.filmBase,
      film_type: $("in-filmtype").value,
      apply_ir_clean: state.devIrPrev && !state.devBefore && hasIr,
      show_ir_mask: state.showIrMask && (hasIr || spotsForReq.length > 0),
      ir_sensitivity: (typeof d.ir_sensitivity === "number" && d.ir_sensitivity >= 0) ? d.ir_sensitivity : state.irSens,
      max_dim: reqMaxDim,
    };
    const cacheKey = JSON.stringify([
      state.activeScan?.name || "",
      f.id, f.cx, f.cy, f.width, f.height, f.angle_deg || 0, f.rot90 || 0, f.inset_pct ?? state.insetPct,
      spotsForReq, devPayload, state.filmBase, body.film_type, body.apply_ir_clean, body.show_ir_mask, body.ir_sensitivity, reqMaxDim,
    ]);
    return { body, cacheKey };
  }

  function putDevCache(cacheKey, entry) {
    if (state.devCache.has(cacheKey)) state.devCache.delete(cacheKey);
    state.devCache.set(cacheKey, entry);
    while (state.devCache.size > 10) {
      const oldest = state.devCache.keys().next().value;
      state.devCache.delete(oldest);
    }
  }

  async function requestDevImage(scanName, body) {
    const res = await fetch(`/api/scan/${encodeURIComponent(scanName)}/develop_preview`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const irStats = {
      shift_x: parseInt(res.headers.get("X-IR-Shift-X") || "0", 10) || 0,
      shift_y: parseInt(res.headers.get("X-IR-Shift-Y") || "0", 10) || 0,
      dust_px: parseInt(res.headers.get("X-IR-Dust-Px") || "0", 10) || 0,
      auto_dilate: parseInt(res.headers.get("X-IR-Auto-Dilate") || "3", 10) || 3,
    };
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const img = await new Promise((resolve, reject) => {
      const im = new Image();
      im.onload = () => { URL.revokeObjectURL(url); resolve(im); };
      im.onerror = (err) => { URL.revokeObjectURL(url); reject(err); };
      im.src = url;
    });
    return { img, irStats };
  }

  function prefetchDevelopNeighbors() {
    if (state.uiMode !== "develop" || !state.activeScan || state.frames.length < 2) return;
    const f = getSelectedFrame();
    if (!f) return;
    const idx = state.frames.indexOf(f);
    if (idx < 0) return;
    const scanName = state.activeScan.name;
    const neighborIndices = [
      (idx + 1) % state.frames.length,
      (idx - 1 + state.frames.length) % state.frames.length,
    ];
    for (const ni of neighborIndices) {
      if (ni === idx) continue;
      const nf = state.frames[ni];
      const { body, cacheKey } = buildDevPreviewReq(nf, 1050);
      if (state.devCache.has(cacheKey) || state.devPrefetchInflight.has(cacheKey)) continue;
      state.devPrefetchInflight.add(cacheKey);
      requestDevImage(scanName, body)
        .then((entry) => {
          if (state.activeScan && state.activeScan.name === scanName) {
            putDevCache(cacheKey, entry);
          }
        })
        .catch(() => {})
        .finally(() => state.devPrefetchInflight.delete(cacheKey));
    }
  }

  async function fetchDevelopPreview(forceFullRes = false, preserveView = false) {
    const f = getSelectedFrame();
    if (!state.activeScan || !f || state.uiMode !== "develop") return;
    const seq = ++state.devReqSeq;
    const key = `${state.activeScan.name}|${f.id}|${f.rot90 || 0}`;
    const isNewFrame = key !== state.devImgKey;
    const [nw, nh] = getFrameNativeDims(f);
    const shouldFit = isNewFrame && !preserveView;
    const wantFull = forceFullRes || (!shouldFit && devNeedsFullRes());
    const reqMaxDim = wantFull ? 0 : 1050;
    const { body, cacheKey } = buildDevPreviewReq(f, reqMaxDim);

    // Instant 0ms swap from prefetch cache if available!
    const cached = state.devCache.get(cacheKey);
    if (cached) {
      state.lastIrStats = cached.irStats;
      state.devImg = cached.img;
      state.devImgKey = key;
      state.devNatW = nw;
      state.devNatH = nh;
      state.devImgFullRes = wantFull || Math.max(cached.img.naturalWidth, cached.img.naturalHeight) >= Math.max(nw, nh) - 4;
      syncDevelopUI();
      if (shouldFit) fitDevelopView();
      else renderDevelopCanvas();
      $("dev-hud-state").textContent = `${cached.img.naturalWidth}x${cached.img.naturalHeight} · CACHED`;
      prefetchDevelopNeighbors();
      return;
    }

    // Keep previous image undistorted until the new frame finishes rendering (never stretch!)
    $("dev-hud-state").textContent = wantFull ? "RENDERING 1:1 HD..." : "RENDERING...";
    const t0 = performance.now();
    try {
      const entry = await requestDevImage(state.activeScan.name, body);
      if (seq !== state.devReqSeq) return;
      putDevCache(cacheKey, entry);
      state.lastIrStats = entry.irStats;
      state.devImg = entry.img;
      state.devImgKey = key;
      state.devNatW = nw;
      state.devNatH = nh;
      state.devImgFullRes = wantFull || Math.max(entry.img.naturalWidth, entry.img.naturalHeight) >= Math.max(nw, nh) - 4;
      syncDevelopUI();
      if (shouldFit) fitDevelopView();
      else renderDevelopCanvas();
      const dt = Math.round(performance.now() - t0);
      $("dev-hud-state").textContent = `${entry.img.naturalWidth}x${entry.img.naturalHeight} · ${dt} ms`;
      prefetchDevelopNeighbors();
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
  $("dev-fit").addEventListener("click", fitDevelopView);
  $("dev-100").addEventListener("click", () => zoomDevelop100());
  $("dev-sample-base").addEventListener("click", () => sampleFilmBase(false));

  // Smooth 60fps wheel zoom, click-drag pan, double-click 1:1 toggle, and WB Eyedropper on Develop canvas
  const devCanvas = $("dev-canvas");
  devCanvas.addEventListener("wheel", (e) => {
    if (state.uiMode !== "develop") return;
    e.preventDefault();
    const rect = devCanvas.getBoundingClientRect();
    const factor = e.deltaY < 0 ? 1.18 : 1 / 1.18;
    zoomDevelopAt(e.clientX - rect.left, e.clientY - rect.top, factor);
  }, { passive: false });

  devCanvas.addEventListener("contextmenu", (e) => e.preventDefault());

  devCanvas.addEventListener("dblclick", (e) => {
    if (state.uiMode !== "develop" || state.wbPickMode || state.irPaintMode || !state.devImg) return;
    const rect = devCanvas.getBoundingClientRect();
    if (Math.abs(state.devZoom - 1.0) < 0.08) {
      fitDevelopView();
    } else {
      zoomDevelop100(e.clientX - rect.left, e.clientY - rect.top);
    }
  });

  function devScreenToNorm(sx, sy) {
    const ix = (sx - state.devPanX) / state.devZoom;
    const iy = (sy - state.devPanY) / state.devZoom;
    return [ix / Math.max(1, state.devNatW), iy / Math.max(1, state.devNatH)];
  }

  function addStrokePoint(sx, sy) {
    if (!state.devStroke) return;
    const [u, v] = devScreenToNorm(sx, sy);
    const scaleMax = Math.max(1, state.devNatW, state.devNatH);
    const rNorm = round4(state.irBrushPx / scaleMax);
    const st = state.devStroke;
    if (st.lastU !== null) {
      const dxPx = (u - st.lastU) * state.devNatW;
      const dyPx = (v - st.lastV) * state.devNatH;
      const distPx = Math.hypot(dxPx, dyPx);
      const stepPx = Math.max(2.0, state.irBrushPx * 0.45);
      if (distPx < stepPx) return;
      const steps = Math.min(48, Math.floor(distPx / stepPx));
      for (let k = 1; k <= steps; k++) {
        const t = k / steps;
        const iu = st.lastU + (u - st.lastU) * t;
        const iv = st.lastV + (v - st.lastV) * t;
        if (iu >= -0.02 && iu <= 1.02 && iv >= -0.02 && iv <= 1.02) {
          st.dabs.push({ u: round4(iu), v: round4(iv), r: rNorm, erase: st.erase ? 1 : 0 });
        }
      }
      st.lastU = u;
      st.lastV = v;
    } else if (u >= -0.02 && u <= 1.02 && v >= -0.02 && v <= 1.02) {
      st.dabs.push({ u: round4(u), v: round4(v), r: rNorm, erase: st.erase ? 1 : 0 });
      st.lastU = u;
      st.lastV = v;
    }
  }

  devCanvas.addEventListener("pointerdown", (e) => {
    if (state.uiMode !== "develop" || !state.devImg) return;
    const rect = devCanvas.getBoundingClientRect();
    const sx = e.clientX - rect.left;
    const sy = e.clientY - rect.top;
    state.devCursorSx = sx;
    state.devCursorSy = sy;

    // WB Eyedropper pick (works accurately at any zoom/pan!)
    if (state.wbPickMode && e.button === 0) {
      const f = getSelectedFrame();
      if (!f) return;
      const dpr = window.devicePixelRatio || 1;
      const px = Math.max(2, Math.min(devCanvas.width - 3, Math.round(sx * dpr)));
      const py = Math.max(2, Math.min(devCanvas.height - 3, Math.round(sy * dpr)));
      const data = devCanvas.getContext("2d").getImageData(px - 2, py - 2, 5, 5).data;
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
      d.temp = Math.max(-1.0, Math.min(1.0, round3(d.temp + (b - r) * 1.6)));
      d.tint = Math.max(-1.0, Math.min(1.0, round3(d.tint + (g - 0.5 * (r + b)) * 1.8)));
      state.wbPickMode = false;
      syncDevelopUI();
      scheduleDevelopPreview(10);
      toast(`WB BALANCED (TEMP ${d.temp.toFixed(2)}, TINT ${d.tint.toFixed(2)})`);
      return;
    }

    // Manual IR Heal / Erase brush stroke (hold Space or middle-button to pan instead)
    if (state.irPaintMode && e.button === 0 && !state.spaceDown) {
      const f = getSelectedFrame();
      if (!f) return;
      devCanvas.setPointerCapture(e.pointerId);
      const erase = e.altKey ? !state.irEraseMode : state.irEraseMode;
      state.devStroke = { dabs: [], erase, lastU: null, lastV: null, beforeSnap: captureSnapshot() };
      addStrokePoint(sx, sy);
      renderDevelopCanvas();
      return;
    }

    if (e.button === 0 || e.button === 1 || e.button === 2) {
      devCanvas.setPointerCapture(e.pointerId);
      state.devDrag = { startSx: sx, startSy: sy, origPanX: state.devPanX, origPanY: state.devPanY };
      $("dev-stage-wrap").classList.add("panning");
    }
  });

  devCanvas.addEventListener("pointermove", (e) => {
    const rect = devCanvas.getBoundingClientRect();
    const sx = e.clientX - rect.left;
    const sy = e.clientY - rect.top;
    state.devCursorSx = sx;
    state.devCursorSy = sy;
    if (state.devStroke) {
      addStrokePoint(sx, sy);
      renderDevelopCanvas();
      return;
    }
    if (state.devDrag) {
      state.devPanX = state.devDrag.origPanX + (sx - state.devDrag.startSx);
      state.devPanY = state.devDrag.origPanY + (sy - state.devDrag.startSy);
      renderDevelopCanvas();
      return;
    }
    if (state.irPaintMode && state.uiMode === "develop") {
      renderDevelopCanvas();
    }
  });

  devCanvas.addEventListener("pointerleave", () => {
    state.devCursorSx = null;
    state.devCursorSy = null;
    if (state.irPaintMode && state.uiMode === "develop") renderDevelopCanvas();
  });

  const endDevPan = () => {
    if (state.devStroke) {
      const st = state.devStroke;
      state.devStroke = null;
      const f = getSelectedFrame();
      if (f && st.dabs.length) {
        if (!Array.isArray(f.ir_spots)) f.ir_spots = [];
        for (const dab of st.dabs) f.ir_spots.push(dab);
        pushUndo(st.beforeSnap);
        syncDevelopUI();
        scheduleDevelopPreview(10);
      } else {
        renderDevelopCanvas();
      }
      return;
    }
    if (!state.devDrag) return;
    state.devDrag = null;
    $("dev-stage-wrap").classList.remove("panning");
  };
  devCanvas.addEventListener("pointerup", endDevPan);
  devCanvas.addEventListener("pointercancel", endDevPan);

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
    if (v) state.irPaintMode = false;
    syncDevelopUI();
    if (v) toast("CLICK A NEUTRAL GRAY AREA IN THE PREVIEW TO BALANCE WB");
  });
  bindToggle("dev-ir-paint", (v) => {
    state.irPaintMode = v;
    if (v) state.wbPickMode = false;
    syncDevelopUI();
    renderDevelopCanvas();
    if (v) toast("HEAL BRUSH ON · DRAG TO HEAL (ALT ERASE · SPACE PAN · [ / ] SIZE)");
  });
  bindToggle("dev-ir-erase", (v) => {
    state.irEraseMode = v;
    if (v && !state.irPaintMode) state.irPaintMode = true;
    syncDevelopUI();
    renderDevelopCanvas();
  });
  $("dev-ir-brush").addEventListener("input", () => {
    state.irBrushPx = Math.max(3, Math.min(80, parseInt($("dev-ir-brush").value, 10) || 14));
    $("val-dev-ir-brush").textContent = `${state.irBrushPx}px`;
    renderDevelopCanvas();
  });
  $("dev-ir-clear-spots").addEventListener("click", () => {
    const f = getSelectedFrame();
    if (!f || !Array.isArray(f.ir_spots) || !f.ir_spots.length) return;
    pushUndo();
    f.ir_spots = [];
    syncDevelopUI();
    scheduleDevelopPreview(10);
    toast("CLEARED MANUAL HEAL SPOTS");
  });
  function setShowIrMask(v) {
    state.showIrMask = v;
    setToggle("tog-irmask", v);
    if ($("dev-ir-mask")) setToggle("dev-ir-mask", v);
    if (state.uiMode === "develop") {
      scheduleDevelopPreview(10);
    } else {
      loadPreviewImages();
    }
  }

  bindToggle("dev-ir-prev", (v) => {
    state.devIrPrev = v;
    scheduleDevelopPreview(10);
  });
  bindToggle("dev-ir-mask", (v) => setShowIrMask(v));
  bindToggle("dev-ir-autoshift", (v) => {
    const f = getSelectedFrame();
    if (!f) return;
    pushUndo();
    const d = ensureFrameDevelop(f);
    d.ir_auto_shift = v;
    if (!v && state.lastIrStats) {
      // Seed manual shift from last auto-detected shift so user can nudge +/-1 px from it
      d.ir_shift_x = state.lastIrStats.shift_x || 0;
      d.ir_shift_y = state.lastIrStats.shift_y || 0;
    }
    syncDevelopUI();
    scheduleDevelopPreview(10);
  });

  // Develop IR Sensitivity & Smoothing/Halo Radius sliders + Manual Shift X/Y
  {
    let sensSnap = null;
    const sEl = $("dev-ir-sens");
    sEl.addEventListener("pointerdown", () => { sensSnap = captureSnapshot(); });
    sEl.addEventListener("input", () => {
      const f = getSelectedFrame();
      if (!f) return;
      const val = parseFloat(sEl.value);
      ensureFrameDevelop(f).ir_sensitivity = val;
      $("val-dev-ir-sens").textContent = val.toFixed(2);
      scheduleDevelopPreview(75);
    });
    sEl.addEventListener("change", () => { if (sensSnap) { pushUndo(sensSnap); sensSnap = null; } });

    let dilSnap = null;
    const dEl = $("dev-ir-dilate");
    dEl.addEventListener("pointerdown", () => { dilSnap = captureSnapshot(); });
    dEl.addEventListener("input", () => {
      const f = getSelectedFrame();
      if (!f) return;
      const val = parseInt(dEl.value, 10);
      ensureFrameDevelop(f).ir_dilate = val;
      const autoDil = (state.lastIrStats && state.lastIrStats.auto_dilate) || 3;
      $("val-dev-ir-dilate").textContent = val < 0 ? `AUTO (${autoDil}px)` : `${val} px`;
      scheduleDevelopPreview(75);
    });
    dEl.addEventListener("change", () => { if (dilSnap) { pushUndo(dilSnap); dilSnap = null; } });

    for (const [id, prop] of [["dev-ir-dx", "ir_shift_x"], ["dev-ir-dy", "ir_shift_y"]]) {
      const el = $(id);
      el.addEventListener("change", () => {
        const f = getSelectedFrame();
        if (!f) return;
        pushUndo();
        const val = Math.max(-8, Math.min(8, parseInt(el.value, 10) || 0));
        el.value = String(val);
        ensureFrameDevelop(f)[prop] = val;
        syncDevelopUI();
        scheduleDevelopPreview(10);
      });
    }
  }

  bindToggle("dev-before", (v) => {
    state.devBefore = v;
    syncDevelopUI();
    scheduleDevelopPreview(10);
  });

  // Wire all 9 Film Lab sliders with single-step Undo on drag end + live preview
  const DEV_SLIDERS = [
    ["dev-exp", "exposure"],
    ["dev-con", "contrast"],
    ["dev-hi", "highlights"],
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

  function copyDevelopLook() {
    const src = getSelectedFrame();
    if (!src) {
      toast("SELECT A FRAME FIRST");
      return;
    }
    state.copiedDevelop = { ...ensureFrameDevelop(src) };
    const pBtn = $("dev-paste-look");
    if (pBtn) pBtn.disabled = false;
    const idx = state.frames.indexOf(src);
    const numTxt = idx >= 0 ? `#${formatFrameNum(getFrameExportNum(idx))}` : src.id.toUpperCase();
    toast(`COPIED DEVELOP LOOK FROM ${numTxt} (CTRL+V TO PASTE)`);
  }

  function pasteDevelopLook(forceSelection = false) {
    if (!state.copiedDevelop) {
      toast("COPY A DEVELOP LOOK FIRST (Y / CTRL+C)");
      return;
    }
    let targets = [];
    if (state.uiMode === "develop" && !forceSelection) {
      const f = getSelectedFrame();
      if (f) targets = [f];
    } else {
      targets = state.frames.filter((f) => state.selectedIds.has(f.id));
    }
    if (!targets.length) {
      toast("SELECT FRAME(S) TO PASTE LOOK");
      return;
    }
    pushUndo();
    for (const f of targets) {
      f.develop = { ...state.copiedDevelop };
    }
    if (state.uiMode === "develop") {
      syncDevelopUI();
      scheduleDevelopPreview(10);
    }
    toast(`PASTED DEVELOP LOOK TO ${targets.length} FRAME${targets.length > 1 ? "S" : ""}`);
  }

  $("dev-copy-look").addEventListener("click", copyDevelopLook);
  $("dev-paste-look").addEventListener("click", pasteDevelopLook);

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
  bindToggle("tog-irmask", (v) => setShowIrMask(v));
  bindToggle("tog-irraw", (v) => {
    state.showIrRaw = v;
    loadPreviewImages();
  });
  bindToggle("btn-select-mode", (v) => {
    state.selectMode = v;
    if (v && state.drawMode) {
      state.drawMode = false;
      setToggle("btn-draw-mode", false);
    }
    vpWrap.classList.toggle("mode-select", !(state.drawMode || state.selectMode));
    updateHud();
    if (v) toast("BOX SELECT: DRAG OVER FRAMES TO SELECT (SHIFT ADDS)");
  });
  bindToggle("btn-draw-mode", (v) => {
    state.drawMode = v;
    if (v && state.selectMode) {
      state.selectMode = false;
      setToggle("btn-select-mode", false);
    }
    vpWrap.classList.toggle("mode-select", !(state.drawMode || state.selectMode));
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
  $("btn-fit").addEventListener("click", () => {
    if (state.uiMode === "develop") fitDevelopView();
    else fitViewport();
  });
  $("btn-100").addEventListener("click", () => {
    if (state.uiMode === "develop") zoomDevelop100();
    else zoom100();
  });
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
  $("btn-export").addEventListener("click", () => runExport("selected"));
  $("btn-export-top").addEventListener("click", () => runExport("selected"));
  $("btn-export-all").addEventListener("click", () => runExport("all"));
  $("btn-export-all-top").addEventListener("click", () => runExport("all"));

  document.querySelectorAll("[data-nudge]").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      const step = state.activeScan ? Math.max(1, Math.round(state.activeScan.dpi / 450)) : 2;
      const mult = e.shiftKey ? 4 : 1;
      const dir = btn.dataset.nudge;
      if (dir === "left") nudgeScreen(-step * mult, 0, 0, e.altKey);
      if (dir === "right") nudgeScreen(step * mult, 0, 0, e.altKey);
      if (dir === "up") nudgeScreen(0, -step * mult, 0, e.altKey);
      if (dir === "down") nudgeScreen(0, step * mult, 0, e.altKey);
      if (dir === "ccw") nudgeFrameOrStrip(0, 0, -0.1 * mult, e.altKey);
      if (dir === "cw") nudgeFrameOrStrip(0, 0, 0.1 * mult, e.altKey);
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

  // ---------- Keyboard Shortcuts (Helix / Vim-Inspired, Unified Across Crop & Develop) ----------
  window.addEventListener("keydown", (e) => {
    if (e.target && ["INPUT", "SELECT", "TEXTAREA"].includes(e.target.tagName)) return;
    if (e.code === "Space") {
      state.spaceDown = true;
      vpWrap.classList.add("panning");
      $("dev-stage-wrap").classList.toggle("paint-mode", false);
      $("dev-stage-wrap").classList.add("panning");
      e.preventDefault();
      return;
    }

    const key = e.key.toLowerCase();
    const hasMod = e.ctrlKey || e.metaKey;

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

    // Standard Ctrl/Cmd shortcuts (work everywhere)
    if (hasMod && key === "a" && state.frames.length) {
      e.preventDefault();
      state.selectedIds = new Set(state.frames.map((f) => f.id));
      if (!state.selectedId) state.selectedId = state.frames[0].id;
      syncUI();
      render();
      toast(`SELECTED ALL ${state.frames.length} FRAMES`);
      return;
    }
    if (hasMod && key === "c") {
      const selTxt = window.getSelection ? window.getSelection().toString() : "";
      if (!selTxt && getSelectedFrame()) {
        e.preventDefault();
        copyDevelopLook();
        return;
      }
    }
    if (hasMod && key === "v") {
      if (state.copiedDevelop) {
        e.preventDefault();
        pasteDevelopLook(e.shiftKey);
        return;
      }
    }
    if (hasMod && key === "z" && e.shiftKey) { e.preventDefault(); redo(); return; }
    if (hasMod && key === "y") { e.preventDefault(); redo(); return; }
    if (hasMod && key === "z") { e.preventDefault(); undo(); return; }
    if (hasMod && key === "s") { e.preventDefault(); saveLayout(false); return; }
    if (hasMod) return;

    // Frame stepping (home-row h/j/k/l + Arrow aliases in BOTH Crop & Develop; Shift extends selection in Crop)
    if (!e.altKey) {
      if (e.key === "ArrowLeft" || e.key === "ArrowUp" || key === "h" || key === "k") {
        e.preventDefault();
        stepFrame(-1, e.shiftKey);
        return;
      }
      if (e.key === "ArrowRight" || e.key === "ArrowDown" || key === "l" || key === "j") {
        e.preventDefault();
        stepFrame(1, e.shiftKey);
        return;
      }
    }

    // Alt + h/j/k/l: translate/nudge frame border in CROP (Alt+Shift = whole selection)
    if (e.altKey && state.uiMode === "crop") {
      const step = state.activeScan ? Math.max(1, Math.round(state.activeScan.dpi / 450)) : 2;
      if (key === "h") { e.preventDefault(); nudgeScreen(-step, 0, 0, e.shiftKey); return; }
      if (key === "l") { e.preventDefault(); nudgeScreen(step, 0, 0, e.shiftKey); return; }
      if (key === "k") { e.preventDefault(); nudgeScreen(0, -step, 0, e.shiftKey); return; }
      if (key === "j") { e.preventDefault(); nudgeScreen(0, step, 0, e.shiftKey); return; }
    }

    // Helix Undo / Redo / Yank / Paste (work in BOTH Crop & Develop!)
    if (key === "u") {
      if (e.shiftKey) redo();
      else undo();
      return;
    }
    if (key === "y") { copyDevelopLook(); return; }
    if (key === "p") { pasteDevelopLook(e.shiftKey); return; }

    // Helix Strip & Whole-Roll Selection (work in BOTH modes!)
    if (key === "x" && state.frames.length) {
      const sel = getSelectedFrame();
      selectStrip(sel ? (sel.strip_index || 0) : state.activeStripIndex);
      return;
    }
    if (e.key === "%" && state.frames.length) {
      state.selectedIds = new Set(state.frames.map((f) => f.id));
      if (!state.selectedId) state.selectedId = state.frames[0].id;
      syncUI();
      render();
      toast(`SELECTED ALL ${state.frames.length} FRAMES`);
      return;
    }
    if (e.key === "," && state.selectedId) {
      setSingleSelection(state.selectedId);
      syncUI();
      render();
      toast("COLLAPSED SELECTION TO PRIMARY FRAME");
      return;
    }

    // Shared View & Export shortcuts
    if (key === "f") {
      if (state.uiMode === "develop") fitDevelopView();
      else fitViewport();
      return;
    }
    if (key === "0") {
      if (state.uiMode === "develop") zoomDevelop100();
      else zoom100();
      return;
    }
    if (key === "m" && !$("tog-irmask").disabled) { setShowIrMask(!state.showIrMask); return; }
    if (key === "e") { runExport(e.shiftKey ? "all" : "selected"); return; }

    // Q / W: tilt in Crop (Shift = whole selection, Alt = 5x); rotate +/-90 in Develop (Shift = whole selection)
    if (key === "q") {
      if (state.uiMode === "develop") rotateSelectedExport90(-90, e.shiftKey);
      else nudgeFrameOrStrip(0, 0, e.altKey ? -0.5 : -0.1, e.shiftKey);
      return;
    }
    if (key === "w") {
      if (state.uiMode === "develop") rotateSelectedExport90(90, e.shiftKey);
      else nudgeFrameOrStrip(0, 0, e.altKey ? 0.5 : 0.1, e.shiftKey);
      return;
    }

    // DEVELOP-only shortcuts
    if (state.uiMode === "develop") {
      if (key === "b") { $("dev-ir-paint").click(); return; }
      if (e.key === "[") {
        state.irBrushPx = Math.max(3, state.irBrushPx - (e.shiftKey ? 10 : 3));
        syncDevelopUI();
        renderDevelopCanvas();
        return;
      }
      if (e.key === "]") {
        state.irBrushPx = Math.min(80, state.irBrushPx + (e.shiftKey ? 10 : 3));
        syncDevelopUI();
        renderDevelopCanvas();
        return;
      }
      if (e.key === "Escape") {
        if (state.wbPickMode) state.wbPickMode = false;
        if (state.irPaintMode) state.irPaintMode = false;
        syncDevelopUI();
        renderDevelopCanvas();
      }
      return;
    }

    // CROP-only shortcuts: 1-9 select Strip 1-9; [ / ] tilt alias
    if (/^[1-9]$/.test(e.key) && !e.shiftKey && !e.altKey) {
      selectStrip(parseInt(e.key, 10) - 1);
      return;
    }
    if (e.key === "[") { nudgeFrameOrStrip(0, 0, e.altKey ? -0.5 : -0.1, e.shiftKey); return; }
    if (e.key === "]") { nudgeFrameOrStrip(0, 0, e.altKey ? 0.5 : 0.1, e.shiftKey); return; }

    if (key === "o") { rotateViewport(e.shiftKey ? -90 : 90); return; }
    if (key === "z") {
      const f = getSelectedFrame() || state.frames[0];
      if (f) { setSingleSelection(f.id); syncUI(); zoomToFrame(f); }
      return;
    }
    if (key === "i") { $("tog-invert").click(); return; }
    if (key === "r" && !$("tog-irraw").disabled) { $("tog-irraw").click(); return; }
    if (key === "a" && e.shiftKey) { runAutoDetect(); return; }
    if (key === "v") { $("btn-select-mode").click(); return; }
    if (key === "b") { $("btn-draw-mode").click(); return; }
    if (key === "g") { stampStripGrid(); return; }
    if (key === "d" || e.key === "Delete" || e.key === "Backspace") { deleteSelectedFrame(e.shiftKey); return; }
    if (e.key === "Escape") {
      if (state.drawMode) $("btn-draw-mode").click();
      if (state.selectMode) $("btn-select-mode").click();
      return;
    }
  });

  window.addEventListener("keyup", (e) => {
    if (e.code === "Space") {
      state.spaceDown = false;
      vpWrap.classList.remove("panning");
      $("dev-stage-wrap").classList.remove("panning");
      $("dev-stage-wrap").classList.toggle("paint-mode", state.irPaintMode && !state.wbPickMode);
      if (state.uiMode === "develop") renderDevelopCanvas();
    }
    if (e.key === "\\" && state.uiMode === "develop" && state.devBefore) {
      state.devBefore = false;
      syncDevelopUI();
      scheduleDevelopPreview(5);
    }
  });

  window.addEventListener("resize", () => {
    render();
    if (state.uiMode === "develop") renderDevelopCanvas();
  });

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
      if (state.uiMode === "develop") renderDevelopCanvas();
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
      state.knownEvents = Array.isArray(st.existing_events) ? st.existing_events : [];
      syncEventSuggestions();
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
