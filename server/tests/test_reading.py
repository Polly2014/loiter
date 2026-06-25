"""P4a reading 引擎单测 —— 不依赖 CopilotX（ENABLED=false 走 fallback），纯逻辑验证。"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["LOITER_NPC_ENABLED"] = "false"   # 强制 fallback，离线可测

from loiter.islands import reading


def test_fallback_shape():
    # ENABLED=false → 每岛返回结构正确的双语 fallback
    for idx in range(6):
        r = reading.generate_reading("ALICE", idx, ["#e84d3c"], 3, "warm, curious, loves the sea")
        assert "title" in r and "lines" in r
        assert isinstance(r["lines"], list) and len(r["lines"]) == 9
        assert r["title"], "title 非空"
        # 双语字段完整
        assert r["title_cn"], "中文标题非空"
        assert r["core_cn"], "核心句非空"
        assert isinstance(r["lines_cn"], list) and len(r["lines_cn"]) == 9
        # 中文字数不超限（防 240×135 溢出）
        assert len(r["title_cn"]) <= reading._CN_TITLE_MAX
        assert len(r["core_cn"]) <= reading._CN_CORE_MAX
        for ln in r["lines_cn"]:
            assert len(ln) <= reading._CN_LINE_MAX


def test_parse_valid():
    out = reading._parse(
        '{"title": "QUIET TIDE", "title_cn": "潮", "core_cn": "你像海一样轻",'
        ' "lines": ["a","b","c","d","e","f","g","h","i"],'
        ' "lines_cn": ["一","二","三","四","五","六","七","八","九"]}')
    assert out["title"] == "QUIET TIDE"
    assert len(out["lines"]) == 9 and out["lines"][0] == "a"
    assert out["title_cn"] == "潮"
    assert out["core_cn"] == "你像海一样轻"
    assert len(out["lines_cn"]) == 9 and out["lines_cn"][8] == "九"


def test_parse_cn_truncation_and_backfill():
    # 超长中文 → 截断；lines_cn 不足 9 行 → 补空
    long_core = "一二三四五六七八九十十一十二十三"  # 15 字 > 12
    out = reading._parse(
        '{"title": "X", "title_cn": "一二三四五六七八", "core_cn": "' + long_core + '",'
        ' "lines": ["a"], "lines_cn": ["只有两行", "第二行"]}')
    assert len(out["title_cn"]) == reading._CN_TITLE_MAX, "标题截到 6 字"
    assert len(out["core_cn"]) == reading._CN_CORE_MAX, "核心句截到 12 字"
    assert len(out["lines"]) == 9, "英文补齐到 9 行"
    assert len(out["lines_cn"]) == 9, "中文补齐到 9 行"
    assert out["lines_cn"][2:] == [""] * 7, "不足部分补空串"


def test_parse_markdown_fence():
    out = reading._parse('```json\n{"title": "X", "lines": ["a"]}\n```')
    assert out["title"] == "X"
    assert len(out["lines"]) == 9 and out["lines"][0] == "a", "不足 9 行补空"
    # 缺 CN 字段 → 空串 / 空列补齐（不报错，向后兼容）
    assert out["title_cn"] == "" and out["core_cn"] == ""
    assert out["lines_cn"] == [""] * 9


def test_parse_garbage():
    assert reading._parse("not json at all") is None
    assert reading._parse('{"title": "X"}') is None, "缺 lines → None"


def test_spectrum_summary():
    home = "#6ab04c"  # GROVE
    # 只有本色 → resonance 描述
    assert "resonance" in reading._spectrum_summary([home], home)
    # 收了 EMBER 红 → 提到 EMBER
    s = reading._spectrum_summary([home, "#e84d3c"], home)
    assert "EMBER" in s


def test_classify_island_hash_fallback():
    # B′：NPC 关 → classify_island 走 hash(text)%6，确定性 + 在范围内 + 同文本同岛
    from loiter.islands.assignment import ISLANDS
    for t in ("warm and curious", "我喜欢安静", "", "🌈 fierce"):
        a = reading.classify_island(t)
        assert 0 <= a < len(ISLANDS)
        assert a == reading.classify_island(t) == reading._hash_island(t), t


def test_classify_island_httpx_missing_falls_back(monkeypatch):
    # Codex P1：httpx 缺失（optional dep 未装）+ NPC 开 → 必须走 hash 兜底、不抛，
    # 否则 /flash/profile 会 500（classify 在 create 前被 await）。
    import sys
    monkeypatch.setattr(reading, "ENABLED", True)
    monkeypatch.setitem(sys.modules, "httpx", None)  # import httpx → ImportError
    out = reading.classify_island("fiery and bright")
    assert out == reading._hash_island("fiery and bright")


if __name__ == "__main__":
    test_fallback_shape()
    test_parse_valid()
    test_parse_cn_truncation_and_backfill()
    test_parse_markdown_fence()
    test_parse_garbage()
    test_spectrum_summary()
    test_classify_island_hash_fallback()
    print("✅ reading 引擎单测通过")
