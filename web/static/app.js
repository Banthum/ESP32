/* BMSmachine web dashboard - vanilla JS, no build step, no CDN
 * dependencies (this has to work fully offline/LAN-only, like a router's
 * config page - see web_server.py's module docstring). Polls /api/state
 * (a passthrough of dashboard-daemon's IPC message, see
 * docs/ipc_protocol.md) and re-renders whichever screen is active.
 *
 * The Energiestromen routing logic below (which edge lights up for a
 * given grid/battery/solar state) is a deliberate port of
 * dashboard-ui/src/screen_energyflow.c's screen_energyflow_update() -
 * keep the two in sync if that logic ever changes there. Same
 * "independently-updatable pieces duplicate small bits of logic rather
 * than reach across a process/language boundary" style already used
 * throughout this project (see CLAUDE.md).
 */
(() => {
"use strict";

const POLL_INTERVAL_MS = 3000;
const STALE_AFTER_MS = 30000; /* daemon pushes every 10s, so 3 missed pushes = stale */
const FLOW_DEADBAND_W = 15.0; /* matches screen_energyflow.c's FLOW_DEADBAND_W */
const DEFAULT_MIN_HOME_W = 100.0; /* fallback until state.min_home_w arrives - see docs/ipc_protocol.md */
const DOT_DURATION_MS = 1500; /* matches screen_energyflow.c's DOT_ANIM_DURATION_MS */

let state = null;
let lastStateAt = 0;

/* ---------- polling ---------- */

async function poll() {
    try {
        const res = await fetch("/api/state", {cache: "no-store"});
        const data = await res.json();
        if (data !== null) {
            state = data;
            lastStateAt = Date.now();
        }
    } catch (err) {
        /* Network hiccup or server not up yet - keep showing the last
         * known state (same "widgets show last known state" principle
         * as the C dashboard, see docs/ipc_protocol.md "Faalgedrag"). */
    }
    updateStatusDot();
    renderActiveScreen();
}

function updateStatusDot() {
    const dot = document.getElementById("status-dot");
    if (!state) {
        dot.className = "";
        return;
    }
    dot.className = (Date.now() - lastStateAt) < STALE_AFTER_MS ? "live" : "stale";
}

/* ---------- tab switching ---------- */

const SCREENS = ["energyflow", "price", "consumption", "solar", "battery", "savings", "settings"];

function renderActiveScreen() {
    const active = document.querySelector("nav button.active").dataset.screen;
    if (!state) return;
    if (active === "energyflow") renderEnergyflow();
    else if (active === "price") renderPrice();
    else if (active === "consumption") renderConsumption();
    else if (active === "solar") renderSolar();
    else if (active === "battery") renderBattery();
    else if (active === "savings") renderSavings();
}

document.querySelectorAll("nav button").forEach(btn => {
    btn.addEventListener("click", () => {
        document.querySelectorAll("nav button").forEach(b => b.classList.remove("active"));
        document.querySelectorAll(".screen").forEach(s => s.classList.remove("active"));
        btn.classList.add("active");
        document.getElementById("screen-" + btn.dataset.screen).classList.add("active");
        // Instellingen forms are populated here (tab-switch), NOT from the
        // 3s poll's renderActiveScreen() below - that runs continuously
        // while this tab stays active, and would stomp on whatever the
        // customer is mid-typing. See populateSettingsForms()'s own
        // comment for the full reasoning.
        if (btn.dataset.screen === "settings") populateSettingsForms();
        renderActiveScreen();
    });
});

/* ---------- generic canvas line chart ---------- */

/* series: [{data: Array<number|null>, color, dashed?}]
 * xCount: number of points along the x axis
 * yMin/yMax: fixed axis range
 * xLabelFn(index) -> string, only called for a handful of tick indices
 * nowFraction: 0..1 position for the vertical "now" cursor, or null */
function drawLineChart(canvas, {
    series, xCount, yMin, yMax, xTickIndices, xLabelFn, yUnit, nowFraction, rightAxis, xTickIndices2, xLabelFn2,
}) {
    const dpr = window.devicePixelRatio || 1;
    const cssW = canvas.clientWidth, cssH = canvas.clientHeight;
    canvas.width = cssW * dpr;
    canvas.height = cssH * dpr;
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);

    /* rightAxis (optional): {yMin, yMax, yUnit} for series marked
     * axis: "right" (e.g. battery SOC%) - needs its own scale since it
     * shares a chart with Wh-scaled series. Reuses the LEFT axis's
     * gridlines (drawn at left yMin/mid/yMax) rather than adding a second
     * set - the right-axis labels at those same 3 heights are just
     * rightAxis.yMin/mid/yMax, since the gridline fractions (0/0.5/1 of
     * plot height) are identical for both axes by construction. */
    // xTickIndices2/xLabelFn2 (optional, 2026-10-02): a second x-label row
    // below the first - only renderBattery() uses this, for its
    // "Vandaag"/"Morgen" day header under the per-day 00:00-24:00 time
    // axis (see screen_battery.c's exact same two-row layout). Needs more
    // bottom padding than the single-row default.
    const padL = 46, padR = rightAxis ? 40 : 10, padT = 10, padB = xTickIndices2 ? 40 : 24;
    const plotW = cssW - padL - padR, plotH = cssH - padT - padB;

    const xForIndex = i => padL + (plotW * i) / (xCount - 1);
    const yForVal = v => padT + plotH - ((v - yMin) / (yMax - yMin)) * plotH;
    const yForValRight = rightAxis
        ? v => padT + plotH - ((v - rightAxis.yMin) / (rightAxis.yMax - rightAxis.yMin)) * plotH
        : null;

    ctx.strokeStyle = "#262626";
    ctx.fillStyle = "#999999";
    ctx.font = "11px sans-serif";
    ctx.lineWidth = 1;

    /* horizontal gridlines + y labels at min/mid/max (left axis) */
    [yMin, (yMin + yMax) / 2, yMax].forEach(v => {
        const y = yForVal(v);
        ctx.beginPath();
        ctx.moveTo(padL, y);
        ctx.lineTo(padL + plotW, y);
        ctx.stroke();
        ctx.textAlign = "right";
        ctx.textBaseline = "middle";
        ctx.fillText(v.toFixed(v < 10 ? 1 : 0), padL - 6, y);
    });
    if (yUnit) {
        ctx.save();
        ctx.translate(12, padT + plotH / 2);
        ctx.rotate(-Math.PI / 2);
        ctx.textAlign = "center";
        ctx.fillText(yUnit, 0, 0);
        ctx.restore();
    }

    /* right-axis labels, at the same 3 gridline heights as the left axis */
    if (rightAxis) {
        [rightAxis.yMin, (rightAxis.yMin + rightAxis.yMax) / 2, rightAxis.yMax].forEach((v, idx) => {
            const leftV = [yMin, (yMin + yMax) / 2, yMax][idx];
            const y = yForVal(leftV);
            ctx.textAlign = "left";
            ctx.textBaseline = "middle";
            ctx.fillText(v.toFixed(v < 10 ? 1 : 0), padL + plotW + 6, y);
        });
        if (rightAxis.yUnit) {
            ctx.save();
            ctx.translate(cssW - 10, padT + plotH / 2);
            ctx.rotate(Math.PI / 2);
            ctx.textAlign = "center";
            ctx.fillText(rightAxis.yUnit, 0, 0);
            ctx.restore();
        }
    }

    /* x labels */
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    xTickIndices.forEach(i => {
        ctx.fillText(xLabelFn(i), xForIndex(i), padT + plotH + 6);
    });
    if (xTickIndices2) {
        xTickIndices2.forEach(i => {
            ctx.fillText(xLabelFn2(i), xForIndex(i), padT + plotH + 22);
        });
    }

    /* now cursor */
    if (nowFraction !== null && nowFraction >= 0 && nowFraction <= 1) {
        const x = padL + plotW * nowFraction;
        ctx.save();
        ctx.strokeStyle = "#2d6fe0";
        ctx.setLineDash([6, 4]);
        ctx.beginPath();
        ctx.moveTo(x, padT);
        ctx.lineTo(x, padT + plotH);
        ctx.stroke();
        ctx.restore();
    }

    /* series - break the path on null gaps, same as the C charts'
     * LV_CHART_POINT_NONE handling. Two render modes:
     * - "line" (default): a connected stroke, as before.
     * - "bar" (2026-10-04, renderSolar()'s verwacht-verbruik series):
     *   filled rectangles, one per `s.barCount` points (independent of
     *   `xCount` - e.g. 48 hourly bars drawn under a 192-kwartier line
     *   grid, screen_solar.c's exact same "fewer, wider bars for
     *   readability" choice, see that file's own comment on why 192 bars
     *   would be unreadably thin). Bar i spans the x-range
     *   [i/barCount, (i+1)/barCount) of the full plot width, so it still
     *   lines up under the right portion of the shared time axis without
     *   needing its own separate axis/xForIndex. */
    series.forEach(s => {
        const yForThisSeries = (s.axis === "right" && yForValRight) ? yForValRight : yForVal;
        if (s.type === "bar") {
            const n = s.barCount || xCount;
            const barW = plotW / n;
            ctx.fillStyle = s.color;
            for (let i = 0; i < n; i++) {
                const v = s.data[i];
                if (v === null || v === undefined) continue;
                const x = padL + barW * i;
                const y = yForThisSeries(v);
                const barPad = barW * 0.1;
                ctx.fillRect(x + barPad, y, barW - 2 * barPad, (padT + plotH) - y);
            }
            return;
        }
        ctx.strokeStyle = s.color;
        ctx.lineWidth = 2;
        ctx.setLineDash(s.dashed ? [4, 3] : []);
        ctx.beginPath();
        let drawing = false;
        for (let i = 0; i < xCount; i++) {
            const v = s.data[i];
            if (v === null || v === undefined) {
                drawing = false;
                continue;
            }
            const x = xForIndex(i), y = yForThisSeries(v);
            if (!drawing) {
                ctx.moveTo(x, y);
                drawing = true;
            } else {
                ctx.lineTo(x, y);
            }
        }
        ctx.stroke();
        ctx.setLineDash([]);
    });
}

function showEmpty(canvas, msg) {
    const ctx = canvas.getContext("2d");
    const dpr = window.devicePixelRatio || 1;
    canvas.width = canvas.clientWidth * dpr;
    canvas.height = canvas.clientHeight * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, canvas.clientWidth, canvas.clientHeight);
    ctx.fillStyle = "#999999";
    ctx.font = "13px sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(msg, canvas.clientWidth / 2, canvas.clientHeight / 2);
}

