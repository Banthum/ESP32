"""Referentie voor de batterij-aansturing (src/ems/logic/battery_control.cpp).

Draait het ORIGINELE enforce/verify-blok uit de Pi's dashboard_daemon.py (niet
een overgetypte kopie) op gesimuleerde en echte scenario's, en schrijft per
cyclus de invoer + de uitkomst van de Pi-code naar
test/test_ems_logic/control_vectors.h. De board-test (pio test -e ems_test)
voert dezelfde invoer door de C++-port en eist exact dezelfde uitkomst.

Bron: dashboard_daemon.py uit de Pi-back-up (home_pi.tar.gz) of een los .py-bestand.
Echte scenario's: battery_log + metingen uit de Pi-database (emsmachine_export.db).

    python -I tools/control_reference.py
    python -I tools/control_reference.py --source pad/naar/dashboard_daemon.py --db pad/naar/emsmachine_export.db
"""
import argparse
import ast
import hashlib
import json
import math
import os
import random
import sqlite3
import tarfile
import textwrap
import types
from datetime import datetime, timedelta

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BACKUP = os.path.join(os.path.dirname(ROOT), "Pi-backup-2026-10-22")
TAR_MEMBER = "./EMS/dashboard/dashboard_daemon.py"
OUT = os.path.join(ROOT, "test", "test_ems_logic", "control_vectors.h")

# Begin/eind van de twee stukken die geport zijn (zie battery_control.h).
INIT_START = "_startup_mode = marstek_client.get_mode()"
INIT_END = "block_started_monotonic = None"
STEP_START = 'if message["battery_soc_pct"] is not None:'
STEP_END = 'message["battery_plan_eigen_zon_wh"] = battery_plan_eigen_zon_wh'

MODES = ["Auto", "Passive", "Manual"]
ACTIONS = [None, "auto", "charge", "block"]  # index = decision_action_t
KINDS = {None: 0, "auto": 1, "passive": 2}


def load_source(path):
    if path.endswith((".tar.gz", ".tgz")):
        with tarfile.open(path) as tar:
            return tar.extractfile(TAR_MEMBER).read().decode("utf-8")
    with open(path, encoding="utf-8") as f:
        return f.read()


def slice_block(lines, start, end, include_end):
    i = next(n for n, l in enumerate(lines) if l.strip() == start)
    j = next(n for n in range(i, len(lines)) if lines[n].strip() == end)
    return textwrap.dedent("\n".join(lines[i:j + 1 if include_end else j]))


def module_constants(src):
    """Alle UPPER_CASE = <expr> op moduleniveau, in volgorde geëvalueerd."""
    ns = {}
    for node in ast.parse(src).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            name = node.targets[0].id
            if name.isupper():
                try:
                    ns[name] = eval(compile(ast.Expression(node.value), "<const>", "eval"), {}, dict(ns))
                except Exception:
                    pass
    return ns


