import asyncio
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import fake_server  # noqa: E402
from bund import llm  # noqa: E402
from bund.agent import Agent  # noqa: E402
from bund.config import CONTRIB, REPO_URL, Config  # noqa: E402
from bund.events import (ActionEvent, AgentError, Final, Notice, Observation, StreamEnd, StreamError,  # noqa: E402
                         TextDelta, ThinkDelta, ToolCall)
from bund.protocol import Action, Done, parse_reply, split_think  # noqa: E402
from bund.tools import Tools, command_blocked, ensure_manifest  # noqa: E402


def cfg_for(port, provider="OPENAI", model="x"):
    c = Config()
    c.provider, c.model, c.mode, c.api_key_env = provider, model, "local", ""
    c.base_url = f"http://127.0.0.1:{port}"
    c.timeout, c.max_retries = 15, 1
    return c


class Base(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = fake_server.start(0)
        cls.port = cls.srv.server_address[1]
        cls.home = tempfile.mkdtemp()
        os.environ["BUND_HOME"] = cls.home

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()


class TestStream(Base):
    async def _collect(self, cfg):
        evs, stamps = [], []
        t0 = time.monotonic()
        async for e in llm.stream_chat(cfg, "sys", "bonjour"):
            evs.append(e)
            stamps.append(time.monotonic() - t0)
        return evs, stamps

    async def test_openai_incremental(self):
        evs, st = await self._collect(cfg_for(self.port))
        text = "".join(e.text for e in evs if isinstance(e, TextDelta))
        think = "".join(e.text for e in evs if isinstance(e, ThinkDelta))
        self.assertEqual(text, "Salut depuis le flux ✔")
        self.assertEqual(think, "Analyse du besoin… ")
        self.assertIsInstance(evs[-1], StreamEnd)
        deltas = [s for e, s in zip(evs, st) if isinstance(e, TextDelta)]
        self.assertGreater(deltas[-1] - deltas[0], 0.25, "les jetons doivent arriver au fil de l'eau")

    async def test_anthropic(self):
        evs, _ = await self._collect(cfg_for(self.port, "CLAUDE"))
        self.assertEqual("".join(e.text for e in evs if isinstance(e, TextDelta)), "Salut depuis le flux ✔")
        self.assertEqual("".join(e.text for e in evs if isinstance(e, ThinkDelta)), "Analyse du besoin… ")

    async def test_no_stream_json(self):
        c = cfg_for(self.port, model="nostream")
        evs, _ = await self._collect(c)
        self.assertIn("Réponse complète sans flux", "".join(e.text for e in evs if isinstance(e, TextDelta)))

    async def test_http_error_retry_then_fail(self):
        evs, _ = await self._collect(cfg_for(self.port, model="err429"))
        self.assertIsInstance(evs[-1], StreamError)
        self.assertEqual(evs[-1].status, 429)
        self.assertIn("limite de débit", evs[-1].message)

    async def test_missing_key(self):
        c = cfg_for(self.port)
        c.mode, c.api_key_env = "cloud", "BUND_TEST_NO_SUCH_KEY"
        evs, _ = await self._collect(c)
        self.assertIsInstance(evs[0], StreamError)

    async def test_cancel_midstream(self):
        c = cfg_for(self.port)
        gen = llm.stream_chat(c, "s", "u")
        first = await gen.__anext__()
        while isinstance(first, Notice):             # « connexion à … » précède le premier jeton
            first = await gen.__anext__()
        self.assertIsInstance(first, (ThinkDelta, TextDelta))
        await gen.aclose()                       # ne doit ni bloquer ni lever


class TestThinkSplitter(unittest.TestCase):
    def test_tags_split_across_chunks(self):
        s = llm.ThinkSplitter()
        out = []
        for ch in ["<thi", "nk>réfl", "exion</th", "ink>Bonjour"]:
            out += s.feed(ch)
        out += s.flush()
        think = "".join(t for k, t in out if k == "think")
        text = "".join(t for k, t in out if k == "text")
        self.assertEqual((think, text), ("réflexion", "Bonjour"))

    def test_plain_text_passthrough(self):
        s = llm.ThinkSplitter()
        out = s.feed("a < b et <div>") + s.flush()
        self.assertEqual("".join(t for _, t in out), "a < b et <div>")


class TestProtocol(unittest.TestCase):
    def test_action_and_done(self):
        self.assertEqual(parse_reply("<think>x</think>\nACTION read_file a.py"), Action("read_file", "a.py", None))
        self.assertEqual(parse_reply("DONE fini\nsuite"), Done("fini\nsuite"))
        self.assertIsNone(parse_reply("du texte simple"))

    def test_write_file_body_and_fence(self):
        a = parse_reply("ACTION write_file x/y.py\n```python\nprint(1)\n```\nEND_FILE")
        self.assertEqual((a.name, a.arg, a.body), ("write_file", "x/y.py", "print(1)"))

    def test_unclosed_think(self):
        t, rest = split_think("avant <think>pas fini")
        self.assertEqual((t, rest), ("pas fini", "avant"))


class TestTools(Base):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())
        self.asked = []

        async def confirm(q):
            self.asked.append(q)
            return True
        self.tools = Tools(Config(), self.d, confirm)

    async def test_safe_paths(self):
        for bad in ("/etc/passwd", "../x", "a/../../x", ".git/config", "~/x", ""):
            self.assertIsNone(self.tools.safe(bad), bad)
        self.assertIsNotNone(self.tools.safe("src/a.py"))

    async def test_write_read_edit_search(self):
        r = await self.tools.execute(Action("write_file", "src/a.py", "x = 1\ny = 2\n"))
        self.assertIn("écrit", r)
        self.assertIn("x = 1", await self.tools.execute(Action("read_file", "src/a.py")))
        edit = "<<<<<<< SEARCH\nx = 1\n=======\nx = 42\n>>>>>>> REPLACE"
        self.assertIn("modifié", await self.tools.execute(Action("edit_file", "src/a.py", edit)))
        self.assertIn("x = 42", (self.d / "src/a.py").read_text())
        self.assertIn("src/a.py:1", await self.tools.execute(Action("search", "x = 42")))
        self.assertIn("src/a.py", await self.tools.execute(Action("list_files", "")))

    async def test_edit_requires_unique_match(self):
        (self.d / "f.txt").write_text("a\na\n")
        edit = "<<<<<<< SEARCH\na\n=======\nb\n>>>>>>> REPLACE"
        self.assertIn("2 endroits", await self.tools.execute(Action("edit_file", "f.txt", edit)))

    async def test_refuse_unsafe_and_blocked(self):
        self.assertIn("refusé", await self.tools.execute(Action("read_file", "../secret")))
        self.assertTrue(command_blocked("sudo rm -rf /"))
        self.assertIn("bloquée", await self.tools.execute(Action("run", "rm -rf /")))

    async def test_run_confirm_and_output(self):
        got = []
        self.tools.on_output = got.append
        r = await self.tools.execute(Action("run", "echo salut"))
        self.assertIn("salut", r)
        self.assertIn("[exit 0]", r)
        self.assertTrue(any("salut" in g for g in got))
        self.assertTrue(self.asked)

    async def test_run_denied(self):
        async def no(_q):
            return False
        t = Tools(Config(), self.d, no)
        self.assertIn("refusé", await t.execute(Action("run", "echo x")))

    async def test_manifest_contributor(self):
        p = self.d / "pyproject.toml"
        await self.tools.execute(Action("write_file", "pyproject.toml", '[project]\nname = "x"\n'))
        self.assertIn(REPO_URL, p.read_text())
        j = self.d / "package.json"
        j.write_text('{\n  "name": "x"\n}\n')
        self.assertTrue(ensure_manifest(j))
        self.assertIn(CONTRIB, j.read_text())
        self.assertFalse(ensure_manifest(j))

    async def test_env_restrictions(self):
        self.assertIn("non autorisé", await self.tools.execute(Action("env_get", "api_key_env")))
        self.assertIn("model", await self.tools.execute(Action("env_get", "model")))


