import { DurableObject } from "cloudflare:workers";

interface Env {
  ROOM_RELAY: DurableObjectNamespace;
  INGEST_TOKEN: string;
  VIEWER_TOKEN?: string;
}

type SocketRole = "publisher" | "viewer";

interface SocketAttachment {
  role: SocketRole;
  room: string;
  connectedAt: number;
}

const DEFAULT_ROOM = "cam01";

function isWebSocketRequest(request: Request): boolean {
  return request.headers.get("Upgrade")?.toLowerCase() === "websocket";
}

function getRoom(raw: string | null): string {
  const room = raw?.trim() ?? "";
  return room.length > 0 ? room : DEFAULT_ROOM;
}

function unauthorized(message: string): Response {
  return new Response(message, { status: 401 });
}

function authorize(role: SocketRole, token: string, env: Env): Response | null {
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
  async fetch(request, env): Promise<Response> {
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

    const role: SocketRole = path === "/esp32" ? "publisher" : "viewer";
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
} satisfies ExportedHandler<Env>;

export class RoomRelay extends DurableObject {
  private publisher: WebSocket | null = null;
  private viewers = new Set<WebSocket>();

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.restoreSocketsFromHibernation();
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname;

    if (path !== "/esp32" && path !== "/viewer") {
      return new Response("Not Found", { status: 404 });
    }

    if (!isWebSocketRequest(request)) {
      return new Response("Expected WebSocket upgrade", { status: 426 });
    }

    const role: SocketRole = path === "/esp32" ? "publisher" : "viewer";
    const room = getRoom(url.searchParams.get("room"));

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    const attachment: SocketAttachment = {
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

  webSocketMessage(ws: WebSocket, message: ArrayBuffer | string): void {
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
      return;
    }

    if (typeof message === "string") {
      return;
    }

    this.broadcastToViewers(message);
  }

  webSocketClose(ws: WebSocket): void {
    this.removeSocket(ws, this.getAttachment(ws));
  }

  webSocketError(ws: WebSocket): void {
    this.removeSocket(ws, this.getAttachment(ws));
  }

  private restoreSocketsFromHibernation(): void {
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

  private replacePublisher(nextPublisher: WebSocket): void {
    if (this.publisher && this.publisher !== nextPublisher) {
      try {
        this.publisher.close(1012, "publisher replaced");
      } catch {
        // no-op
      }
    }
    this.publisher = nextPublisher;
  }

  private broadcastToViewers(frame: ArrayBuffer): void {
    const staleSockets: WebSocket[] = [];
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

  private getAttachment(ws: WebSocket): SocketAttachment | null {
    const raw = ws.deserializeAttachment();
    if (!raw || typeof raw !== "object") {
      return null;
    }

    const role = (raw as { role?: string }).role;
    const room = (raw as { room?: string }).room;
    const connectedAt = (raw as { connectedAt?: number }).connectedAt;

    if ((role !== "publisher" && role !== "viewer") || typeof room !== "string") {
      return null;
    }
    if (typeof connectedAt !== "number") {
      return null;
    }

    return { role, room, connectedAt };
  }

  private removeSocket(ws: WebSocket, attachment: SocketAttachment | null): void {
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
