"""Pont synchrone -> asynchrone : consomme un générateur asyncio depuis du code synchrone (Streamlit, scripts)."""
from __future__ import annotations

import asyncio
import queue
import threading
from typing import AsyncIterator, Iterator, TypeVar

T = TypeVar("T")
_END = object()


def iter_sync(agen: AsyncIterator[T]) -> Iterator[T]:
    """Itère un générateur asynchrone dans un thread dédié ; les éléments arrivent au fil de l'eau."""
    q: "queue.Queue" = queue.Queue()
    stop = threading.Event()

    def runner() -> None:
        async def pump() -> None:
            try:
                async for item in agen:
                    if stop.is_set():
                        break
                    q.put(item)
            except BaseException as e:      # noqa: BLE001 - relayé à l'appelant
                q.put(e)
            finally:
                q.put(_END)

        asyncio.run(pump())

    t = threading.Thread(target=runner, daemon=True)
    t.start()
    try:
        while True:
            item = q.get()
            if item is _END:
                return
            if isinstance(item, BaseException):
                raise item
            yield item
    finally:
        stop.set()