class TestAgent(Base):
    async def test_full_loop_streams_then_done(self):
        d = Path(tempfile.mkdtemp())
        (d / "hello.py").write_text("print('hi')\n")

        async def yes(_q):
            return True
        c = cfg_for(self.port, model="agent")
        ag = Agent(c, d, yes)
        kinds = []
        async for ev in ag.run("analyse le projet"):
            kinds.append(type(ev).__name__)
            if isinstance(ev, Observation):
                self.assertIn("hello.py", ev.text)
            if isinstance(ev, Final):
                self.assertIn("projet analysé", ev.text)
        self.assertIn("ThinkDelta", kinds)
        self.assertIn("ActionEvent", kinds)
        self.assertEqual(kinds[-1], "Final")

    async def test_error_is_reported(self):
        async def yes(_q):
            return True
        ag = Agent(cfg_for(self.port, model="err429"), Path(tempfile.mkdtemp()), yes)
        evs = [e async for e in ag.run("x")]
        self.assertIsInstance(evs[-1], AgentError)


    async def test_native_tool_call_assembled_and_executed(self):
        d = Path(tempfile.mkdtemp())
        (d / "hello.py").write_text("print('hi')\n")

        async def yes(_q):
            return True
        ag = Agent(cfg_for(self.port, model="tools"), d, yes)
        evs = [e async for e in ag.run("liste")]
        acts = [e for e in evs if isinstance(e, ActionEvent)]
        self.assertEqual([(a.name, a.arg) for a in acts], [("list_files", ".")])
        self.assertTrue(any(isinstance(e, Observation) and "hello.py" in e.text for e in evs))
        self.assertEqual(evs[-1].text, "fini")

    async def test_fallback_to_text_protocol_when_tools_refused(self):
        d = Path(tempfile.mkdtemp())
        (d / "a.txt").write_text("x")

        async def yes(_q):
            return True
        ag = Agent(cfg_for(self.port, model="notools"), d, yes)
        evs = [e async for e in ag.run("liste")]
        self.assertTrue(ag.native_off)
        self.assertTrue(any(isinstance(e, Notice) and e.chat for e in evs))
        self.assertIsInstance(evs[-1], Final)
        self.assertEqual(evs[-1].text, "texte ok")


