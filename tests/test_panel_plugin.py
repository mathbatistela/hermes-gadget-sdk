"""Panels on the plugin side (plugin/panel.py): the tool's arguments shaped for the firmware."""

import pytest

from hermes_gadget_plugin import panel


def test_a_panel_is_folded_to_the_panel_fonts_and_cleaned():
    msg, notes = panel.build({
        "title": "Orçamento — **nov**",
        "id": "budget nov",
        "seconds": 60,
        "items": [
            {"type": "progress", "style": "arc", "value": "3401", "max": 5500, "text": "62%", "label": "do mês"},
            {"type": "stats", "items": [{"label": "Gasto", "value": "R$ 3.401"}, {"label": "Dias", "value": 7}]},
            {"type": "bars", "id": "cat", "items": [{"id": "mercado", "label": "Mercado 🛒", "value": 1210,
                                                    "text": "R$ 1.210", "detail": "14 compras"}]},
            {"type": "buttons", "items": [{"id": "go", "label": "Ir", "state": "primary"}]},
        ],
    })
    assert notes == []
    assert msg["type"] == "panel" and msg["id"] == "budget_nov" and msg["ttl_s"] == 60
    assert msg["title"] == "Orçamento - nov"  # Markdown out, the dash folded, the accent kept
    arc, stats, bars, buttons = msg["items"]
    assert arc == {"type": "progress", "value": 3401.0, "max": 5500.0, "text": "62%", "label": "do mês", "style": "arc"}
    assert stats["items"] == [{"label": "Gasto", "text": "R$ 3.401"}, {"label": "Dias", "text": "7"}]
    assert bars["items"][0] == {"id": "mercado", "label": "Mercado", "value": 1210, "text": "R$ 1.210",
                                "detail": "14 compras"}
    assert buttons["items"] == [{"id": "go", "label": "Ir", "state": "primary"}]


def test_what_was_changed_comes_back_as_notes():
    msg, notes = panel.build({"items": [
        {"type": "hologram"},
        {"type": "chart", "values": [1]},
        {"type": "buttons", "items": [{"label": "no id"}, {"id": "ok", "label": "OK"}]},
        {"type": "bars", "items": [{"label": f"b{i}", "value": i} for i in range(10)]},
    ]})
    assert [i["type"] for i in msg["items"]] == ["buttons", "bars"]
    assert msg["id"].startswith("p-")
    assert len(msg["items"][1]["items"]) == panel.MAX_ENTRIES
    joined = " | ".join(notes)
    assert "unknown item type 'hologram'" in joined
    assert "chart: needs at least 2" in joined
    assert "a button needs an id and a label" in joined
    assert "kept the first 8 of 10" in joined


def test_nothing_to_draw_is_an_error():
    with pytest.raises(panel.PanelError, match="items is required"):
        panel.build({"title": "x"})
    with pytest.raises(panel.PanelError, match="nothing to draw"):
        panel.build({"items": [{"type": "text"}]})


def test_a_tap_reads_as_a_sentence_for_the_agent():
    text = panel.tap_text({"panel": "p1", "item": "cat", "value": "mercado", "label": "Mercado"}, {"p1": "Orçamento"})
    assert text == '[Gadget panel] The user tapped "Mercado" on the panel "Orçamento" (panel p1, item cat, value mercado).'
    assert panel.tap_text({"panel": "x", "value": "v"}, {}).startswith('[Gadget panel] The user tapped "v" on the panel "x"')