/* Rounds a chart's Y-axis to fit the actual data, instead of a fixed
 * range that can clip real values (2026-10-01, Jeroen: the Kosten chart's
 * old fixed 0-0.5 range was clipping real peak-hour prices around 0.57
 * after the all-in price formula; same fix applied to Verbruik, which had
 * the same fixed-range gap). Mirrors dashboard-ui's
 * compute_price_axis_range()/compute_axis_max_kwh() in screen_price.c/
 * screen_consumption.c - same step-rounding idea, kept in sync.
 * `pinMinAtZero`: if true, min never goes above 0 (but CAN go below 0 if
 * real data is negative, e.g. EPEX import prices) - false isn't used yet
 * but keeps this generic rather than price-specific. Returns null if none
 * of the arrays had any real numeric value (caller falls back to a
 * default range in that case). */
function roundedAxisRange(valueArrays, step, pinMinAtZero) {
    let lo = Infinity, hi = -Infinity;
    for (const arr of valueArrays) {
        if (!Array.isArray(arr)) continue;
        for (const v of arr) {
            if (typeof v !== "number") continue;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (!isFinite(lo) || !isFinite(hi)) return null;
    if (pinMinAtZero && lo > 0) lo = 0;
    const min = lo < 0 ? Math.floor(lo / step) * step : 0;
    let max = Math.ceil(hi / step) * step;
    if (max <= min) max = min + step;
    return {min, max};
}

/* ---------- Kosten ---------- */

function renderPrice() {
    const canvas = document.getElementById("price-chart");
    // Quarter-resolution, 2-daags (192-punts, vandaag+morgen, sinds
    // 2026-10-08 - was 96/today-only; Jeroen: "ik wil dat de
    // energiekostentabel voor twee dagen laat zien") -
    // price_curve_quarterly/export_price_curve_quarterly, not the plain
    // hourly price_curve/export_price_curve (those still exist in `state`
    // for price_now's derivation, just not plotted here anymore). Mirrors
    // screen_price.c's exact same switch, same 192-point vandaag+morgen
    // convention renderSolar()/renderBattery() already use below.
    const curve = state.price_curve_quarterly;
    const exportCurve = state.export_price_curve_quarterly;
    const hasPrice = Array.isArray(curve) && !curve.every(v => v === null);
    if (!hasPrice) {
        showEmpty(canvas, "Geen prijsdata");
        return;
    }
    const now = new Date();
    const nowQuarterOfPlan = (now.getHours() * 60 + now.getMinutes()) / 15;
    const gasVisible = state.gas_enabled && Array.isArray(state.gas_price_curve);
    document.getElementById("gas-legend").style.display = gasVisible ? "" : "none";

    const series = [{data: curve, color: getCss("--net")}];
    if (Array.isArray(exportCurve)) {
        series.push({data: exportCurve, color: getCss("--batt")});
    }
    // gas_price_curve stays hourly (24, today only - no "morgen"-fetch at
    // all, see price_client.py) - expanded here by repeating its 24 values
    // across both days (modulo 24) so it shares the chart's single xCount
    // with the other two (now 2-daagse kwartier-resolutie) series, same as
    // screen_price.c.
    if (gasVisible) {
        const gasQuarterly = [];
        for (let q = 0; q < 192; q++) gasQuarterly.push(state.gas_price_curve[Math.floor(q / 4) % 24]);
        series.push({data: gasQuarterly, color: getCss("--zon")});
    }

    // Gas isn't included in the range calc - it can have its own very
    // different unit (EUR/m3), same "don't let gas warp stroom's scale"
    // reasoning as screen_price.c's gas axis staying separate (SECONDARY_Y
    // there; this chart has no secondary axis at all, gas just shares the
    // same visual range here when in EUR/kWh mode, same as before).
    const range = roundedAxisRange([curve, exportCurve], 0.1, true) || {min: 0, max: 0.5};

    drawLineChart(canvas, {
        series,
        xCount: 192,
        yMin: range.min, yMax: range.max,
        // Zelfde 2-daagse as-indeling als renderSolar()/renderBattery()
        // (00:00/12:00/24:00 per dag + "Vandaag"/"Morgen"-koptekst).
        xTickIndices: [0, 48, 96, 144, 191],
        xLabelFn: i => BATTERY_DAY_AXIS_LABELS[i],
        xTickIndices2: [48, 144],
        xLabelFn2: i => i < 96 ? "Vandaag" : "Morgen",
        yUnit: "EUR/kWh",
        nowFraction: nowQuarterOfPlan / 192,
    });
}

/* ---------- Verbruik ---------- */

function renderConsumption() {
    const canvas = document.getElementById("consumption-chart");
    const expected = state.expected_consumption_curve;
    const actual = state.actual_consumption_curve;
    if (!Array.isArray(expected)) {
        showEmpty(canvas, "Onvoldoende data (nog geen 14 dagen geschiedenis)");
        return;
    }
    // IPC's expected_consumption_curve/actual_consumption_curve is kWh
    // PER KWARTIER (energy), displayed as average power (kW) instead
    // since 2026-10-05 (on request) - kW = kWh / 0.25h = kWh * 4, same
    // conversion screen_consumption.c's touchscreen twin applies and the
    // Zon-tab's hourly-consumption-bars comment below already explains.
    const toKw = arr => Array.isArray(arr) ? arr.map(v => typeof v === "number" ? v * 4 : v) : arr;
    const expectedKw = toKw(expected);
    const actualKw = toKw(actual);
    const now = new Date();
    const nowQuarter = (now.getHours() * 60 + now.getMinutes()) / 15;
    const range = roundedAxisRange([expectedKw, actualKw], 0.1, true) || {min: 0, max: 6};

    drawLineChart(canvas, {
        series: [
            {data: expectedKw, color: getCss("--dim")},
            {data: actualKw || [], color: getCss("--batt")},
        ],
        xCount: 96,
        yMin: range.min, yMax: range.max,
        xTickIndices: [0, 24, 48, 72, 95],
        xLabelFn: i => String(Math.floor(i / 4)).padStart(2, "0") + ":00",
        yUnit: "kW",
        nowFraction: nowQuarter / 96,
    });
}

/* ---------- Zon ---------- */

function renderSolar() {
    const canvas = document.getElementById("solar-chart");
    // 2-daags (vandaag+morgen), kwartier-resolutie (2026-10-04, Jeroen:
    // "ik wil dat het zonverwachting dashboard uitgebreid wordt naar twee
    // dagen") - was 96/today-only. Mirrors screen_solar.c's exact same
    // switch (N_QUARTERS = IPC_BATTERY_PLAN_LEN), same 192-point
    // vandaag+morgen convention renderBattery() already uses below.
    const curve = state.solar_forecast_curve_quarterly;
    const consumptionQuarterly = state.expected_consumption_curve_quarterly;
    const hasSolar = Array.isArray(curve) && !curve.every(v => v === null);
    const hasConsumption = Array.isArray(consumptionQuarterly) && !consumptionQuarterly.every(v => v === null);
    if (!hasSolar && !hasConsumption) {
        showEmpty(canvas, "Nog geen zonvoorspelling/verbruiksdata beschikbaar");
        return;
    }

    // Verwacht-verbruik als 48 uur-staven, niet 192 kwartier-staven
    // (Jeroen's expliciete keuze, 2026-10-04) - mirrors screen_solar.c's
    // own comment on why: 192 staven over deze breedte is een
    // dichtgesmeerde band, geen leesbare staven (al eerder ontdekt+gefixt
    // voor Scherm D, die toen van staven NAAR lijnen ging). Gemiddelde van
    // wat er binnen dat uur geldig is (nooit verzonnen als alle 4
    // kwartieren ontbreken), zelfde "beste beschikbare data"-conventie als
    // de rest van dit project.
    // Bron is kWh PER KWARTIER (energie), niet Watt - zelfde eenheid als
    // het al-bestaande expected_consumption_curve-veld (zie
    // docs/ipc_protocol.md). Live-gevonden fout (2026-10-04): dit werd
    // hier per ongeluk als Watt behandeld (/1000), wat echte 0,05-0,7
    // kWh/kwartier-waarden liet verdwijnen naar vrijwel 0 kW - op het
    // touchscreen leek dat "alle staven even (piepklein) groot", op de
    // website waren ze zo goed als onzichtbaar. energie * 4 = gemiddeld kW.
    const hourlyConsumptionKw = [];
    if (hasConsumption) {
        for (let h = 0; h < 48; h++) {
            let sum = 0, count = 0;
            for (let k = 0; k < 4; k++) {
                const v = consumptionQuarterly[h * 4 + k];
                if (typeof v !== "number") continue;
                sum += v;
                count++;
            }
            hourlyConsumptionKw.push(count === 0 ? null : (sum / count) * 4);
        }
    }

    // Eén gedeelde Y-as (kW) voor beide series - axis auto-scaled to
    // whatever either curve actually contains. Mirrors screen_solar.c's
    // compute_combined_axis_range_w(), EMS (touchscreen) leading.
    const range = roundedAxisRange([hasSolar ? curve : [], hourlyConsumptionKw], 0.5, true) || {min: 0, max: 5};
    const now = new Date();
    const nowQuarterOfPlan = (now.getHours() * 60 + now.getMinutes()) / 15;

    const series = [];
    if (hasConsumption) {
        series.push({data: hourlyConsumptionKw, type: "bar", barCount: 48, color: getCss("--dim")});
    }
    if (hasSolar) {
        series.push({data: curve, color: getCss("--zon")});
    }

    drawLineChart(canvas, {
        series,
        xCount: 192,
        yMin: range.min, yMax: range.max,
        // Zelfde 2-daagse as-indeling als renderBattery() hieronder
        // (00:00/12:00/24:00 per dag + "Vandaag"/"Morgen"-koptekst).
        xTickIndices: [0, 48, 96, 144, 191],
        xLabelFn: i => BATTERY_DAY_AXIS_LABELS[i],
        xTickIndices2: [48, 144],
        xLabelFn2: i => i < 96 ? "Vandaag" : "Morgen",
        yUnit: "kW",
        nowFraction: nowQuarterOfPlan / 192,
    });
}

/* ---------- Batterij ---------- */

// Fixed axis end/mid-cap labels for renderBattery()'s per-day time axis
// (2026-10-02) - index 0/96 = start of vandaag/morgen, 48/144 = midday,
// 191 = last kwartier of the whole 2-day span (labeled "24:00" as an
// end-of-chart cap, same idiom screen_common_add_time_axis_labels()
// already uses elsewhere, even though kwartier 191 is technically 23:45).
// Deliberately no separate index-95 "24:00" entry (removed 2026-10-05,
// live-caught: index 95 "vandaag 24:00" and index 96 "morgen 00:00" are
// only 1 kwartier - a few pixels - apart on a 192-point-wide chart, so
// their center-aligned text labels rendered as illegible overlapping
// garbage). Both labels mark the exact same instant (midnight) anyway -
// index 96's "00:00" alone is the correct, non-duplicated way to mark
// that boundary.
const BATTERY_DAY_AXIS_LABELS = {0: "00:00", 48: "12:00", 96: "00:00", 144: "12:00", 191: "24:00"};

function renderBattery() {
    document.getElementById("batt-soc").textContent =
        typeof state.battery_soc_pct === "number" ? state.battery_soc_pct + " %" : "-";
    document.getElementById("batt-power").textContent =
        typeof state.battery_power_w === "number" ? Math.round(state.battery_power_w) + " W" : "-";
    document.getElementById("batt-capacity").textContent =
        typeof state.battery_capacity_kwh === "number" ? state.battery_capacity_kwh.toFixed(2) + " kWh" : "-";

    const canvas = document.getElementById("battery-plan-chart");
    // IPC's battery_plan_* curves are Wh PER KWARTIER (energy), displayed
    // as average power (kW) instead since 2026-10-05 (on request) - kW =
    // Wh-per-kwartier / 0.25h / 1000 = Wh-per-kwartier / 250. Mirrors
    // screen_battery.c's WH_PER_KWARTIER_TO_W conversion exactly (that
    // screen divides by 1000 again at the axis-label layer; this does
    // both steps in one multiplier since there's no separate label-divisor
    // concept here).
    const toKw = arr => Array.isArray(arr) ? arr.map(v => typeof v === "number" ? v / 250 : v) : arr;
    const eigenZon = toKw(state.battery_plan_eigen_zon_wh);
    const actiefGestuurd = toKw(state.battery_plan_actief_gestuurd_wh);
    const verwachtVerbruik = toKw(state.battery_plan_verwacht_verbruik_wh);
    // Nieuw (2026-10-15) - hoeveel zelfconsumptie-ontladen actief
    // geblokkeerd is, zie dashboard/battery_scheduler.py's geblokkeerd_wh.
    const geblokkeerd = toKw(state.battery_plan_geblokkeerd_wh);
    const socForecast = state.battery_soc_forecast_pct;
    if (!Array.isArray(eigenZon)) {
        showEmpty(canvas, "Nog geen planning\n(batterij niet bereikbaar of net gestart)");
        return;
    }
    // Left (kW) axis scaled to the actual eigen_zon/actief_gestuurd/
    // verwacht_verbruik data, not to battery_capacity_kwh (2026-10-02,
    // Jeroen: "ik wil dat de batterijlading alleen de SOC lijn volgt, dus
    // niet langer dat SOC ook kWh laat zien, daardoor kan je beter de
    // zonverwachting en het verwachte verbruik zien" - these 3 series are
    // per-kwartier energy amounts, way smaller than a full battery's
    // capacity, so scaling to capacity squashed them flat). SOC stays a
    // pure percentage on its own rightAxis below - battery_capacity_kwh is
    // no longer read for this chart at all. Mirrors screen_battery.c's
    // exact same switch. Round step 0.2 (was 50 Wh - 50*4/1000 = 0.2 kW,
    // same resolution as screen_battery.c's POWER_AXIS_ROUND_STEP_W=200).
    const range =
        roundedAxisRange([eigenZon, actiefGestuurd, verwachtVerbruik, geblokkeerd], 0.2, true) || {min: 0, max: 4};
    const now = new Date();
    // today = 0-96, "morgen" = 96-192 (2026-10-02: switched from 48 hourly
    // buckets to 192 kwartier buckets, 2 days still - see
    // battery_scheduler.QUARTERS_PER_PLAN, mirrors screen_battery.c).
    const nowQuarterOfPlan = (now.getHours() * 60 + now.getMinutes()) / 15;

    drawLineChart(canvas, {
        series: [
            {data: eigenZon, color: getCss("--zon")},
            {data: actiefGestuurd, color: getCss("--net")},
            {data: verwachtVerbruik || [], color: getCss("--dim")},
            {data: geblokkeerd || [], color: getCss("--block")},
            {data: socForecast || [], color: getCss("--batt"), axis: "right"},
        ],
        xCount: eigenZon.length, // 192 (vandaag+morgen) per battery_scheduler.QUARTERS_PER_PLAN
        yMin: range.min, yMax: range.max,
        rightAxis: {yMin: 0, yMax: 100, yUnit: "SOC%"},
        // Row 1: an actual 00:00-24:00 time axis for EACH day separately
        // (2026-10-02, Jeroen: "de x as van de batterij is onoverzichtelijk,
        // ik wil dat je gewoon van 0:00 tot 23:59 voor beide dagen laat
        // zien" - a single continuous 48h axis with only "Vandaag"/"Morgen"
        // gave no time-of-day reference at all). Indices/labels hardcoded
        // (not computed from i) same as screen_battery.c's static
        // day_axis_marks - these are fixed axis end/mid-caps, not real data
        // points. Mirrors screen_battery.c's exact same two-row layout.
        xTickIndices: [0, 48, 96, 144, 191],
        xLabelFn: i => BATTERY_DAY_AXIS_LABELS[i],
        // Row 2: the pre-existing "Vandaag"/"Morgen" day header, one tick
        // centered under each day's own half of the chart (kwartier 48 =
        // midday today, kwartier 144 = midday tomorrow).
        xTickIndices2: [48, 144],
        xLabelFn2: i => i < 96 ? "Vandaag" : "Morgen",
        yUnit: "kW",
        nowFraction: nowQuarterOfPlan / eigenZon.length,
    });
}

/* ---------- Besparing ---------- */

function fmtEur(value) {
    return (typeof value === "number" ? value : 0).toFixed(2) + " EUR";
}

function renderSavings() {
    // Mirrors screen_savings.c's exact same 5-period x (batterij/ems/
    // totaal) grid - see dashboard/savings_tracker.py's module docstring
    // for what the two counters mean. IPC fields are always present
    // (COALESCE on the Python side), so no "nog geen data" empty state
    // needed here unlike the chart-based screens above.
    const periods = ["dag", "week", "maand", "jaar", "altijd"];
    for (const period of periods) {
        const batterij = state[`savings_${period}_batterij_eur`];
        const ems = state[`savings_${period}_ems_eur`];
        document.getElementById(`sav-${period}-batterij`).textContent = fmtEur(batterij);
        document.getElementById(`sav-${period}-ems`).textContent = fmtEur(ems);
        document.getElementById(`sav-${period}-totaal`).textContent =
            fmtEur((typeof batterij === "number" ? batterij : 0) + (typeof ems === "number" ? ems : 0));
    }
}

/* ---------- Instellingen ---------- */
/* Deliberately NOT wired into renderActiveScreen()'s poll-driven dispatch
 * (see the nav click handler above) - this tab has no live/animated data
 * like the chart tabs do, and re-populating form fields every 3s poll
 * tick would overwrite whatever the customer is mid-typing. Forms are
 * instead populated once per tab visit (nav click) and once more after a
 * successful save (to show the daemon's own freshly-restarted view of
 * what just got written, same "trust the source of truth, don't assume
 * the echo" principle the rest of this file already follows elsewhere).
 */

function populateSettingsForms() {
    if (!state || !state.settings) return;
    const s = state.settings;

    const solar = s.solar;
    const noPanels = !!(solar && solar.no_solar_panels);
    document.getElementById("solar-no-panels").checked = noPanels;
    document.getElementById("solar-fields").style.display = noPanels ? "none" : "";
    if (solar && !noPanels) {
        document.getElementById("solar-postcode").value = solar.postcode || "";
        document.getElementById("solar-huisnummer").value = solar.huisnummer || "";
        document.getElementById("solar-panel-count").value = solar.panel_count ?? "";
        document.getElementById("solar-panel-wp").value = solar.panel_wp ?? "";
        document.getElementById("solar-richting").value = solar.richting || "zuid";
        document.getElementById("solar-declination").value = solar.declination ?? "";
        document.getElementById("solar-azimuth").value = solar.azimuth ?? "";
        document.getElementById("solar-forecast-factor").value = solar.forecast_factor ?? "";
    }

    const batt = s.battery || {};
    document.getElementById("batt-capacity-input").value = batt.capacity_kwh ?? "";
    document.getElementById("batt-max-power").value = batt.max_power_w ?? "";
    document.getElementById("batt-active-control").checked = !!batt.active_control_enabled;
    document.getElementById("batt-min-soc").value = batt.min_soc_pct ?? "";
    document.getElementById("batt-max-soc").value = batt.max_soc_pct ?? "";
    document.getElementById("batt-energy-loss").value = batt.energy_loss_pct ?? "";

    const gas = s.gas || {};
    document.getElementById("gas-enabled").checked = !!gas.enabled;
    document.getElementById("gas-unit").value = gas.unit || "kwh";
}

document.getElementById("solar-no-panels").addEventListener("change", e => {
    document.getElementById("solar-fields").style.display = e.target.checked ? "none" : "";
});

function setSettingsStatus(id, ok, msg) {
    const el = document.getElementById(id);
    el.textContent = msg;
    el.className = "settings-status " + (ok ? "ok" : "err");
}

async function postSettings(path, body) {
    const res = await fetch(path, {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body),
    });
    return res.json();
}

document.getElementById("form-solar").addEventListener("submit", async e => {
    e.preventDefault();
    setSettingsStatus("status-solar", true, "Bezig met opslaan...");
    const noPanels = document.getElementById("solar-no-panels").checked;
    const body = noPanels ? {no_solar_panels: true} : {
        postcode: document.getElementById("solar-postcode").value,
        huisnummer: document.getElementById("solar-huisnummer").value,
        panel_count: Number(document.getElementById("solar-panel-count").value),
        panel_wp: Number(document.getElementById("solar-panel-wp").value),
        richting: document.getElementById("solar-richting").value,
        declination: Number(document.getElementById("solar-declination").value),
        azimuth: Number(document.getElementById("solar-azimuth").value),
        forecast_factor: Number(document.getElementById("solar-forecast-factor").value),
    };
    try {
        const result = await postSettings("/api/settings/solar", body);
        setSettingsStatus("status-solar", result.ok, result.ok ? "Opgeslagen" : result.error);
        if (result.ok) await poll();
    } catch (err) {
        setSettingsStatus("status-solar", false, "Netwerkfout: " + err);
    }
});

document.getElementById("form-battery").addEventListener("submit", async e => {
    e.preventDefault();
    setSettingsStatus("status-battery", true, "Bezig met opslaan...");
    const body = {
        capacity_kwh: Number(document.getElementById("batt-capacity-input").value),
        max_power_w: Number(document.getElementById("batt-max-power").value),
        active_control_enabled: document.getElementById("batt-active-control").checked,
        min_soc_pct: Number(document.getElementById("batt-min-soc").value),
        max_soc_pct: Number(document.getElementById("batt-max-soc").value),
        energy_loss_pct: Number(document.getElementById("batt-energy-loss").value),
    };
    try {
        const result = await postSettings("/api/settings/battery", body);
        setSettingsStatus("status-battery", result.ok, result.ok ? "Opgeslagen" : result.error);
        if (result.ok) await poll();
    } catch (err) {
        setSettingsStatus("status-battery", false, "Netwerkfout: " + err);
    }
});

document.getElementById("form-gas").addEventListener("submit", async e => {
    e.preventDefault();
    setSettingsStatus("status-gas", true, "Bezig met opslaan...");
    const body = {
        enabled: document.getElementById("gas-enabled").checked,
        unit: document.getElementById("gas-unit").value,
    };
    try {
        const result = await postSettings("/api/settings/gas", body);
        setSettingsStatus("status-gas", result.ok, result.ok ? "Opgeslagen" : result.error);
        if (result.ok) await poll();
    } catch (err) {
        setSettingsStatus("status-gas", false, "Netwerkfout: " + err);
    }
});

document.getElementById("form-savings-reset").addEventListener("submit", async e => {
    e.preventDefault();
    if (!confirm("Alle historische besparingsdata permanent wissen? Dit kan niet ongedaan worden gemaakt.")) return;
    setSettingsStatus("status-savings-reset", true, "Bezig met resetten...");
    try {
        const result = await postSettings("/api/settings/savings/reset", {});
        setSettingsStatus("status-savings-reset", result.ok, result.ok ? "Gereset" : result.error);
    } catch (err) {
        setSettingsStatus("status-savings-reset", false, "Netwerkfout: " + err);
    }
});

/* ---------- Energiestromen ---------- */
/* Loosely mirrors screen_energyflow.c's NET_X/ZON_X/etc, but NOT the
 * exact Y values/offset: the C layout is constrained to a 276px-tall
 * physical touchscreen content area (room for the Batterij name-label
 * before it clips - see that file's layout comment for the exact
 * numbers), capping how far ZON_Y/BATT_Y can spread from the Net-Home
 * line (58px each there). A browser has no such ceiling, so this stays
 * symmetric but spread further apart (90px each way, on request -
 * "langere verticale lijn"), viewBox grown to 320 tall to fit. */
const NET = [80, 150], HOME = [400, 150], ZON = [240, 60], BATT = [240, 240];
const NODE_R = 44;

function offsetTowards([x0, y0], [x1, y1], r) {
    const dx = x1 - x0, dy = y1 - y0, len = Math.hypot(dx, dy);
    return [x0 + (dx / len) * r, y0 + (dy / len) * r];
}

/* [start, end] for each of the 6 mesh edges - "start" is always the
 * first-named node, matching screen_energyflow_build()'s convention (see
 * its point-array-direction comment). */
const EDGES = {
    zonHome: [offsetTowards(ZON, HOME, NODE_R), offsetTowards(HOME, ZON, NODE_R)],
    battHome: [offsetTowards(BATT, HOME, NODE_R), offsetTowards(HOME, BATT, NODE_R)],
    netHome: [[NET[0] + NODE_R, NET[1]], [HOME[0] - NODE_R, HOME[1]]],
    zonNet: [offsetTowards(ZON, NET, NODE_R), offsetTowards(NET, ZON, NODE_R)],
    zonBatt: [offsetTowards(ZON, BATT, NODE_R), offsetTowards(BATT, ZON, NODE_R)],
    battNet: [offsetTowards(BATT, NET, NODE_R), offsetTowards(NET, BATT, NODE_R)],
};

let efBuilt = false;
let efEdgeEls = {}, efDotEls = {}, efDotState = {}; /* name -> {active, forward} */
let efRafHandle = null;

function buildEnergyflowSvg() {
    const svg = document.getElementById("energyflow-svg");
    svg.innerHTML = "";
    const ns = "http://www.w3.org/2000/svg";
    const mk = (tag, attrs) => {
        const el = document.createElementNS(ns, tag);
        for (const k in attrs) el.setAttribute(k, attrs[k]);
        return el;
    };

    for (const name in EDGES) {
        const [[x1, y1], [x2, y2]] = EDGES[name];
        const line = mk("line", {x1, y1, x2, y2, class: "ef-edge", stroke: "#5a5a5a"});
        svg.appendChild(line);
        efEdgeEls[name] = line;
    }

    const nodes = [
        {key: "net", pos: NET, label: "Net"},
        {key: "zon", pos: ZON, label: "Zon"},
        {key: "home", pos: HOME, label: "Home"},
        {key: "batt", pos: BATT, label: "Batterij"},
    ];
    for (const n of nodes) {
        const [cx, cy] = n.pos;
        const circle = mk("circle", {cx, cy, r: NODE_R, class: "ef-node-circle", stroke: "#5a5a5a", id: "ef-ring-" + n.key});
        svg.appendChild(circle);
        const val = mk("text", {x: cx, y: cy + 4, class: "ef-node-label", id: "ef-val-" + n.key});
        svg.appendChild(val);
        const name = mk("text", {x: cx, y: cy + NODE_R + 16, class: "ef-node-name"});
        name.textContent = n.label;
        svg.appendChild(name);
    }

    for (const name in EDGES) {
        const dot = mk("circle", {class: "ef-dot", r: 5, fill: "#5a5a5a", visibility: "hidden"});
        svg.appendChild(dot);
        efDotEls[name] = dot;
        efDotState[name] = {active: false, forward: true, startedAt: 0};
    }
    efBuilt = true;

    function tick(now) {
        for (const name in efDotState) {
            const st = efDotState[name];
            if (!st.active) continue;
            const t = ((now - st.startedAt) % DOT_DURATION_MS) / DOT_DURATION_MS;
            const [a, b] = EDGES[name];
            const [from, to] = st.forward ? [a, b] : [b, a];
            const x = from[0] + (to[0] - from[0]) * t;
            const y = from[1] + (to[1] - from[1]) * t;
            efDotEls[name].setAttribute("cx", x);
            efDotEls[name].setAttribute("cy", y);
        }
        efRafHandle = requestAnimationFrame(tick);
    }
    if (efRafHandle) cancelAnimationFrame(efRafHandle);
    efRafHandle = requestAnimationFrame(tick);
}

function setEdge(name, active, forward, color) {
    efEdgeEls[name].setAttribute("stroke", active ? color : "#5a5a5a");
    const st = efDotState[name];
    if (active && (!st.active || st.forward !== forward)) {
        st.startedAt = performance.now();
    }
    st.active = active;
    st.forward = forward;
    efDotEls[name].setAttribute("visibility", active ? "visible" : "hidden");
    if (active) efDotEls[name].setAttribute("fill", color);
}

function renderEnergyflow() {
    if (!efBuilt) buildEnergyflowSvg();
    const c = getComputedStyle(document.documentElement);
    const netColor = c.getPropertyValue("--net").trim();
    const zonColor = c.getPropertyValue("--zon").trim();
    const battColor = c.getPropertyValue("--batt").trim();
    const homeColor = c.getPropertyValue("--home").trim();

    /* --- Zon: same current-hour + 5-min-bucket interpolation as
     * screen_energyflow.c's screen_energyflow_update() --- */
    const now = new Date();
    const hour = now.getHours(), min = now.getMinutes();
    const curve = state.solar_forecast_curve;
    let zonForecastW = 0, zonActive = false;
    const zonValid = Array.isArray(curve) && curve[hour] !== null && curve[hour] !== undefined;
    if (zonValid) {
        zonForecastW = curve[hour] * 1000.0;
        if (hour + 1 < 24 && curve[hour + 1] !== null && curve[hour + 1] !== undefined) {
            const nextW = curve[hour + 1] * 1000.0;
            const frac = (Math.floor(min / 5) * 5) / 60;
            zonForecastW += (nextW - zonForecastW) * frac;
        }
        zonActive = zonForecastW > FLOW_DEADBAND_W;
        /* Label text set below, near the Home calc - it may get corrected
         * for the dynamic min_home_w floor (see that block's comment),
         * not just the raw forecast value. */
    } else {
        document.getElementById("ef-val-zon").textContent = "-";
    }
    setEdge("zonHome", zonActive, true, zonColor);
    document.getElementById("ef-ring-zon").setAttribute("stroke", zonActive ? zonColor : "#5a5a5a");

    /* --- Net --- */
    const gridValid = typeof state.grid_power_w === "number";
    let netDir = 0;
    if (gridValid) {
        if (state.grid_power_w > FLOW_DEADBAND_W) netDir = 1;
        else if (state.grid_power_w < -FLOW_DEADBAND_W) netDir = -1;
        const exportW = state.grid_power_w < 0 ? -state.grid_power_w : 0;
        const importW = state.grid_power_w > 0 ? state.grid_power_w : 0;
        document.getElementById("ef-val-net").innerHTML =
            `<tspan x="${NET[0]}" dy="-8">&lt;- ${Math.round(exportW)} W</tspan><tspan x="${NET[0]}" dy="18">-&gt; ${Math.round(importW)} W</tspan>`;
    } else {
        document.getElementById("ef-val-net").textContent = "-";
    }
    document.getElementById("ef-ring-net").setAttribute("stroke", netDir !== 0 ? netColor : "#5a5a5a");
    setEdge("netHome", netDir === 1, true, netColor);

    /* --- Batterij --- */
    const battValid = typeof state.battery_power_w === "number";
    let battDir = 0;
    if (battValid) {
        if (state.battery_power_w > FLOW_DEADBAND_W) battDir = 1;
        else if (state.battery_power_w < -FLOW_DEADBAND_W) battDir = -1;
        const chargeW = state.battery_power_w < 0 ? -state.battery_power_w : 0;
        const dischargeW = state.battery_power_w > 0 ? state.battery_power_w : 0;
        document.getElementById("ef-val-batt").innerHTML =
            `<tspan x="${BATT[0]}" dy="-8">v ${Math.round(chargeW)} W</tspan><tspan x="${BATT[0]}" dy="18">^ ${Math.round(dischargeW)} W</tspan>`;
    } else {
        document.getElementById("ef-val-batt").textContent = "-";
    }
    document.getElementById("ef-ring-batt").setAttribute("stroke", battDir !== 0 ? battColor : "#5a5a5a");
    setEdge("battHome", battDir === 1, true, battColor);

    /* --- Derived routing edges (see screen_energyflow.c's layout
     * comment: Home can only ever be a sink, never a source) --- */
    setEdge("zonNet", netDir === -1 && zonActive, true, netColor);
    setEdge("zonBatt", battDir === -1 && zonActive, true, battColor);
    let battNetRoute = 0;
    if (netDir === -1 && battDir === 1) battNetRoute = 1; /* Batterij -> Net */
    else if (netDir === 1 && battDir === -1) battNetRoute = -1; /* Net -> Batterij */
    setEdge("battNet", battNetRoute !== 0, battNetRoute > 0, netColor);

    /* --- Home (always a sink, permanent teal ring, see build()) --- */
    document.getElementById("ef-ring-home").setAttribute("stroke", homeColor);
    // zonValid is NOT required here (2026-10-01, live-caught: Home showed
    // "-" at night with a real 167W battery + 8W grid reading, because
    // solar_forecast_curve has no entry for late-night hours -
    // Forecast.Solar simply stops reporting after sunset). zonForecastW is
    // already 0 whenever zonValid is false (its declaration above), and a
    // missing forecast is overwhelmingly more likely to mean "genuinely
    // ~0 solar right now" than "a real nonzero value we're just missing",
    // so Home can and should still compute from grid+battery alone. The
    // Zon node's OWN displayed number is unaffected (only written below
    // when zonValid) - it still shows "-" with no forecast, a different,
    // still-useful signal from Home's best-effort 0-default.
    if (gridValid && battValid) {
        const rawHomeW = zonForecastW + state.grid_power_w + state.battery_power_w;
        let homeW = rawHomeW;
        let zonDisplayW = zonForecastW;
        // Dynamic Home floor (2026-10-01, was the fixed MIN_HOME_W
        // constant) - state.min_home_w comes from dashboard_daemon.py
        // (storage.overnight_baseline_load_w(), see
        // docs/ipc_protocol.md), DEFAULT_MIN_HOME_W is only a fallback
        // for a stale/pre-this-feature state object. A real household
        // never actually draws 0W (standby loads) - when the raw
        // zon+batterij+net sum comes in under that floor, show the floor
        // instead, and ADD the shortfall to the Zon number specifically
        // (not grid/batterij, which are real sensor readings) - the Zon
        // term must absorb the correction in the direction that keeps
        // zon+batterij+net == Home a real, consistent identity after the
        // floor kicks in, not two independently-set numbers that no
        // longer add up. zonForecastW is >=0 by construction (solar
        // generation), so zonDisplayW can only increase here - no
        // clamping needed.
        const minHomeW = typeof state.min_home_w === "number" ? state.min_home_w : DEFAULT_MIN_HOME_W;
        // Only apply the floor while zon is actually contributing
        // (2026-10-12, Jeroen) - it exists to catch *invisible* solar
        // self-consumption (generation covering load without ever
        // showing up on grid/batterij), which only the estimated zon
        // term can account for. With zonForecastW === 0 (no sun at all)
        // grid+batterij are direct measurements that already equal real
        // household load exactly; flooring them would show a fabricated
        // number instead of the real one.
        if (zonForecastW > 0 && rawHomeW < minHomeW) {
            const deficit = minHomeW - rawHomeW;
            homeW = minHomeW;
            zonDisplayW = zonForecastW + deficit;
        }
        document.getElementById("ef-val-home").textContent = Math.round(homeW) + " W";
        if (zonValid) {
            document.getElementById("ef-val-zon").textContent = Math.round(zonDisplayW) + " W";
        }
    } else {
        document.getElementById("ef-val-home").textContent = "-";
        if (zonValid) {
            // Can't apply the min_home_w correction without grid/batterij
            // data - fall back to the raw forecast, same as before this
            // correction existed.
            document.getElementById("ef-val-zon").textContent = Math.round(zonForecastW) + " W";
        }
    }
}

function getCss(varName) {
    return getComputedStyle(document.documentElement).getPropertyValue(varName).trim();
}

/* ---------- boot ---------- */

poll();
setInterval(poll, POLL_INTERVAL_MS);
window.addEventListener("resize", renderActiveScreen);
})();
