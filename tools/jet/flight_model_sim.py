#!/usr/bin/env python3
"""The F-16 flight model (src/gta4_trainer.cpp, JetControls) in Python, run
through a few manoeuvres: the launch off the wheels, takeoff and climb, idle glide, hands-off bank,
pulled turn, stall, afterburner, rudder. Prints speed, climb, bank, heading
and height every few seconds."""
import math
def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx, aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)
def qaxis(x, y, z, ang):
    h = ang * 0.5; s = math.sin(h); return (x*s, y*s, z*s, math.cos(h))
def qnorm(q):
    n = math.sqrt(sum(c*c for c in q)); return tuple(c/n for c in q)
def qrot(q, v):
    qx, qy, qz, qw = q
    tx = 2*(qy*v[2] - qz*v[1]); ty = 2*(qz*v[0] - qx*v[2]); tz = 2*(qx*v[1] - qy*v[0])
    return (v[0] + qw*tx + (qy*tz - qz*ty), v[1] + qw*ty + (qz*tx - qx*tz), v[2] + qw*tz + (qx*ty - qy*tx))
kIdle, kDry, kBurn, kDrag, kLift, kAuth, kStall, kMax = 1.3, 10.3, 6.7, 0.0021, 32.0, 45.0, 16.0, 95.0
def step(st, dt, lx, ly, rud, thr, ab):
    q, v, rate = st['q'], st['v'], st['rate']
    f = qrot(q, (0,1,0)); r = qrot(q, (1,0,0)); u = qrot(q, (0,0,1))
    burner = thr > 0.9
    thrust = kIdle + (kDry - kIdle)*thr + (kBurn if burner else 0)
    drag = kDrag*v*v*(1 + 2.5*ab) + 2.0*ab
    v2 = min(max(v + (thrust - drag - 9.81*f[2])*dt, 0), kMax)
    auth = min(max((v - 12)/(kAuth - 12), 0.2), 1.0)
    want = (-ly*0.95*auth, lx*2.6*auth, rud*0.45*auth)
    ease = min(1, dt*5)
    rate = [rate[a] + (want[a]-rate[a])*ease for a in range(3)]
    q = qmul(q, qaxis(1,0,0, rate[0]*dt)); q = qmul(q, qaxis(0,1,0, rate[1]*dt)); q = qmul(q, qaxis(0,0,1, -rate[2]*dt))
    q = qmul(qaxis(0,0,1, r[2]*0.3*auth*dt), q)
    level = math.sqrt(max(0, 1 - f[2]*f[2]))
    bank_cos = max(-1, min(1, u[2]/max(level, 0.2)))
    hold = min(1/max(abs(bank_cos), 0.5), 2.0)
    lift = 9.81*level*min(1, v*v/(kLift*kLift))*(0.5 + 0.5*hold)
    along = -9.81*f[2]
    bend = [u[a]*lift + (-9.81 if a == 2 else 0) - f[a]*along for a in range(3)]
    axis = (f[1]*bend[2]-f[2]*bend[1], f[2]*bend[0]-f[0]*bend[2], f[0]*bend[1]-f[1]*bend[0])
    mag = math.sqrt(sum(c*c for c in axis))
    if mag > 1e-3: q = qmul(qaxis(axis[0]/mag, axis[1]/mag, axis[2]/mag, mag/max(v,10)*dt), q)
    if v < kStall and f[2] > -0.95:
        drop = (-f[1], f[0], 0); n = math.hypot(drop[0], drop[1]); fall = (1 - v/kStall)*1.4*dt
        q = qmul(qaxis(drop[0]/n, drop[1]/n, 0, fall), q) if n > 1e-3 else qmul(q, qaxis(1,0,0,-fall))
    q = qnorm(q)
    f = qrot(q, (0,1,0))
    st['pos'] = [st['pos'][a] + f[a]*v2*dt for a in range(3)]
    st.update(q=q, v=v2, rate=rate)
    return st
def run(name, secs, ctl, v0=30.0, q0=(0,0,0,1), every=2.0):
    st = dict(q=q0, v=v0, rate=[0,0,0], pos=[0,0,100.0])
    dt = 1/30; t = 0; nxt = 0
    print('==', name)
    while t <= secs:
        if t >= nxt:
            f = qrot(st['q'], (0,1,0)); r = qrot(st['q'], (1,0,0))
            hd = math.degrees(math.atan2(-f[0], f[1]))
            print('  t %5.1f  v %5.1f  climb %6.1f deg  bank %6.1f deg  heading %7.1f  alt %7.1f' % (t, st['v'], math.degrees(math.asin(max(-1,min(1,f[2])))), math.degrees(math.asin(max(-1,min(1,-r[2])))), hd, st['pos'][2]))
            nxt += every
        st = step(st, dt, *ctl(t))
        t += dt
def launch(name, secs, ctl, every=0.5):
    """The takeoff (v1.7.17): from the wheels at 38 m/s at least, level, the
    nose rotating up at kRotate rad/s at least for the first 0.7 s, however
    gently the stick is pulled (the trainer needs it pulled past 0.4 to
    launch). Prints the height gained: 4 m is where touchdowns are allowed."""
    kRotate, kRotateFor = 0.35, 0.7
    v0 = 38.0
    auth = min(max((v0 - 12)/(kAuth - 12), 0.2), 1.0)
    st = dict(q=(0, 0, 0, 1), v=v0, rate=[max(-ctl(0)[1]*0.95*auth, kRotate), 0, 0], pos=[0, 0, 0.0])
    dt = 1/30; t = 0; nxt = 0; climbed = None
    print('==', name)
    while t <= secs:
        if t >= nxt:
            f = qrot(st['q'], (0,1,0))
            print('  t %5.1f  v %5.1f  climb %6.1f deg  height %6.1f' % (t, st['v'], math.degrees(math.asin(max(-1,min(1,f[2])))), st['pos'][2]))
            nxt += every
        st = step(st, dt, *ctl(t))
        if t < kRotateFor:
            st['rate'][0] = max(st['rate'][0], kRotate)
        t += dt
        if climbed is None and st['pos'][2] > 4.0:
            climbed = t
    print('  4 m up after %s s' % (None if climbed is None else round(climbed, 2)))
launch('launch: full pull 1 s then hands off, full power', 4, lambda t: (0, -1 if t < 1.0 else 0, 0, 1.0, 0))
launch('launch: just past the threshold for 0.3 s then hands off', 4, lambda t: (0, -0.45 if t < 0.3 else 0, 0, 1.0, 0))
run('takeoff, full dry, pull 1.5 s then hands off', 30, lambda t: (0, -1 if t < 1.5 else 0, 0, 0.85, 0))
run('cruise 70 m/s, idle, hands off', 40, lambda t: (0, 0, 0, 0.0, 0), v0=70)
run('roll to ~60 deg bank then hands off, dry 0.7', 16, lambda t: (1 if t < 0.45 else 0, 0, 0, 0.7, 0), v0=60, every=1.0)
run('banked turn with pull: roll 0.5 s then pull', 12, lambda t: (1 if t < 0.5 else 0, -0.7 if t >= 0.5 else 0, 0, 0.8, 0), v0=60, every=1.0)
run('pull vertical then hands off at idle (stall)', 30, lambda t: (0, -1 if t < 2.5 else 0, 0, 0.0, 0), v0=50, every=1.0)
run('afterburner level', 30, lambda t: (0, 0, 0, 1.0, 0), v0=40, every=3.0)
run('rudder right 2 s', 6, lambda t: (0, 0, 1 if t < 2 else 0, 0.7, 0), v0=60, every=0.5)
