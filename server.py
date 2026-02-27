import asyncio
import contextlib
import time
from typing import Dict, Optional

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

try:
    import uvloop  # type: ignore
except Exception:
    uvloop = None


HOST = "0.0.0.0"
PORT = 8081
VIEWER_QUEUE_SIZE = 1
STATS_INTERVAL_SEC = 5.0
SEND_TIMEOUT_SEC = 0.25

viewer_queues: Dict[ServerConnection, asyncio.Queue[bytes]] = {}
camera_ws: Optional[ServerConnection] = None
camera_lock = asyncio.Lock()

stats_started_at = time.monotonic()
stats_frames = 0
stats_bytes = 0
stats_dropped = 0


def report_stats(force: bool = False) -> None:
    global stats_started_at, stats_frames, stats_bytes, stats_dropped

    now = time.monotonic()
    elapsed = now - stats_started_at
    if elapsed <= 0:
        return
    if not force and elapsed < STATS_INTERVAL_SEC:
        return

    fps = stats_frames / elapsed
    mbps = (stats_bytes * 8.0) / (elapsed * 1_000_000.0)
    avg_kb = (stats_bytes / stats_frames / 1024.0) if stats_frames else 0.0
    print(
        f"[STAT] {elapsed:.1f}s | in_fps={fps:.2f}, in_mbps={mbps:.2f}, "
        f"avg_frame={avg_kb:.1f}KB, dropped={stats_dropped}, viewers={len(viewer_queues)}"
    )

    stats_started_at = now
    stats_frames = 0
    stats_bytes = 0
    stats_dropped = 0


async def viewer_sender(websocket: ServerConnection, queue: asyncio.Queue[bytes]) -> None:
    try:
        while True:
            frame = await queue.get()
            await asyncio.wait_for(websocket.send(frame), timeout=SEND_TIMEOUT_SEC)
    except (asyncio.TimeoutError, ConnectionClosed):
        with contextlib.suppress(Exception):
            await websocket.close(code=1011, reason="viewer_slow_or_closed")
    except asyncio.CancelledError:
        raise
    except Exception:
        with contextlib.suppress(Exception):
            await websocket.close(code=1011, reason="viewer_send_error")


async def handle_camera(websocket: ServerConnection) -> None:
    global camera_ws, stats_frames, stats_bytes, stats_dropped

    async with camera_lock:
        if camera_ws is not None and camera_ws != websocket:
            print("[CAM] 新推流连接到来，关闭旧连接")
            with contextlib.suppress(Exception):
                await camera_ws.close(code=1012, reason="camera_replaced")
        camera_ws = websocket

    print("[CAM] 摄像头已连接，开始中转视频")
    try:
        async for message in websocket:
            if not isinstance(message, (bytes, bytearray)):
                continue

            frame = bytes(message)
            stats_frames += 1
            stats_bytes += len(frame)

            if viewer_queues:
                dropped_this_frame = 0
                for queue in tuple(viewer_queues.values()):
                    if queue.full():
                        with contextlib.suppress(asyncio.QueueEmpty):
                            queue.get_nowait()
                        dropped_this_frame += 1
                    with contextlib.suppress(asyncio.QueueFull):
                        queue.put_nowait(frame)
                stats_dropped += dropped_this_frame

            report_stats(force=False)
    except ConnectionClosed:
        print("[CAM] 摄像头连接断开")
    finally:
        report_stats(force=True)
        async with camera_lock:
            if camera_ws == websocket:
                camera_ws = None


async def handle_viewer(websocket: ServerConnection) -> None:
    queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=VIEWER_QUEUE_SIZE)
    viewer_queues[websocket] = queue
    sender_task = asyncio.create_task(viewer_sender(websocket, queue))

    print(f"[VIEW] 观众上线，当前人数: {len(viewer_queues)}")
    try:
        await websocket.wait_closed()
    finally:
        sender_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await sender_task
        viewer_queues.pop(websocket, None)
        print(f"[VIEW] 观众离线，当前人数: {len(viewer_queues)}")


async def video_relay(websocket: ServerConnection) -> None:
    path = websocket.request.path
    if path == "/esp32":
        await handle_camera(websocket)
        return

    if path == "/viewer":
        await handle_viewer(websocket)
        return

    print(f"[WARN] 拒绝未知路径: {path}")
    await websocket.close(code=1008, reason="invalid_path")


async def main() -> None:
    print(f"[BOOT] 视频中转服务启动: ws://{HOST}:{PORT}")
    print("[BOOT] 配置: multi-viewer, compression=off, max_queue=1, viewer_queue=1")
    async with serve(
        video_relay,
        HOST,
        PORT,
        compression=None,
        max_queue=1,
        max_size=1024 * 1024,
        write_limit=1024 * 1024,
        ping_interval=20,
        ping_timeout=20,
        close_timeout=3,
    ):
        await asyncio.Future()


if __name__ == "__main__":
    if uvloop is not None:
        uvloop.install()
        print("[BOOT] uvloop 已启用")
    else:
        print("[BOOT] uvloop 不可用，使用默认 asyncio")

    asyncio.run(main())
