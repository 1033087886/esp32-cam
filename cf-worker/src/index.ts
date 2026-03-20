import { DurableObject } from "cloudflare:workers";

const DEFAULT_ROOM = "cam01";

function isWebSocketRequest(request) {
  return request.headers.get("Upgrade")?.toLowerCase() === "websocket";
}

function getRoom(raw) {
  const room = raw?.trim() ?? "";
  return room.length > 0 ? room : DEFAULT_ROOM;
}

function unauthorized(message) {
  return new Response(message, { status: 401 });
}

function authorize(role, token, env) {
  if (role === "publisher") {
    if (!env.INGEST_TOKEN) {
      return new Response("INGEST_TOKEN is not configured", { status: 500 });
    }
    if (token !== env.INGEST_TOKEN) {
      return unauthorized("invalid ingest token");
    }
    return null;
  }

  const viewerToken = env.VIEWER_TOKEN?.trim() ?? "";
  if (!viewerToken) {
    return null;
  }
  if (token !== viewerToken) {
    return unauthorized("invalid viewer token");
  }
  return null;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;

    if (path === "/healthz") {
      return new Response("ok");
    }

    if (path !== "/esp32" && path !== "/viewer") {
      return new Response("Not Found", { status: 404 });
    }

    if (!isWebSocketRequest(request)) {
      return new Response("Expected WebSocket upgrade", { status: 426 });
    }

    const role = path === "/esp32" ? "publisher" : "viewer";
    const room = getRoom(url.searchParams.get("room"));
    const token = url.searchParams.get("token") ?? "";

    const authResult = authorize(role, token, env);
    if (authResult) {
      return authResult;
    }

    const id = env.ROOM_RELAY.idFromName(room);
    const stub = env.ROOM_RELAY.get(id);
    url.searchParams.set("room", room);
    url.searchParams.delete("token");

    return stub.fetch(new Request(url.toString(), request));
  },
};

export class RoomRelay extends DurableObject {
  publisher = null;
  viewers = new Set();

  constructor(ctx, env) {
    super(ctx, env);
    this.restoreSocketsFromHibernation();
  }

  async fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname;

    if (path !== "/esp32" && path !== "/viewer") {
      return new Response("Not Found", { status: 404 });
    }

    if (!isWebSocketRequest(request)) {
      return new Response("Expected WebSocket upgrade", { status: 426 });
    }

    const role = path === "/esp32" ? "publisher" : "viewer";
    const room = getRoom(url.searchParams.get("room"));

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    const attachment = {
      role,
      room,
      connectedAt: Date.now(),
    };

    server.serializeAttachment(attachment);
    this.ctx.acceptWebSocket(server);

    if (role === "publisher") {
      this.replacePublisher(server);
    } else {
      this.viewers.add(server);
    }

    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    const attachment = this.getAttachment(ws);
    if (!attachment) {
      try {
        ws.close(1011, "missing attachment");
      } catch {
        // no-op
      }
      this.removeSocket(ws, null);
      return;
    }

    if (attachment.role !== "publisher") {
      // Forward text control messages from viewer to publisher.
      if (typeof message === "string" && this.publisher && this.publisher.readyState === WebSocket.OPEN) {
        try {
          this.publisher.send(message);
        } catch {
          // no-op
        }
      }
      return;
    }

    if (typeof message === "string") {
      return;
    }

    this.broadcastToViewers(message);
  }

  webSocketClose(ws) {
    this.removeSocket(ws, this.getAttachment(ws));
  }

  webSocketError(ws) {
    this.removeSocket(ws, this.getAttachment(ws));
  }

  restoreSocketsFromHibernation() {
    for (const ws of this.ctx.getWebSockets()) {
      const attachment = this.getAttachment(ws);
      if (!attachment) {
        try {
          ws.close(1011, "missing attachment");
        } catch {
          // no-op
        }
        continue;
      }

      if (attachment.role === "publisher") {
        if (this.publisher && this.publisher !== ws) {
          try {
            ws.close(1012, "publisher already exists");
          } catch {
            // no-op
          }
          continue;
        }
        this.publisher = ws;
      } else {
        this.viewers.add(ws);
      }
    }
  }

  replacePublisher(nextPublisher) {
    if (this.publisher && this.publisher !== nextPublisher) {
      try {
        this.publisher.close(1012, "publisher replaced");
      } catch {
        // no-op
      }
    }
    this.publisher = nextPublisher;
  }

  broadcastToViewers(frame) {
    const staleSockets = [];
    for (const ws of this.viewers) {
      if (ws.readyState !== WebSocket.OPEN) {
        staleSockets.push(ws);
        continue;
      }

      try {
        ws.send(frame);
      } catch {
        staleSockets.push(ws);
      }
    }

    for (const ws of staleSockets) {
      this.removeSocket(ws, this.getAttachment(ws));
      try {
        ws.close(1011, "viewer send failed");
      } catch {
        // no-op
      }
    }
  }

  getAttachment(ws) {
    const raw = ws.deserializeAttachment();
    if (!raw || typeof raw !== "object") {
      return null;
    }

    const role = raw.role;
    const room = raw.room;
    const connectedAt = raw.connectedAt;

    if ((role !== "publisher" && role !== "viewer") || typeof room !== "string") {
      return null;
    }
    if (typeof connectedAt !== "number") {
      return null;
    }

    return { role, room, connectedAt };
  }

  removeSocket(ws, attachment) {
    const meta = attachment ?? this.getAttachment(ws);
    if (!meta) {
      return;
    }

    if (meta.role === "publisher") {
      if (this.publisher === ws) {
        this.publisher = null;
      }
      return;
    }

    this.viewers.delete(ws);
  }
}
