"""Faux serveur LLM (HTTP/1.1, transfert en morceaux) pour tester le streaming sans clé API.

Lancement manuel :  python3 tests/fake_server.py 8765
Le comportement dépend du nom du modèle : "err429" -> HTTP 429, "nostream" -> JSON simple,
"agent" -> joue un scénario (ACTION list_files puis DONE), sinon flux de démonstration.
"""
from __future__ import annotations

import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DELAY = 0.12


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):  # silence
        pass

    def _chunk(self, s: str) -> None:
        b = s.encode()
        self.wfile.write(f"{len(b):x}\r\n".encode() + b + b"\r\n")
        self.wfile.flush()

    def do_POST(self):
        try:
            self._handle()
        except (BrokenPipeError, ConnectionResetError):
            pass                                   # le client a annulé en plein flux

    def _handle(self):
        n = int(self.headers.get("content-length", 0))
        req = json.loads(self.rfile.read(n) or b"{}")
        anth = self.path.endswith("/messages")
        model = req.get("model", "")
        user = ""
        for m in req.get("messages", []):
            if m.get("role") == "user":
                user = m.get("content", "")
        if self.path.endswith("/api/chat"):                       # Ollama natif : NDJSON + champ "thinking"
            self.send_response(200)
            self.send_header("Content-Type", "application/x-ndjson")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for t in ("Je ", "réfléchis"):
                self._chunk(json.dumps({"message": {"role": "assistant", "content": "", "thinking": t}, "done": False}) + "\n")
                time.sleep(DELAY)
            for t in ("Bon", "jour"):
                self._chunk(json.dumps({"message": {"role": "assistant", "content": t}, "done": False}) + "\n")
                time.sleep(DELAY)
            self._chunk(json.dumps({"message": {"role": "assistant", "content": ""}, "done": True}) + "\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            return
        if model in ("tools", "notools") and req.get("stream"):
            if model == "notools" and req.get("tools"):             # fournisseur sans function calling
                body = json.dumps({"error": {"message": "tools are not supported"}}).encode()
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            has_obs = any("OBSERVATION" in str(m.get("content", "")) for m in req.get("messages", []))
            if model == "notools":                                  # repli texte : protocole ACTION / DONE
                outs = ["DONE texte ok"] if has_obs else ["ACTION list_files ."]
                for t in outs:
                    self._chunk("data: " + json.dumps({"choices": [{"delta": {"content": t}}]}) + "\n\n")
                self._chunk("data: [DONE]\n\n")
            elif has_obs:
                self._chunk("data: " + json.dumps({"choices": [{"delta": {"content": "fini"}}]}) + "\n\n")
                self._chunk("data: [DONE]\n\n")
            else:                                                   # appel d'outil en morceaux, comme OpenAI
                tc = lambda **k: "data: " + json.dumps({"choices": [{"delta": {"tool_calls": [k]}}]}) + "\n\n"
                self._chunk(tc(index=0, id="c1", type="function", function={"name": "list_files", "arguments": ""}))
                self._chunk(tc(index=0, function={"arguments": '{"pa'}))
                self._chunk(tc(index=0, function={"arguments": 'th": "."}'}))
                self._chunk("data: " + json.dumps({"choices": [{"delta": {}, "finish_reason": "tool_calls"}]}) + "\n\n")
                self._chunk("data: [DONE]\n\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            return
        if model == "err429":
            body = json.dumps({"error": {"message": "limite de débit"}}).encode()
            self.send_response(429)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if model == "nostream" or not req.get("stream"):
            txt = "Réponse complète sans flux"
            obj = ({"content": [{"type": "text", "text": txt}]} if anth
                   else {"choices": [{"message": {"content": txt}}]})
            body = json.dumps(obj).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        if model == "agent":
            if "OBSERVATION" in user:
                think, parts = ["Les fichiers sont listés. "], ["DONE ", "Terminé : ", "projet analysé."]
            else:
                think, parts = ["Je vais lister les fichiers. "], ["ACTION ", "list_files ", "."]
        else:
            think, parts = ["Analyse ", "du besoin… "], ["Salut ", "depuis ", "le ", "flux ✔"]
        if anth:
            for t in think:
                self._chunk("event: content_block_delta\ndata: " + json.dumps(
                    {"type": "content_block_delta", "delta": {"type": "thinking_delta", "thinking": t}}) + "\n\n")
                time.sleep(DELAY)
            for t in parts:
                self._chunk("event: content_block_delta\ndata: " + json.dumps(
                    {"type": "content_block_delta", "delta": {"type": "text_delta", "text": t}}) + "\n\n")
                time.sleep(DELAY)
            self._chunk('event: message_stop\ndata: {"type":"message_stop"}\n\n')
        else:
            for t in think:
                self._chunk("data: " + json.dumps({"choices": [{"delta": {"reasoning_content": t}}]}) + "\n\n")
                time.sleep(DELAY)
            for t in parts:
                self._chunk("data: " + json.dumps({"choices": [{"delta": {"content": t}}]}) + "\n\n")
                time.sleep(DELAY)
            self._chunk("data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


def start(port: int = 0):
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


if __name__ == "__main__":
    s = start(int(sys.argv[1]) if len(sys.argv) > 1 else 8765)
    print("faux serveur sur", s.server_address)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