class Reference:
    """Het Pi-blok met nagebootste marstek_client/storage/time/logger."""

    def __init__(self, src):
        lines = src.splitlines()
        self.init_code = compile(slice_block(lines, INIT_START, INIT_END, True), "pi_init", "exec")
        self.step_code = compile(slice_block(lines, STEP_START, STEP_END, False), "pi_step", "exec")
        self.consts = module_constants(src)

    def start(self, cfg, startup_reading):
        self.now = 0.0
        self.tick = None
        self.out = None
        ref = self

        class Log:
            def info(self, fmt, *a):
                ref._log(fmt, a)

            def warning(self, fmt, *a):
                ref._log(fmt, a)

        def get_mode():
            return ref._reading

        def write_auto_mode():
            ref.out["cmd"] = ("auto", 0.0, 0.0)
            return ref.tick["ack"]

        def write_passive_mode(power_w, duration_s):
            ref.out["cmd"] = ("passive", float(power_w), float(duration_s))
            return ref.tick["ack"]

        def log_battery_event(_conn, event, detail):
            if event == "command_sent":
                ref.out["cmd_action"] = detail["action"]
            elif event == "mode_mismatch":
                ref.out["mismatch"] = detail

        self.ns = dict(self.consts)
        self.ns.update({
            "marstek_client": types.SimpleNamespace(get_mode=get_mode, write_auto_mode=write_auto_mode,
                                                    write_passive_mode=write_passive_mode),
            "storage": types.SimpleNamespace(log_battery_event=log_battery_event),
            "time": types.SimpleNamespace(monotonic=lambda: ref.now),
            "logger": Log(),
            "conn": None,
            "battery_active_control_enabled": True,
            "battery_max_power_w": cfg["max_power_w"],
            "battery_min_soc_pct": cfg["min_soc_pct"],
            "battery_max_soc_pct": cfg["max_soc_pct"],
            "raw_decision": None,
            "last_known_soc_pct": None,
        })
        self._reading = startup_reading
        exec(self.init_code, self.ns)

    def _log(self, fmt, args):
        if self.out is None:
            return
        if "minimale laadduur" in fmt:
            self.out["charge_floor"] = True
        elif "minimale blokkeerduur" in fmt:
            self.out["block_floor"] = True
        elif "ES.GetMode mislukt" in fmt:
            self.out["getmode_failed"] = True
        elif "preventief opnieuw" in fmt:
            self.out["verify_blind"] = True

    def step(self, tick):
        self.now = tick["t"]
        self.tick = tick
        self._reading = tick["reading"]
        self.out = {"cmd": None, "cmd_action": None, "mismatch": None, "charge_floor": False,
                    "block_floor": False, "getmode_failed": False, "verify_blind": False}
        self.ns["message"] = {"battery_soc_pct": tick["soc"], "grid_power_w": tick["grid"]}
        self.ns["raw_decision"] = tick["decision"]
        exec(self.step_code, self.ns)
        ns = self.ns
        self.out["state"] = {
            "last_sent_action": ns["last_sent_action"],
            "last_sent_power_w": ns["last_sent_power_w"],
            "fails": ns["consecutive_getmode_failures"],
            "charge_started": ns["charge_started_monotonic"],
            "block_started": ns["block_started_monotonic"],
        }
        return self.out


# ---------------------------------------------------------------- batterijmodel

class Battery:
    """Simpele batterij die het laatste bevestigde commando volgt, met de
    storingen die de Pi in het echt zag: GetMode-timeouts, terugvallen naar
    Auto, en "Passive" melden maar ~0 W leveren."""

    def __init__(self, rng, p_fail, p_drift):
        self.rng = rng
        self.p_fail = p_fail
        self.p_drift = p_drift
        self.mode = rng.choice(["Auto", "Auto", "Passive"])
        self.power = rng.choice([0.0, -2500.0, 0.0])

    def apply(self, cmd, ack):
        if cmd is None or not ack:
            return
        kind, power, _duration = cmd
        if kind == "auto":
            self.mode, self.power = "Auto", 0.0
        else:
            self.mode, self.power = "Passive", power

    def read(self):
        r = self.rng.random()
        if r < self.p_drift:
            self.mode, self.power = "Auto", 0.0
        elif r < self.p_drift * 1.5 and self.mode == "Passive":
            self.power = 0.0
        if self.rng.random() < self.p_fail:
            return None
        reading = {}
        if self.rng.random() > 0.01:
            reading["mode"] = "Manual" if self.rng.random() < 0.005 else self.mode
        if self.rng.random() > 0.02:
            # Gemeten vermogen ruist; soms een paar W, soms flink ernaast.
            noise = self.rng.choice([0, 0, 0, 3, -7, 18, -30, 60])
            reading["ongrid_power"] = int(round(self.power + noise))
        return reading


def make_decision(action, max_power_w, remaining_s):
    if action is None:
        return None
    power = -max_power_w if action == "charge" else 0.0
    return {"action": action, "power_w": power, "remaining_s": remaining_s}


def run_scenario(ref, name, cfg, startup_reading, inputs, rng, battery):
    """inputs: lijst {t, decision, soc, grid}; vult reading/ack in via het
    batterijmodel (gesloten lus met de Pi-code) en legt de uitkomst vast."""
    ref.start(cfg, startup_reading)
    init_state = ref.ns["last_sent_action"]
    ticks = []
    for inp in inputs:
        tick = dict(inp)
        tick["reading"] = battery.read() if tick["decision"] is not None else None
        tick["ack"] = rng.random() < cfg["ack_rate"]
        out = ref.step(tick)
        battery.apply(out["cmd"], tick["ack"])
        tick["out"] = out
        ticks.append(tick)
    return {"name": name, "cfg": cfg, "startup": startup_reading, "init_action": init_state, "ticks": ticks}


