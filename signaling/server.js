#!/usr/bin/env node
const { WebSocketServer } = require('ws');

const port = Number(process.env.PORT) || 8080;
const wss = new WebSocketServer({ port });
const rooms = new Map();

wss.on('connection', (ws) => {
    let currentRoom = null;

    ws.on('message', (message) => {
        try {
            const msg = JSON.parse(message.toString());
            if (msg.type === 'join') {
                currentRoom = msg.room;
                if (!rooms.has(currentRoom)) {
                    rooms.set(currentRoom, new Set());
                }
                rooms.get(currentRoom).add(ws);
            } else if (currentRoom) {
                const peers = rooms.get(currentRoom);
                if (peers) {
                    for (const peer of peers) {
                        if (peer !== ws && peer.readyState === 1) {
                            peer.send(JSON.stringify(msg));
                        }
                    }
                }
            }
        } catch (e) {
            // Ignore parse errors.
        }
    });

    ws.on('close', () => {
        if (currentRoom && rooms.has(currentRoom)) {
            rooms.get(currentRoom).delete(ws);
            if (rooms.get(currentRoom).size === 0) {
                rooms.delete(currentRoom);
            }
        }
    });
});
console.log(`AetherProxy signaling server running on port ${port}`);