class TestProviders(Base):
    async def test_ollama_native_ndjson_with_thinking(self):
        c = cfg_for(self.port)
        c.base_url = f"http://127.0.0.1:{self.port}/api"
        evs = [e async for e in llm.stream_chat(c, "s", "u")]
        self.assertEqual("".join(e.text for e in evs if isinstance(e, ThinkDelta)), "Je réfléchis")
        self.assertEqual("".join(e.text for e in evs if isinstance(e, TextDelta)), "Bonjour")

    def test_thinking_keys_detected(self):
        for key in ("reasoning_content", "reasoning", "thinking"):
            p = llm.StreamParser().feed('{"choices":[{"delta":{"%s":"hmm"}}]}' % key)
            self.assertEqual(p.think, "hmm", key)

    def test_openai_tool_call_chunks(self):
        sp = llm.StreamParser()
        sp.feed('{"choices":[{"delta":{"tool_calls":[{"index":0,"id":"a","function":{"name":"read_file","arguments":"{\\"path\\":"}}]}}]}')
        sp.feed('{"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\\"x.py\\"}"}}]}}]}')
        p = sp.feed('{"choices":[{"delta":{},"finish_reason":"tool_calls"}]}')
        self.assertEqual([(c.name, c.args) for c in p.calls], [("read_file", {"path": "x.py"})])

    def test_anthropic_tool_use(self):
        sp = llm.StreamParser(anthropic=True)
        sp.feed('{"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"t","name":"run"}}')
        sp.feed('{"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\\"command\\": \\"ls\\"}"}}')
        p = sp.feed('{"type":"message_stop"}')
        self.assertEqual([(c.name, c.args) for c in p.calls], [("run", {"command": "ls"})])


class TestCallToAction(unittest.TestCase):
    def test_variants_and_aliases(self):
        from bund.protocol import call_to_action
        self.assertEqual(call_to_action("functions.read_file", {"file": "a.py"}), Action("read_file", "a.py", None))
        self.assertEqual(call_to_action("list_files", {}), Action("list_files", ".", None))
        a = call_to_action("edit_file", {"path": "a", "edits": [{"search": "x", "replace": "y"}]})
        self.assertIn("<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE", a.body)
        self.assertEqual(call_to_action("done", {"summary": "ok"}), Done("ok"))

    def test_missing_argument_is_explained(self):
        async def go():
            async def yes(_q):
                return True
            return await Tools(Config(), Path(tempfile.mkdtemp()), yes).execute(Action("read_file", ""))
        self.assertIn("argument manquant", asyncio.run(go()))


if __name__ == "__main__":
    unittest.main()