# ---------------------------------------------------------------- scenario's

def random_scenarios(ref, rng, count, ticks_each):
    out = []
    for n in range(count):
        dt = rng.choice([10.0, 10.0, 30.0])
        jitter = rng.random() < 0.5
        cfg = {"max_power_w": rng.choice([2500.0, 800.0, 1200.0]), "min_soc_pct": rng.choice([12.0, 20.0]),
               "max_soc_pct": rng.choice([100.0, 90.0]), "ack_rate": rng.choice([0.64, 0.9, 1.0, 0.4])}
        battery = Battery(rng, p_fail=rng.choice([0.05, 0.2, 0.5, 0.8]), p_drift=rng.choice([0.0, 0.02, 0.08]))
        startup = rng.choice([None, {"mode": "Auto", "ongrid_power": 0}, {"mode": "Passive", "ongrid_power": -2490},
                              {"mode": "Passive", "ongrid_power": 12}, {"mode": "Passive"}])
        t = rng.uniform(100, 1e5)
        soc = rng.uniform(0, 100)
        action, seg_left, remaining = None, 0, 0.0
        inputs = []
        for _ in range(ticks_each):
            if seg_left <= 0:
                action = rng.choice(ACTIONS + ["charge", "block", "auto"])
                # 88-92 cycli van 10 s: wisselen rond de grens van de minimale
                # laad-/blokduur (900 s), waar een afwijking anders onopgemerkt blijft.
                seg_left = rng.choice([1, 2, 5, 20, 60, 120, 88, 89, 90, 91, 92])
                remaining = rng.choice([0.0, 300.0, 1731.670531, 5400.0, 7200.0, 899.5])
            seg_left -= 1
            remaining = max(0.0, remaining - dt)
            soc = min(100.0, max(0.0, soc + rng.uniform(-2, 2)))
            if rng.random() < 0.05:
                soc = rng.choice([cfg["min_soc_pct"], cfg["max_soc_pct"], cfg["min_soc_pct"] + 0.5])
            inputs.append({
                "t": round(t, 3),
                "decision": make_decision(action, cfg["max_power_w"], remaining),
                "soc": None if rng.random() < 0.1 else round(soc, 1),
                "grid": None if rng.random() < 0.05 else rng.choice(
                    [round(rng.uniform(-3500, 3000), 2), -10.0, -26.0, -25.0, 400.0]),
            })
            t += dt + (rng.uniform(0.0, 2.5) if jitter else 0.0)
        out.append(run_scenario(ref, "random_%02d" % n, cfg, startup, inputs, rng, battery))
    return out


def window_remaining(windows, now_local, plan_local):
    """Resterende seconden in het venster waar `now` in valt (start_time "HH:MM"
    of "morgen HH:MM" t.o.v. het moment van de replan), of None."""
    for w in windows or []:
        st = w.get("start_time") if isinstance(w, dict) else None
        if not st:
            continue
        day = plan_local.date() + (timedelta(days=1) if st.startswith("morgen") else timedelta())
        hh, mm = map(int, st.split()[-1].split(":"))
        start = datetime(day.year, day.month, day.day, hh, mm)
        end = start + timedelta(minutes=15 * w.get("length_intervals", 1))
        if start <= now_local < end:
            return (end - now_local).total_seconds()
    return None


