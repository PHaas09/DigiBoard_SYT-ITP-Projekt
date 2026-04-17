from flask import Flask, request, jsonify
from pathlib import Path
from datetime import datetime
import json
import re

app = Flask(__name__)

BASE_DIR = Path(__file__).resolve().parent
SAVE_DIR = BASE_DIR / "saved_games"
SAVE_DIR.mkdir(parents=True, exist_ok=True)

LATEST_FILE = BASE_DIR / "latest_state.json"


def safe_token(value: str, default: str = "unknown") -> str:
    value = (value or "").strip()
    value = re.sub(r"[^A-Za-z0-9_.-]", "_", value)
    return value or default


def get_int(name: str, default: int = 0) -> int:
    raw = request.form.get(name, str(default))
    raw = raw.strip()
    try:
        return int(raw)
    except ValueError:
        return default


def get_char(name: str, default: str = " ") -> str:
    raw = (request.form.get(name, "") or "").strip()
    if not raw or raw in {"_", "-"}:
        return default
    return raw[0]


def get_board() -> list[str]:
    raw = (request.form.get("board", "") or "")[:9]
    raw = raw.ljust(9, "-")
    board = []
    for ch in raw:
        if ch in {"X", "O"}:
            board.append(ch)
        else:
            board.append(" ")
    return board


def board_as_text(board: list[str]) -> str:
    row1 = f" {board[0]} | {board[1]} | {board[2]} "
    row2 = f" {board[3]} | {board[4]} | {board[5]} "
    row3 = f" {board[6]} | {board[7]} | {board[8]} "
    sep = "\n---+---+---\n"
    return row1 + sep + row2 + sep + row3


def mode_name(mode: int) -> str:
    return {
        0: "LOCAL_2P",
        1: "AI_EASY",
        2: "AI_HARD",
        3: "DUO",
    }.get(mode, "UNKNOWN")


def screen_name(mode: int) -> str:
    return {
        0: "GAME",
        1: "SETTINGS",
        2: "AI_START",
        3: "DUO_WAIT",
    }.get(mode, "UNKNOWN")


def build_payload() -> dict:
    board = get_board()
    payload = {
        "receivedAt": datetime.now().isoformat(timespec="seconds"),
        "role": safe_token(request.form.get("role", "UNKNOWN")),
        "deviceId": safe_token(request.form.get("deviceId", "UNKNOWN")),
        "sessionId": safe_token(request.form.get("sessionId", "0")),
        "gameId": get_int("game_id", 0),
        "eventIndex": get_int("eventIndex", 0),
        "seq": get_int("seq", 0),
        "gameMode": get_int("gameMode", 0),
        "screenMode": get_int("screenMode", 0),
        "currentPlayer": get_char("currentPlayer", " "),
        "gameOver": get_int("gameOver", 0),
        "winner": get_char("winner", " "),
        "duoConnected": get_int("duoConnected", 0),
        "localSymbol": get_char("localSymbol", " "),
        "remoteSymbol": get_char("remoteSymbol", " "),
        "event": safe_token(request.form.get("event", "unknown")),
        "channel": get_int("channel", 0),
        "uptimeMs": get_int("uptimeMs", 0),
        "board": board,
        "boardWire": request.form.get("board", ""),
    }
    return payload


def game_filename(payload: dict) -> str:
    return f"game_{payload['gameId']:010d}.txt"


def write_latest(payload: dict) -> None:
    with LATEST_FILE.open("w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)


def append_game_snapshot(payload: dict) -> Path:
    filename = game_filename(payload)
    path = SAVE_DIR / filename
    is_new = not path.exists()

    with path.open("a", encoding="utf-8") as f:
        if is_new:
            f.write("TicTacToe Saved Game\n")
            f.write("=" * 70 + "\n")
            f.write(f"gameId     : {payload['gameId']}\n")
            f.write("=" * 70 + "\n\n")

        f.write(f"[{payload['receivedAt']}] ")
        f.write(f"deviceId={payload['deviceId']} ")
        f.write(f"sessionId={payload['sessionId']} ")
        f.write(f"role={payload['role']} ")
        f.write(f"eventIndex={payload['eventIndex']} ")
        f.write(f"event={payload['event']} ")
        f.write(f"seq={payload['seq']} ")
        f.write(f"screen={screen_name(payload['screenMode'])} ")
        f.write(f"currentPlayer={payload['currentPlayer']} ")
        f.write(f"gameOver={payload['gameOver']} ")
        f.write(f"winner={payload['winner']} ")
        f.write(f"duoConnected={payload['duoConnected']} ")
        f.write(f"local={payload['localSymbol']} ")
        f.write(f"remote={payload['remoteSymbol']} ")
        f.write(f"channel={payload['channel']} ")
        f.write(f"uptimeMs={payload['uptimeMs']}\n")

        f.write(board_as_text(payload["board"]))
        f.write("\n")
        f.write("-" * 70)
        f.write("\n")

    return path


@app.get("/")
def root():
    return jsonify({
        "ok": True,
        "message": "Flask TicTacToe backend running",
        "routes": ["/push (POST)", "/latest (GET)", "/state (GET)", "/games (GET)"]
    })


@app.post("/push")
def push():
    print("\n=== PUSH RECEIVED ===")
    print(dict(request.form))

    payload = build_payload()
    write_latest(payload)

    if payload["gameId"] <= 0:
        return jsonify({
            "ok": True,
            "stored": False,
            "reason": "gameId <= 0",
            "state": payload
        })

    path = append_game_snapshot(payload)

    return jsonify({
        "ok": True,
        "stored": True,
        "file": path.name,
        "gameId": payload["gameId"],
        "eventIndex": payload["eventIndex"],
        "state": payload
    })


@app.get("/latest")
def latest():
    if not LATEST_FILE.exists():
        return jsonify({"ok": True, "have": False})

    with LATEST_FILE.open("r", encoding="utf-8") as f:
        data = json.load(f)

    return jsonify({
        "ok": True,
        "have": True,
        "state": data,
    })


@app.get("/state")
def state():
    if not LATEST_FILE.exists():
        return jsonify({"have": False})

    with LATEST_FILE.open("r", encoding="utf-8") as f:
        data = json.load(f)

    return jsonify({
        "have": True,
        "gameId": data.get("gameId", 0),
        "seq": data.get("seq", 0),
        "gameMode": data.get("gameMode", 0),
        "screenMode": data.get("screenMode", 0),
        "currentPlayer": data.get("currentPlayer", " "),
        "gameOver": bool(data.get("gameOver", 0)),
        "winner": data.get("winner", " "),
        "duoConnected": bool(data.get("duoConnected", 0)),
        "localSymbol": data.get("localSymbol", " "),
        "remoteSymbol": data.get("remoteSymbol", " "),
        "channel": data.get("channel", 0),
        "role": data.get("role", "UNKNOWN"),
        "event": data.get("event", "unknown"),
        "lastUpdateMs": data.get("uptimeMs", 0),
        "board": data.get("board", [" "] * 9),
    })


@app.get("/games")
def games():
    files = sorted([p.name for p in SAVE_DIR.glob("*.txt")], reverse=True)
    return jsonify({
        "ok": True,
        "count": len(files),
        "files": files,
    })


if __name__ == "__main__":
    print("Starting Flask on 0.0.0.0:5000")
    app.run(host="0.0.0.0", port=5000, debug=True)