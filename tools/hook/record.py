#!/usr/bin/env python3
"""Record a ground-truth scenario from the real game (offline, hooked) -> work/reference/<name>/.

Scenario file (text, v1):
    name   ball_drop_500uu
    arena  Stadium_P            # map package; entered via Training -> Free Play
    ticks  1680                 # physics ticks to record (120 per second)
    ball   pos 0 0 10           # Bullet units (1 BU = 50 uu). Floats, or exact bits as 0x41200000
    ball   vel 0 0 0
    ball   angvel 0 0 0
    ball   basis 1 0 0  0 1 0  0 0 1     # rotation matrix, row-major (btMatrix3x3 rows)
    ball   quat 0 0 0 1                  # alternative to basis: x y z w (converted like btMatrix3x3::setRotation)
    ball   inertia <9 numbers>           # optional world inverse inertia tensor; default: derived from the rotation
    car    pos/vel/angvel/basis/quat/inertia   same for the Free Play car
    input  <from> <to> [throttle v] [steer v] [pitch v] [yaw v] [roll v] [dodgef v] [dodger v]
                       [handbrake 1] [jump 1] [boost 1] [holdboost 1] [airroll 1]
           controller inputs (TAGame._Types_TA.VehicleInputs) for recorded ticks from..to-1 (tick 0 = the tick in
           which the initial state is applied, i.e. the one that produces row 0). Unlisted fields and ticks are 0.
           Injected by the hook at the start of each physics tick, before the game's own tick code.
Setting the rotation also sets the body's world inverse inertia tensor (btRigidBody+0x130), which is real state
(the solver uses it before the next integration refreshes it).
Unset fields keep whatever the game has. All edits are applied at one tick boundary; recording starts with the
state after the following tick.

Output rec.tsv columns: hook tick counter, game physics frame, label, raw bytes (hex):
    ball_bt_xform  btRigidBody world transform: 3 basis rows (4 floats each) + origin (4 floats)
    ball_bt_vel    linear velocity (4 floats) + angular velocity (4 floats)
    ball_rbstate   script-visible RBState (rounded copy, for cross-checks only)
    car_*          same for the Free Play car

Usage: record.py scenario.txt [--keep]     (--keep leaves the game running for the next scenario)
"""
import os, struct, subprocess, sys, time, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get("RLIOS_WORK") or os.path.normpath(os.path.join(HERE, "..", "..", "..", "work"))
SESS = os.path.join(WORK, "reference", "_session")
OFF_BT = 0x790  # RBActor_TA.BulletRigidBody (work/sdk/current/sdk.txt)
OFF_RBSTATE = 0x5E0


def fbits(tok):
    return bytes.fromhex(tok[2:].zfill(8))[::-1] if tok.lower().startswith("0x") else struct.pack("<f", float(tok))


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def num(tok):
    return struct.unpack("<f", fbits(tok))[0]


def quat_to_basis(q):
    """btMatrix3x3::setRotation in float arithmetic (same operation order as core/sim/vec.h setRotation)."""
    x, y, z, w = q
    d = f32(f32(f32(f32(x * x) + f32(y * y)) + f32(z * z)) + f32(w * w))
    s = f32(2.0 / d)
    xs, ys, zs = f32(x * s), f32(y * s), f32(z * s)
    wx, wy, wz = f32(w * xs), f32(w * ys), f32(w * zs)
    xx, xy, xz = f32(x * xs), f32(x * ys), f32(x * zs)
    yy, yz, zz = f32(y * ys), f32(y * zs), f32(z * zs)
    return [f32(1.0 - f32(yy + zz)), f32(xy - wz), f32(xz + wy),
            f32(xy + wz), f32(1.0 - f32(xx + zz)), f32(yz - wx),
            f32(xz - wy), f32(yz + wx), f32(1.0 - f32(xx + yy))]


INV_INERTIA_LOCAL = {"ball": struct.unpack("<f", bytes.fromhex("71f7cc3c"))[0]}  # btRigidBody+0x1C0, measured