def real_scenarios(ref, rng, db_path, count, hours, dt=10.0):
    """Beslissingen uit de echte replan-events van de Pi, SOC en P1 uit de
    echte metingen; batterijreacties uit het model met de echte ACK-ratio."""
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    acks = [json.loads(d).get("ack") for (d,) in con.execute("select detail from battery_log where event='command_sent'")]
    ack_rate = sum(1 for a in acks if a) / max(1, len(acks))
    sends = [t for (t,) in con.execute("select timestamp from battery_log where event='command_sent' order by timestamp")]
    replans = [(t, json.loads(d)) for t, d in con.execute(
        "select timestamp, detail from battery_log where event='replan' order by timestamp")]
    out = []
    picks = sorted(rng.sample(sends, count)) if len(sends) >= count else sends
    for n, centre in enumerate(picks):
        t0, t1 = centre - hours * 1800, centre + hours * 1800
        meas = {}
        for src in ("grid_power", "battery_soc_pct"):
            meas[src] = list(con.execute("select timestamp, value from measurements where source=? and timestamp between ? and ?"
                                         " order by timestamp", (src, t0 - 120, t1)))
        cfg = {"max_power_w": 2500.0, "min_soc_pct": 12.0, "max_soc_pct": 100.0, "ack_rate": ack_rate}
        battery = Battery(rng, p_fail=0.15, p_drift=0.02)
        idx = {"grid_power": 0, "battery_soc_pct": 0}
        rp = 0
        inputs = []
        t = float(t0)
        while t < t1:
            while rp + 1 < len(replans) and replans[rp + 1][0] <= t:
                rp += 1
            decision = None
            if replans and replans[rp][0] <= t and t - replans[rp][0] < 600:
                detail = replans[rp][1]
                action = detail.get("action")
                if action:
                    now_local = datetime.fromtimestamp(t)
                    plan_local = datetime.fromtimestamp(replans[rp][0])
                    key = "windows_chosen" if action == "charge" else "block_windows_chosen"
                    remaining = window_remaining(detail.get(key), now_local, plan_local)
                    if remaining is None:
                        remaining = 0.0 if action == "auto" else 900.0
                    decision = make_decision(action, cfg["max_power_w"], remaining)
            vals = {}
            for src, rows in meas.items():
                while idx[src] + 1 < len(rows) and rows[idx[src] + 1][0] <= t:
                    idx[src] += 1
                ok = rows and rows[idx[src]][0] <= t and t - rows[idx[src]][0] <= 180
                vals[src] = rows[idx[src]][1] if ok else None
            inputs.append({"t": t, "decision": decision, "soc": vals["battery_soc_pct"], "grid": vals["grid_power"]})
            t += dt
        startup = rng.choice([None, {"mode": "Auto", "ongrid_power": 0}, {"mode": "Passive", "ongrid_power": -2500}])
        out.append(run_scenario(ref, "pi_%s" % datetime.fromtimestamp(centre).strftime("%Y%m%d_%H%M"), cfg, startup,
                                inputs, rng, battery))
    return out


# ---------------------------------------------------------------- C-header

def c_double(v):
    return "0.0" if v is None else repr(float(v))


def reading_fields(r):
    if r is None:
        return "0, 0, 0, 0, 0.0"
    mode = r.get("mode")
    has_ongrid = r.get("ongrid_power") is not None
    return "1, %d, %d, %d, %s" % (mode is not None, MODES.index(mode) if mode is not None else 0, has_ongrid,
                                   c_double(r.get("ongrid_power")))


def tick_line(tick):
    d = tick["decision"]
    o = tick["out"]
    st = o["state"]
    cmd = o["cmd"]
    m = o["mismatch"]
    mm_passive = bool(m) and m.get("expected") == "Passive"
    mm_power = mm_passive and m.get("confirmed") in (None, "Passive")
    fields = [
        c_double(tick["t"]),
        str(ACTIONS.index(d["action"]) if d else 0), c_double(d["power_w"] if d else None),
        c_double(d["remaining_s"] if d else None),
        str(int(tick["soc"] is not None)), c_double(tick["soc"]),
        str(int(tick["grid"] is not None)), c_double(tick["grid"]),
        reading_fields(tick["reading"]), str(int(tick["ack"])),
        # verwacht
        str(KINDS[cmd[0] if cmd else None]), str(ACTIONS.index(o["cmd_action"]) if cmd else 0),
        c_double(cmd[1] if cmd else None), c_double(cmd[2] if cmd else None),
        str(int(bool(m))), str(int(mm_passive)), str(int(mm_power)),
        str(int(o["verify_blind"])), str(int(o["getmode_failed"])), str(int(o["charge_floor"])), str(int(o["block_floor"])),
        str(ACTIONS.index(st["last_sent_action"])), str(int(st["last_sent_power_w"] is not None)),
        c_double(st["last_sent_power_w"]), str(st["fails"]),
        str(int(st["charge_started"] is not None)), c_double(st["charge_started"]),
        str(int(st["block_started"] is not None)), c_double(st["block_started"]),
    ]
    return "    {" + ", ".join(fields) + "},"


