// Log shipping (v1.53.4): everything wave-os prints is also kept in a RAM ring (rlog.h: ship_ring) and sent to the PC over Wi-Fi as
// small UDP packets (port 5140), after every command and as soon as the network is up (so the whole boot history arrives too).
// On the PC, tools\logserver.ps1 (start-logserver.bat / start-pc.bat) appends it all to wave-log.txt.
// The password is never in it: the prompt prints only '*' and the typed characters never go through the print routine.

#define LOG_PORT 5140
static u32 ship_r;                         // how much of the ring has been sent (absolute count, like ship_w)
static u32 ship_on = 1, ship_sent, ship_lost, ship_fail;
static u64 ship_pause_until;

static int log_ship(void) {
    if (!ship_on || !net_ip || !update_srv || !nic_send) return 0;
    u64 hz = tick_hz();
    if (ship_pause_until && ticks() < ship_pause_until) return 0;          // the PC did not answer ARP a moment ago: do not stall every command
    if (ship_w - ship_r > SHIP_RING) { ship_lost += ship_w - ship_r - SHIP_RING; ship_r = ship_w - SHIP_RING; }    // the ring lapped us: skip what was overwritten
    static u8 tmp[1100];
    while (ship_r != ship_w) {
        u32 avail = ship_w - ship_r, n = avail < 1000 ? avail : 1000;
        for (u32 i = 0; i < n; i++) tmp[i] = ship_ring[(ship_r + i) & (SHIP_RING - 1)];
        if (n == 1000) { u32 cut = n; while (cut > 500 && tmp[cut - 1] != '\n') cut--; if (tmp[cut - 1] == '\n') n = cut; }   // end the packet at a line end if there is one
        if (net_udp(update_srv, 5141, LOG_PORT, tmp, n)) {
            ship_fail++;
            if (hz) ship_pause_until = ticks() + hz * 10;
            return 0;
        }
        ship_r += n; ship_sent += n;
        delay_us(2000);                                                    // do not flood the chip's transmit queue
    }
    return 1;
}

// log [on|off]: show / switch the shipping
static int wave_log(const char *arg) {
    if (arg[0] == 'o' && arg[1] == 'n') { ship_on = 1; ship_pause_until = 0; }
    else if (arg[0] == 'o' && arg[1] == 'f') ship_on = 0;
    int ok = log_ship();
    puts("log to the PC: "); puts(ship_on ? "on" : "off"); puts(", sent "); put_dec(ship_sent); puts(" bytes, lost "); put_dec(ship_lost); puts(", failed sends "); put_dec(ship_fail); putc('\n');
    sum_s(ship_on ? (net_ip ? (ok ? "sent " : "not sent ") : "no network ") : "off "); sum_u(ship_sent); sum_s(" B");
    return ok;
}