def inertia_world(basis, k):
    """btRigidBody::updateInertiaTensor: basis.scaled(k) * basis^T, float arithmetic (core/sim ballInvInertiaWorld).
    k: scalar or per-axis (m_invInertiaLocal); scaled() multiplies column j by k[j]."""
    k = k if isinstance(k, (list, tuple)) else (k, k, k)
    sc = [f32(v * k[i % 3]) for i, v in enumerate(basis)]
    out = []
    for i in range(3):
        for j in range(3):
            out.append(f32(f32(f32(sc[i * 3] * basis[j * 3]) + f32(sc[i * 3 + 1] * basis[j * 3 + 1]))
                           + f32(sc[i * 3 + 2] * basis[j * 3 + 2])))
    return out


def rows4(m):
    """3x3 -> three btVector3 rows (4 floats each, w = 0)."""
    return b"".join(struct.pack("<4f", m[i * 3], m[i * 3 + 1], m[i * 3 + 2], 0.0) for i in range(3)).hex()


def send(lines, wait=2.0):
    out = os.path.join(SESS, "cmd.out")
    before = os.path.getsize(out) if os.path.exists(out) else 0
    tmp = os.path.join(SESS, "cmd.tmp")
    with open(tmp, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.rename(tmp, os.path.join(SESS, "cmd.txt"))
    deadline = time.time() + 30
    while time.time() < deadline:
        time.sleep(0.25)
        if not os.path.exists(os.path.join(SESS, "cmd.txt")) and not os.path.exists(os.path.join(SESS, "cmd.run")):
            break
    time.sleep(wait if wait < 0.5 else 0.3)
    with open(out) as f:
        f.seek(before)
        return f.read()


def game_running():
    return subprocess.run(["pgrep", "-f", "rlinject.ex[e]"], capture_output=True).returncode == 0


def stop_game():
    user, home = os.environ["RL_USER"], os.environ.get("RL_USER_HOME", "/home/" + os.environ["RL_USER"])
    pfx = os.path.join(os.environ["RL_PREFIX"], "pfx")
    ws = os.path.join(os.path.dirname(os.environ["RL_PROTON"]), "files", "bin", "wineserver")
    subprocess.run(["sudo", "-n", "-u", user, "env", f"HOME={home}", f"WINEPREFIX={pfx}", ws, "-k"])


def ensure_session():
    if game_running() and os.path.exists(os.path.join(SESS, "DONE")):
        return
    if game_running():
        stop_game()
        time.sleep(5)
    shutil.rmtree(SESS, ignore_errors=True)
    os.makedirs(SESS)
    env = dict(os.environ, RLHOOK_DUMP_DELAY="40", RLHOOK_STOP_ON_DONE="0")
    subprocess.Popen([os.path.join(HERE, "run.sh"), SESS, "3600"], env=env, stdout=open(SESS + ".launch.log", "w"),
                     stderr=subprocess.STDOUT, start_new_session=True)
    for _ in range(120):
        if os.path.exists(os.path.join(SESS, "DONE")):
            return
        time.sleep(2)
    sys.exit("game did not come up (see %s.launch.log, hook.log)" % SESS)


def find_actor(cls, arena):
    for line in send([f"find {cls}"]).splitlines():
        p = line.split()
        if len(p) == 2 and p[1].startswith(arena + ".TheWorld."):
            return int(p[0], 16)
    return None


OFF_INPUT = 0x81C  # Vehicle_TA.Input (TAGame._Types_TA.VehicleInputs, 0x24 bytes; work/sdk/current/sdk.txt)
# The game's own input path: PlayerController_TA.bOverrideInput (0xA6C mask 4) makes the controller feed
# PlayerController_TA.OverrideInput (0xBB0) to Car.SetVehicleInput (vtable+0x830, RVA 0xEF1290) once per rendered
# frame; the hook also calls SetVehicleInput itself at the start of every tick, so line k is exactly tick k's input.
# Recorded per tick: car_input (Car.Input, 0x81C) and car_inmisc (0x818..0x848, incl. the replicated byte copies).
OFF_PC_FLAGS, OFF_PC_OVERRIDE, OFF_PC_VEHICLEINPUT = 0xA6C, 0xBB0, 0xA48
# Measured with a hardware write watchpoint on Car.Input: inside every physics tick UnrealScript calls
# Car.SetVehicleInput (exec 0xE3F0B0) with the controller's VehicleInput (0xA48), so that is injected too.
INPUT_FLOATS = ["throttle", "steer", "pitch", "yaw", "roll", "dodgef", "dodger"]
INPUT_BITS = {"handbrake": (7, 1), "jump": (7, 2), "boost": (7, 4), "holdboost": (7, 8), "airroll": (8, 1)}


def input_lines(inputs, ticks):
    """One line per tick: 9 hex words of VehicleInputs (see rlhook.c `inputs`)."""
    out = []
    for k in range(ticks):
        w = [0] * 9
        for a, b, kv in inputs:
            if a <= k < b:
                for key, v in kv.items():
                    if key in INPUT_FLOATS:
                        w[INPUT_FLOATS.index(key)] = struct.unpack("<I", fbits(v))[0]
                    elif key in INPUT_BITS:
                        i, m = INPUT_BITS[key]
                        w[i] = (w[i] | m) if num(v) != 0 else (w[i] & ~m)
                    else:
                        sys.exit("unknown input field " + key)
        out.append(" ".join("%08x" % x for x in w))
    return "\n".join(out) + "\n"


def hexread(addr, n):
    txt = send([f"hex {addr:x} {n}"])
    rows = [l for l in txt.splitlines() if l.strip()[:4].isalnum() and ":" in l and not l.startswith(">")]
    return "".join("".join(r.split(":")[1].split()) for r in rows)[: 2 * n]


def read_ptr(addr):
    txt = send([f"hex {addr:x} 8"])
    row = [l for l in txt.splitlines() if l.strip().startswith("0000:")][-1].split()[1:9]
    return int("".join(reversed(row)), 16)


def main():
    scen = sys.argv[1]
    keep = "--keep" in sys.argv
    cfg, edits, inputs = {}, {}, []
    for raw in open(scen):
        t = raw.split("#")[0].split()
        if not t:
            continue
        if t[0] == "input":
            inputs.append((int(t[1]), int(t[2]), dict(zip(t[3::2], t[4::2]))))
        elif t[0] in ("ball", "car"):
            edits.setdefault(t[0], {})[t[1]] = t[2:]
        else:
            cfg[t[0]] = t[1]
    name, arena, ticks = cfg["name"], cfg.get("arena", "Stadium_P"), int(cfg["ticks"])
    ensure_session()
    ball = find_actor("Ball_TA", arena)
    if ball is None:
        send([f"call TAGame.Default__GFxData_Training_TA PlayFreeplayMap n:{arena} i:0"])
        for _ in range(60):
            time.sleep(2)
            ball = find_actor("Ball_TA", arena)
            if ball:
                break
        if ball is None:
            sys.exit("Free Play did not load")
        time.sleep(6)  # spawn / countdown settle
    car = find_actor("Car_TA", arena)
    actors = {"ball": ball, "car": car}
    cmds = []
    for label, a in actors.items():
        if not a:
            continue
        cmds += [f"watchp {label}_bt_xform 0x{a:x} 0x{OFF_BT:x} 0x10 0x40", f"watchp {label}_bt_vel 0x{a:x} 0x{OFF_BT:x} 0x160 0x20",
                 f"watch {label}_rbstate 0x{a:x} 0x{OFF_RBSTATE:x} 0x3c",
                 f"watchp {label}_bt_inertia 0x{a:x} 0x{OFF_BT:x} 0x130 0x30", f"watchp {label}_bt_act 0x{a:x} 0x{OFF_BT:x} 0xf4 0x8"]
    for label, e in edits.items():
        bt = read_ptr(actors[label] + OFF_BT)
        if "pos" in e:
            h = b"".join(fbits(x) for x in e["pos"][:3]).hex()
            cmds += [f"poke 0x{bt:x} 0x40 {h}", f"poke 0x{bt:x} 0x80 {h}"]  # world + interpolation transform origin
        if "vel" in e:
            h = b"".join(fbits(x) for x in e["vel"][:3]).hex()
            cmds += [f"poke 0x{bt:x} 0x160 {h}", f"poke 0x{bt:x} 0x90 {h}"]  # + interpolation linear velocity
        if "angvel" in e:
            h = b"".join(fbits(x) for x in e["angvel"][:3]).hex()
            cmds += [f"poke 0x{bt:x} 0x170 {h}", f"poke 0x{bt:x} 0xa0 {h}"]
        basis = None
        if "basis" in e:
            basis = [num(x) for x in e["basis"][:9]]
        elif "quat" in e:
            basis = quat_to_basis([num(x) for x in e["quat"][:4]])
        if basis:
            h = rows4(basis)
            cmds += [f"poke 0x{bt:x} 0x10 {h}", f"poke 0x{bt:x} 0x50 {h}"]  # world + interpolation transform basis
        inertia = [num(x) for x in e["inertia"][:9]] if "inertia" in e else None
        if inertia is None and basis:
            k = INV_INERTIA_LOCAL.get(label)
            if k is None:  # m_invInertiaLocal (btRigidBody+0x1C0), read live
                k = list(struct.unpack("<3f", bytes.fromhex(hexread(bt + 0x1C0, 12))))
            inertia = inertia_world(basis, k)
        if inertia:
            cmds.append(f"poke 0x{bt:x} 0x130 {rows4(inertia)}")  # m_invInertiaTensorWorld
        cmds.append(f"poke 0x{bt:x} 0xf4 0100000000000000")  # ACTIVE_TAG, deactivation time 0 (a resting body sleeps)
    if inputs:
        if not car:
            sys.exit("inputs need the car")
        with open(os.path.join(SESS, f"{name}.inp"), "w") as f:
            f.write(input_lines(inputs, ticks))
        pc = None
        for line in send(["find PlayerController_TA"]).splitlines():
            p = line.split()
            if len(p) == 2 and p[1].startswith(arena + ".TheWorld."):
                pc = int(p[0], 16)
        if pc is None:
            sys.exit("no PlayerController_TA")
        flags = struct.unpack("<I", bytes.fromhex(hexread(pc + OFF_PC_FLAGS, 4)))[0] | 4  # bOverrideInput
        cmds += [f"poke 0x{pc:x} 0x{OFF_PC_FLAGS:x} {struct.pack('<I', flags).hex()}",
                 f"inputs 0x{pc:x} 0x{OFF_PC_OVERRIDE:x} {name}.inp 0x{car:x} 0x{OFF_PC_VEHICLEINPUT:x}",
                 f"watch car_input 0x{car:x} 0x{OFF_INPUT:x} 0x24", f"watch car_inmisc 0x{car:x} 0x818 0x30"]
    for extra in filter(None, os.environ.get("RLREC_EXTRA", "").split(";")):  # RE helper, e.g. "hwbp {car+0x81c}"
        cmds.append(extra.format_map({"car": car or 0, "ball": ball or 0}) if "{" not in extra else
                    eval('f"' + extra + '"', {}, {"car": car or 0, "ball": ball or 0}))
    cmds.append(f"recstart {name}.tsv {ticks}")
    print(send(cmds))
    done = os.path.join(SESS, f"{name}.tsv.done")
    deadline = time.time() + ticks / 120 * 3 + 60
    while not os.path.exists(done):
        if time.time() > deadline:
            sys.exit("recording did not finish")
        time.sleep(0.5)
    out = os.path.join(WORK, "reference", name)
    os.makedirs(out, exist_ok=True)
    shutil.move(os.path.join(SESS, f"{name}.tsv"), os.path.join(out, "rec.tsv"))
    os.remove(done)
    shutil.copy(scen, os.path.join(out, "scenario.txt"))
    shutil.copy(os.path.join(SESS, "hook.log"), os.path.join(out, "hook.log"))
    n = sum(1 for _ in open(os.path.join(out, "rec.tsv")))
    print(f"recorded {n} rows -> {out}/rec.tsv")
    if not keep:
        stop_game()


if __name__ == "__main__":
    main()