def write_header(scenarios, source_desc, source_sha):
    total = sum(len(s["ticks"]) for s in scenarios)
    lines = [
        "// GEGENEREERD door tools/control_reference.py - niet met de hand aanpassen.",
        "// Bron: %s (sha256 %s)" % (source_desc, source_sha[:16]),
        "// %d scenario's, %d cycli. Verwachte waarden = uitkomst van de originele Pi-code." % (len(scenarios), total),
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    double t;",
        "    uint8_t dec_action; double dec_power_w, dec_remaining_s;",
        "    uint8_t soc_valid; double soc;",
        "    uint8_t grid_valid; double grid;",
        "    uint8_t r_valid, r_has_mode, r_mode, r_ongrid_valid; double r_ongrid;",
        "    uint8_t ack;",
        "    uint8_t x_kind, x_action; double x_power_w, x_duration_s;",
        "    uint8_t x_mismatch, x_mismatch_passive, x_mismatch_power;",
        "    uint8_t x_blind, x_getmode_failed, x_charge_floor, x_block_floor;",
        "    uint8_t x_last_action, x_power_valid; double x_power; int x_fails;",
        "    uint8_t x_cs_valid; double x_cs; uint8_t x_bs_valid; double x_bs;",
        "} ctrl_vec_tick_t;",
        "",
        "typedef struct {",
        "    const char * name;",
        "    double max_power_w, min_soc_pct, max_soc_pct;",
        "    uint8_t s_valid, s_has_mode, s_mode, s_ongrid_valid; double s_ongrid;",
        "    uint8_t x_init_action;",
        "    int first, count;",
        "} ctrl_vec_scenario_t;",
        "",
        "static const char * const CTRL_VEC_MODES[] = {%s};" % ", ".join('"%s"' % m for m in MODES),
        "",
        "static const ctrl_vec_tick_t CTRL_VEC_TICKS[] = {",
    ]
    heads = []
    first = 0
    for s in scenarios:
        lines.append("    // %s" % s["name"])
        lines.extend(tick_line(t) for t in s["ticks"])
        c = s["cfg"]
        heads.append('    {"%s", %s, %s, %s, %s, %d, %d, %d},' % (
            s["name"], c_double(c["max_power_w"]), c_double(c["min_soc_pct"]), c_double(c["max_soc_pct"]),
            reading_fields(s["startup"]), ACTIONS.index(s["init_action"]), first, len(s["ticks"])))
        first += len(s["ticks"])
    lines += ["};", "", "static const ctrl_vec_scenario_t CTRL_VEC_SCENARIOS[] = {"] + heads + ["};", ""]
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    return total


def summarize(scenarios):
    agg = {}
    for s in scenarios:
        for t in s["ticks"]:
            o = t["out"]
            for k in ("charge_floor", "block_floor", "getmode_failed", "verify_blind"):
                agg[k] = agg.get(k, 0) + int(o[k])
            if o["mismatch"]:
                agg["mismatch"] = agg.get("mismatch", 0) + 1
            if o["cmd"]:
                key = "send_%s" % o["cmd_action"]
                agg[key] = agg.get(key, 0) + 1
    return agg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=os.path.join(DEFAULT_BACKUP, "home_pi.tar.gz"))
    ap.add_argument("--db", default=os.path.join(DEFAULT_BACKUP, "emsmachine_export.db"))
    ap.add_argument("--random", type=int, default=40)
    ap.add_argument("--ticks", type=int, default=300)
    ap.add_argument("--real", type=int, default=8)
    ap.add_argument("--real-hours", type=float, default=1.0)
    ap.add_argument("--seed", type=int, default=20261022)
    args = ap.parse_args()

    src = load_source(args.source)
    sha = hashlib.sha256(src.encode("utf-8")).hexdigest()
    ref = Reference(src)
    rng = random.Random(args.seed)
    scenarios = random_scenarios(ref, rng, args.random, args.ticks)
    if args.real and os.path.exists(args.db):
        scenarios += real_scenarios(ref, rng, args.db, args.real, args.real_hours)
    total = write_header(scenarios, os.path.basename(args.source), sha)
    print("control_vectors.h: %d scenario's, %d cycli" % (len(scenarios), total))
    print("dekking:", json.dumps(summarize(scenarios), sort_keys=True))


if __name__ == "__main__":
    main()
