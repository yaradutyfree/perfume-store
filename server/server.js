/*
 * Perfume Store WebSocket Server
 * Deploy to Render: https://render.com
 * Env vars: ADMIN_SECRET (default: YaraOud2024)
 */
const http = require('http');
const WebSocket = require('ws');

const PORT         = process.env.PORT || 3000;
const ADMIN_SECRET = process.env.ADMIN_SECRET || 'YaraOud2024';

/* ── State ─────────────────────────────────────────────── */
let products   = [];      /* latest product list from admin */
let brands     = [];      /* latest brand list from admin   */
let orderCount = 0;       /* orders received today          */
let adminWs    = null;    /* connected admin client         */

/* ── Server ─────────────────────────────────────────────── */
const server = http.createServer((req, res) => {
    /* health-check endpoint for Render */
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Perfume Store WS Server running\n');
});

const wss = new WebSocket.Server({ server });

wss.on('connection', (ws, req) => {
    ws.role = 'viewer';
    ws.isAlive = true;

    console.log(`[+] connection from ${req.socket.remoteAddress}`);

    /* send current state immediately on connect */
    if (products.length > 0) {
        send(ws, { type: 'products', products, brands });
    }
    send(ws, { type: 'order_count', count: orderCount });

    ws.on('pong', () => { ws.isAlive = true; });

    ws.on('message', (raw) => {
        let msg;
        try { msg = JSON.parse(raw); } catch { return; }

        switch (msg.type) {

            /* ── Admin authenticates ── */
            case 'admin_auth':
                if (msg.secret !== ADMIN_SECRET) {
                    send(ws, { type: 'error', message: 'Wrong secret' });
                    return;
                }
                ws.role = 'admin';
                adminWs = ws;
                console.log('[admin] connected');
                /* send full state to admin */
                send(ws, { type: 'state', products, brands, orderCount });
                break;

            /* ── Admin pushes product update ── */
            case 'update_products':
                if (ws.role !== 'admin') return;
                products = msg.products || [];
                if (Array.isArray(msg.brands)) brands = msg.brands;
                console.log(`[admin] updated ${products.length} products, ${brands.length} brands`);
                /* broadcast to all viewers */
                broadcast('viewer', { type: 'products', products, brands });
                send(ws, { type: 'ack', products, brands, message: 'Products broadcast to all clients' });
                break;

            /* ── Customer places order ── */
            case 'order':
                orderCount++;
                console.log(`[order #${orderCount}] from ${msg.name || 'unknown'}`);
                /* forward to admin */
                if (adminWs && adminWs.readyState === WebSocket.OPEN) {
                    send(adminWs, {
                        type:       'new_order',
                        orderCount,
                        name:       msg.name  || '',
                        city:       msg.city  || '',
                        items:      msg.items || [],
                        total:      msg.total || 0,
                        timestamp:  new Date().toISOString()
                    });
                }
                /* broadcast updated count to all */
                broadcast('viewer', { type: 'order_count', count: orderCount });
                send(ws, { type: 'order_ack', message: 'Order received!' });
                break;

            /* ── Admin resets daily counter ── */
            case 'reset_count':
                if (ws.role !== 'admin') return;
                orderCount = 0;
                broadcast('viewer', { type: 'order_count', count: 0 });
                break;

            /* ── Ping / keepalive ── */
            case 'ping':
                send(ws, { type: 'pong' });
                break;
        }
    });

    ws.on('close', () => {
        if (ws === adminWs) { adminWs = null; console.log('[admin] disconnected'); }
    });

    ws.on('error', (err) => console.error('ws error:', err.message));
});

/* ── Helpers ────────────────────────────────────────────── */
function send(ws, obj) {
    if (ws && ws.readyState === WebSocket.OPEN)
        ws.send(JSON.stringify(obj));
}

function broadcast(role, obj) {
    const json = JSON.stringify(obj);
    wss.clients.forEach(c => {
        if (c.readyState === WebSocket.OPEN && c.role === role)
            c.send(json);
    });
}

/* ── Heartbeat (detect dead connections) ────────────────── */
const heartbeat = setInterval(() => {
    wss.clients.forEach(ws => {
        if (!ws.isAlive) { ws.terminate(); return; }
        ws.isAlive = false;
        ws.ping();
    });
}, 30000);

wss.on('close', () => clearInterval(heartbeat));

/* ── Reset order count at midnight ─────────────────────── */
function scheduleReset() {
    const now  = new Date();
    const next = new Date(now);
    next.setHours(24, 0, 0, 0);
    setTimeout(() => {
        orderCount = 0;
        broadcast('viewer', { type: 'order_count', count: 0 });
        if (adminWs) send(adminWs, { type: 'order_count', count: 0 });
        console.log('[midnight] order count reset');
        scheduleReset();
    }, next - now);
}
scheduleReset();

server.listen(PORT, () => console.log(`Perfume Store WS Server on port ${PORT}`));
