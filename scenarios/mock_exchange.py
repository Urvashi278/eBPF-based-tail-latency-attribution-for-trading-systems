#!/usr/bin/env python3
"""Offline stand-in for the live feeds, speaking the same wire protocols (stdlib only).

  Bitstamp:  ws  /            bts:subscribe -> live_orders_<pair> order_created/changed/deleted
             GET /api/v2/order_book/<pair>/?group=2   per-order snapshot
  Binance:   ws  /ws/<sym>@depth@100ms                 depthUpdate diffs with U/u sequence
             GET /api/v3/depth?symbol=<SYM>&limit=N    snapshot with lastUpdateId

Usage:
  mock_exchange.py [--port 8765] [--rate 2000] [--gap-every N]
  engine --feed bitstamp --ws-url ws://127.0.0.1:8765 --rest-url http://127.0.0.1:8765
  engine --feed binance  --ws-url ws://127.0.0.1:8765 --rest-url http://127.0.0.1:8765

--gap-every N drops one Binance sequence number every N diffs to exercise resync.
"""
import argparse
import asyncio
import base64
import hashlib
import json
import random
import struct
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class Market:
    """One simulated venue state shared by both protocols."""

    def __init__(self, seed=7):
        self.rng = random.Random(seed)
        self.lock = threading.Lock()
        self.mid = 65000.0
        self.orders = {}           # id -> [side, price, amount]
        self.next_id = 1_700_000_000_000_000
        self.levels = {0: {}, 1: {}}   # side -> {price_str: qty}
        self.update_id = 1_000_000
        for _ in range(400):
            self._new_order()
        for side in (0, 1):
            for i in range(1, 200):
                px = round(self.mid - i * 0.01, 2) if side == 0 else round(self.mid + i * 0.01, 2)
                self.levels[side][f"{px:.2f}"] = round(self.rng.uniform(0.001, 2), 5)

    def _new_order(self):
        side = self.rng.randint(0, 1)
        off = int(self.rng.expovariate(0.15)) + 1
        px = int(self.mid) - off if side == 0 else int(self.mid) + off
        amt = round(self.rng.uniform(0.0005, 1.5), 8)
        oid = self.next_id
        self.next_id += self.rng.randint(1, 50)
        self.orders[oid] = [side, px, amt]
        return oid

    def bitstamp_events(self):
        """One venue event, plus deletes for resting orders it traded against."""
        with self.lock:
            self.mid += self.rng.uniform(-1, 1)
            r = self.rng.random()
            fills = []
            if r < 0.5 or len(self.orders) < 50:
                oid = self._new_order(); ev = "order_created"
                side, px, _ = self.orders[oid]
                for o, (s2, p2, a2) in list(self.orders.items()):
                    if s2 != side and ((side == 0 and p2 <= px) or (side == 1 and p2 >= px)):
                        fills.append(self._evt("order_deleted", o, s2, p2, a2))
                        del self.orders[o]
            elif r < 0.8:
                oid = self.rng.choice(list(self.orders)); ev = "order_deleted"
            else:
                oid = self.rng.choice(list(self.orders)); ev = "order_changed"
                self.orders[oid][2] = round(self.orders[oid][2] * self.rng.uniform(0.1, 0.9), 8)
            side, px, amt = self.orders[oid]
            if ev == "order_deleted":
                del self.orders[oid]
            # the aggressor trades first, then (if anything is left) rests
            return fills + [self._evt(ev, oid, side, px, amt)]

    @staticmethod
    def _evt(ev, oid, side, px, amt):
        us = int(time.time() * 1e6)
        data = {"id": oid, "id_str": str(oid), "order_type": side, "datetime": str(us // 10**6),
                "microtimestamp": str(us), "amount": amt, "amount_str": f"{amt:.8f}",
                "amount_traded": "0", "amount_at_create": f"{amt:.8f}",
                "price": px, "price_str": str(px)}
        return {"data": data, "channel": None, "event": ev}

    def bitstamp_snapshot(self):
        with self.lock:
            us = int(time.time() * 1e6)
            bids = sorted(((p, a, i) for i, (s, p, a) in self.orders.items() if s == 0), key=lambda x: -x[0])
            asks = sorted(((p, a, i) for i, (s, p, a) in self.orders.items() if s == 1), key=lambda x: x[0])
            f = lambda rows: [[str(p), f"{a:.8f}", str(i)] for p, a, i in rows]
            return {"timestamp": str(us // 10**6), "microtimestamp": str(us), "bids": f(bids), "asks": f(asks)}

    def binance_event(self):
        with self.lock:
            self.mid += self.rng.uniform(-0.5, 0.5)
            U = self.update_id + 1
            n = self.rng.randint(3, 25)
            b, a = [], []
            for _ in range(n):
                side = self.rng.randint(0, 1)
                off = int(self.rng.expovariate(0.05)) + 1
                px = round(self.mid - off * 0.01, 2) if side == 0 else round(self.mid + off * 0.01, 2)
                key = f"{px:.2f}"
                qty = 0.0 if (key in self.levels[side] and self.rng.random() < 0.35) else round(self.rng.uniform(0.001, 3), 5)
                if qty == 0:
                    self.levels[side].pop(key, None)
                else:
                    self.levels[side][key] = qty
                    # a new level can't cross the other side: remove what it would trade through
                    other = self.levels[1 - side]
                    for k2 in [k for k in other if (float(k) <= px if side == 0 else float(k) >= px)]:
                        del other[k2]
                        (a if side == 0 else b).append([k2, "0.00000000"])
                (b if side == 0 else a).append([key, f"{qty:.8f}"])
            self.update_id += len(b) + len(a)
            return {"e": "depthUpdate", "E": int(time.time() * 1000), "s": "BTCUSDT",
                    "U": U, "u": self.update_id, "b": b, "a": a}

    def binance_snapshot(self, limit):
        with self.lock:
            bids = sorted(self.levels[0].items(), key=lambda kv: -float(kv[0]))[:limit]
            asks = sorted(self.levels[1].items(), key=lambda kv: float(kv[0]))[:limit]
            return {"lastUpdateId": self.update_id,
                    "bids": [[p, f"{q:.8f}"] for p, q in bids],
                    "asks": [[p, f"{q:.8f}"] for p, q in asks]}


def ws_frame(text):
    data = text.encode()
    n = len(data)
    if n < 126:
        hdr = struct.pack("!BB", 0x81, n)
    elif n < 65536:
        hdr = struct.pack("!BBH", 0x81, 126, n)
    else:
        hdr = struct.pack("!BBQ", 0x81, 127, n)
    return hdr + data


async def read_ws_frame(reader):
    h = await reader.readexactly(2)
    op, n = h[0] & 0x0F, h[1] & 0x7F
    if n == 126:
        n = struct.unpack("!H", await reader.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack("!Q", await reader.readexactly(8))[0]
    mask = await reader.readexactly(4) if h[1] & 0x80 else b"\0\0\0\0"
    data = bytearray(await reader.readexactly(n))
    for i in range(n):
        data[i] ^= mask[i & 3]
    return op, bytes(data)


class Server:
    def __init__(self, args):
        self.a = args
        self.m = Market()

    async def handle(self, reader, writer):
        try:
            req = await reader.readuntil(b"\r\n\r\n")
        except Exception:
            writer.close(); return
        lines = req.decode(errors="replace").split("\r\n")
        path = lines[0].split(" ")[1]
        headers = {l.split(":", 1)[0].strip().lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
        if headers.get("upgrade", "").lower() == "websocket":
            accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + GUID).encode()).digest()).decode()
            writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
            await writer.drain()
            if path.startswith("/ws/"):
                await self.binance_stream(writer)
            else:
                await self.bitstamp_stream(reader, writer)
        else:
            await self.rest(path, writer)

    async def rest(self, path, writer):
        if path.startswith("/api/v2/order_book/"):
            body = json.dumps(self.m.bitstamp_snapshot())
        elif path.startswith("/api/v2/trading-pairs-info"):
            body = json.dumps([{"name": "BTC/USD", "url_symbol": "btcusd", "base_decimals": 8,
                                "counter_decimals": 0, "trading": "Enabled"},
                               {"name": "ETH/USD", "url_symbol": "ethusd", "base_decimals": 8,
                                "counter_decimals": 2, "trading": "Enabled"}])
        elif path.startswith("/api/v3/exchangeInfo"):
            body = json.dumps({"symbols": [{"symbol": "BTCUSDT", "filters": [
                {"filterType": "PRICE_FILTER", "minPrice": "0.01000000", "maxPrice": "1000000.00000000", "tickSize": "0.01000000"},
                {"filterType": "LOT_SIZE", "minQty": "0.00001000", "maxQty": "9000.00000000", "stepSize": "0.00001000"}]}]})
        elif path.startswith("/api/v3/depth"):
            limit = 5000
            if "limit=" in path:
                limit = int(path.split("limit=")[1].split("&")[0])
            body = json.dumps(self.m.binance_snapshot(limit))
        else:
            writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"); await writer.drain(); writer.close(); return
        # chunked, like the real APIs often are
        out = b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
        data = body.encode()
        for i in range(0, len(data), 4096):
            c = data[i:i + 4096]
            out += f"{len(c):x}\r\n".encode() + c + b"\r\n"
        out += b"0\r\n\r\n"
        writer.write(out); await writer.drain(); writer.close()

    async def bitstamp_stream(self, reader, writer):
        op, data = await read_ws_frame(reader)
        sub = json.loads(data)
        channel = sub["data"]["channel"]
        writer.write(ws_frame(json.dumps({"event": "bts:subscription_succeeded", "channel": channel, "data": {}})))
        await writer.drain()
        await self.pump(writer, lambda: self._bitstamp(channel), self.a.rate)

    def _bitstamp(self, channel):
        out = []
        for e in self.m.bitstamp_events():
            e["channel"] = channel
            out.append(json.dumps(e))
        return out

    async def binance_stream(self, writer):
        count = [0]

        def gen():
            e = self.m.binance_event()
            count[0] += 1
            if self.a.gap_every and count[0] % self.a.gap_every == 0:
                e = self.m.binance_event()          # skip one diff -> sequence gap
            return [json.dumps(e)]
        await self.pump(writer, gen, max(1, self.a.rate // 10))

    async def pump(self, writer, gen, rate):
        # Poisson arrivals, sent in small bursts like a real feed
        try:
            while True:
                for msg in gen():
                    writer.write(ws_frame(msg))
                if random.random() < 0.02:
                    writer.write(b"\x89\x00")              # ping, client must pong
                await writer.drain()
                await asyncio.sleep(random.expovariate(rate))
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--rate", type=int, default=2000, help="bitstamp events/s (binance diffs = rate/10)")
    ap.add_argument("--gap-every", type=int, default=0)
    a = ap.parse_args()
    srv = await asyncio.start_server(Server(a).handle, "127.0.0.1", a.port)
    print(f"mock exchange on 127.0.0.1:{a.port}", flush=True)
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
