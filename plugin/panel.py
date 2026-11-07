"""Panels (docs/protocol.md#panels): the agent's description, checked and shaped for the device.

The device draws a panel with LVGL: a title over a column of items. ``build`` turns the
``gadget_panel`` tool's arguments into the ``panel`` message, with the firmware's limits
applied and every string folded to what the panel fonts draw, and says what it changed
so the agent can learn the contract. The firmware applies the same limits again.
"""

from __future__ import annotations

import math
import re
import uuid
from typing import Any

from . import textfmt

VERSION = 1
MAX_ITEMS = 12
MAX_ENTRIES = 8
MAX_LIST_ROWS = 16
MAX_POINTS = 32
MAX_TEXT = 160
MAX_DETAIL = 320
MAX_ID = 48

ITEM_TYPES = ("text", "stats", "progress", "bars", "chart", "list", "buttons")
STATES = ("ok", "warn", "bad", "accent", "primary", "neutral")
_ID = re.compile(r"[^A-Za-z0-9_.:-]")


class PanelError(ValueError):
    """Nothing in the arguments could be drawn."""


def _text(value: Any, charset: str, limit: int = MAX_TEXT) -> str:
    if value is None or isinstance(value, bool):
        return ""
    if isinstance(value, float) and value.is_integer():
        value = int(value)
    text = textfmt.for_device(str(value), charset).replace("\n", " ") if limit <= MAX_TEXT \
        else textfmt.for_device(str(value), charset)
    return text[:limit]


def _id(value: Any) -> str:
    if value is None or isinstance(value, bool):
        return ""
    if isinstance(value, float) and value.is_integer():
        value = int(value)
    return _ID.sub("_", str(value).strip())[:MAX_ID]


def _number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value) if math.isfinite(value) else None
    if isinstance(value, str):
        try:
            n = float(value.replace(",", "."))
        except ValueError:
            return None
        return n if math.isfinite(n) else None
    return None


def _clean(d: dict) -> dict:
    return {k: v for k, v in d.items() if v not in ("", None, [])}


def _entries(item: dict, kind: str, charset: str, notes: list[str]) -> list[dict]:
    raw = item.get("items")
    if not isinstance(raw, list):
        raw = item.get("rows") if isinstance(item.get("rows"), list) else []
    limit = MAX_LIST_ROWS if kind == "list" else MAX_ENTRIES
    if len(raw) > limit:
        notes.append(f"{kind}: kept the first {limit} of {len(raw)} entries")
    out = []
    for e in raw[:limit]:
        if not isinstance(e, dict):
            continue
        entry = {"id": _id(e.get("id")), "label": _text(e.get("label"), charset),
                 "detail": _text(e.get("detail"), charset, MAX_DETAIL)}
        state = str(e.get("state") or e.get("style") or "").lower()
        if state in STATES:
            entry["state"] = state
        if kind == "bars":
            amount = _number(e.get("value"))
            if amount is None or amount < 0:
                notes.append(f'bars: "{entry["label"]}" needs a non-negative number as value; drawn as 0')
                amount = 0.0
            entry["value"] = int(amount) if amount.is_integer() else amount
            entry["text"] = _text(e.get("text"), charset)
        else:
            # The figure shown: text, or value written out.
            shown = e.get("text") if e.get("text") not in (None, "") else e.get("value")
            entry["text"] = _text(shown, charset)
        if kind == "buttons" and (not entry["id"] or not entry["label"]):
            notes.append("buttons: a button needs an id and a label; one was dropped")
            continue
        if not entry["label"] and not entry.get("text"):
            continue
        out.append(_clean(entry))
    return out


def _item(raw: dict, charset: str, notes: list[str]) -> dict | None:
    kind = str(raw.get("type") or "").lower()
    if kind not in ITEM_TYPES:
        notes.append(f"unknown item type {kind or '(none)'!r} dropped; use one of {', '.join(ITEM_TYPES)}")
        return None
    item: dict[str, Any] = {"type": kind, "id": _id(raw.get("id")), "label": _text(raw.get("label"), charset)}
    if kind == "text":
        item["text"] = _text(raw.get("text"), charset, MAX_DETAIL)
        style = str(raw.get("style") or "")
        if style in ("title", "dim"):
            item["style"] = style
        if not item["text"]:
            notes.append("text: an item without text was dropped")
            return None
    elif kind in ("stats", "bars", "list", "buttons"):
        item["items"] = _entries(raw, kind, charset, notes)
        if not item["items"]:
            notes.append(f"{kind}: no usable entries; dropped")
            return None
    elif kind == "progress":
        value, top = _number(raw.get("value")), _number(raw.get("max"))
        if value is None:
            notes.append("progress: value must be a number; drawn as 0")
            value = 0.0
        item["value"] = value
        if top is not None and top > 0:
            item["max"] = top
        item["text"] = _text(raw.get("text"), charset)
        if str(raw.get("style") or "") == "arc":
            item["style"] = "arc"
        state = str(raw.get("state") or "").lower()
        if state in STATES:
            item["state"] = state
    elif kind == "chart":
        values = [v for v in (_number(x) for x in (raw.get("values") or [])) if v is not None]
        if len(values) > MAX_POINTS:
            notes.append(f"chart: kept the last {MAX_POINTS} of {len(values)} values")
            values = values[-MAX_POINTS:]
        if len(values) < 2:
            notes.append("chart: needs at least 2 numeric values; dropped")
            return None
        item["values"] = values
        labels = raw.get("labels")
        if isinstance(labels, list) and len(labels) >= 2:
            item["labels"] = [_text(labels[0], charset)[:24], _text(labels[-1], charset)[:24]]
        if str(raw.get("style") or "") == "columns":
            item["style"] = "columns"
    return _clean(item)


def build(args: dict, charset: str = "latin1") -> tuple[dict, list[str]]:
    """The ``panel`` message for the tool's arguments, and notes on what was changed."""
    notes: list[str] = []
    raw_items = args.get("items")
    if not isinstance(raw_items, list) or not raw_items:
        raise PanelError("items is required: a list of panel items")
    if len(raw_items) > MAX_ITEMS:
        notes.append(f"kept the first {MAX_ITEMS} of {len(raw_items)} items")
    items = [i for i in (_item(r, charset, notes) for r in raw_items[:MAX_ITEMS] if isinstance(r, dict)) if i]
    if not items:
        raise PanelError("nothing to draw: " + "; ".join(notes) if notes else "nothing to draw")
    panel_id = _id(args.get("id")) or f"p-{uuid.uuid4().hex[:6]}"
    msg: dict[str, Any] = {"type": "panel", "id": panel_id, "title": _text(args.get("title"), charset),
                           "items": items}
    ttl = _number(args.get("seconds"))
    if ttl is not None and ttl > 0:
        msg["ttl_s"] = int(min(ttl, 86400))
    return _clean(msg), notes


def tap_text(data: Any, titles: dict[str, str]) -> str:
    """What the agent reads when the user taps a panel entry (a ``panel.tap`` event)."""
    data = data if isinstance(data, dict) else {}
    panel_id = str(data.get("panel") or "")
    label = str(data.get("label") or data.get("value") or "")
    title = titles.get(panel_id) or panel_id
    where = ", ".join(f"{k} {data[k]}" for k in ("panel", "item", "value") if data.get(k))
    return f'[Gadget panel] The user tapped "{label}" on the panel "{title}" ({where}).'
