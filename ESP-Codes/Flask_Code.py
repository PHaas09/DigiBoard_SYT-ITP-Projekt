from flask import Flask, request, jsonify
from pathlib import Path
from datetime import datetime
import json
import re

app = Flask(__name__)

BASE_DIR = Path(__file__).resolve().parent
SAVE_DIR = BASE_DIR / "saved_games"
SAVE_DIR.mkdir(parents=True, exist_ok=True)

DATA_DIR = BASE_DIR / "saved_games_data"
DATA_DIR.mkdir(parents=True, exist_ok=True)

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


def get_board_wire() -> str:
    raw = (request.form.get("board", "") or "")[:9]
    raw = raw.ljust(9, "-")
    out = []
    for ch in raw:
        if ch in {"X", "O"}:
            out.append(ch)
        else:
            out.append("-")
    return "".join(out)


def board_wire_to_list(board_wire: str) -> list[str]:
    board_wire = (board_wire or "")[:9].ljust(9, "-")
    out = []
    for ch in board_wire:
        if ch in {"X", "O"}:
            out.append(ch)
        else:
            out.append(" ")
    return out


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


def winner_from_board(board_wire: str) -> str:
    b = board_wire_to_list(board_wire)
    wins = [
        (0, 1, 2), (3, 4, 5), (6, 7, 8),
        (0, 3, 6), (1, 4, 7), (2, 5, 8),
        (0, 4, 8), (2, 4, 6),
    ]

    for a, c, d in wins:
        if b[a] != " " and b[a] == b[c] and b[c] == b[d]:
            return b[a]

    if all(x in {"X", "O"} for x in b):
        return "D"

    return " "


def build_payload() -> dict:
    board_wire = get_board_wire()
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
        "board": board_wire_to_list(board_wire),
        "boardWire": board_wire,
    }
    return payload


def game_filename(game_id: int) -> str:
    return f"game_{game_id}.txt"


def game_data_filename(game_id: int) -> str:
    return f"game_{game_id}.json"


def write_latest(payload: dict) -> None:
    with LATEST_FILE.open("w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)


def load_game_events(game_id: int) -> list[dict]:
    path = DATA_DIR / game_data_filename(game_id)
    if not path.exists():
        return []
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def save_game_events(game_id: int, events: list[dict]) -> None:
    path = DATA_DIR / game_data_filename(game_id)
    with path.open("w", encoding="utf-8") as f:
        json.dump(events, f, ensure_ascii=False, indent=2)


def sort_events(events: list[dict]) -> list[dict]:
    return sorted(
        events,
        key=lambda e: (
            int(e.get("eventIndex", 0)),
            int(e.get("seq", 0)),
            e.get("receivedAt", ""),
            e.get("role", ""),
            e.get("deviceId", ""),
        )
    )


def merge_event(game_id: int, payload: dict) -> list[dict]:
    events = load_game_events(game_id)

    key = (
        payload.get("eventIndex", 0),
        payload.get("seq", 0),
        payload.get("role", ""),
        payload.get("deviceId", ""),
        payload.get("sessionId", ""),
    )

    replaced = False
    for i, event in enumerate(events):
        event_key = (
            event.get("eventIndex", 0),
            event.get("seq", 0),
            event.get("role", ""),
            event.get("deviceId", ""),
            event.get("sessionId", ""),
        )
        if event_key == key:
            events[i] = payload
            replaced = True
            break

    if not replaced:
        events.append(payload)

    events = sort_events(events)
    save_game_events(game_id, events)
    return events


def extract_unique_board_wires(events: list[dict]) -> list[str]:
    wires = []
    last = None

    for event in events:
        wire = (event.get("boardWire", "") or "")[:9].ljust(9, "-")

        if wire == "---------":
            continue

        if wire != last:
            wires.append(wire)
            last = wire

    return wires


def render_game_text(game_id: int, events: list[dict]) -> str:
    board_wires = extract_unique_board_wires(events)

    first_date = datetime.now().date().isoformat()
    if events:
        raw = events[0].get("receivedAt", "")
        if "T" in raw:
            first_date = raw.split("T", 1)[0]
        elif raw:
            first_date = raw[:10]

    winner = " "
    for event in reversed(events):
        w = event.get("winner", " ")
        if w in {"X", "O", "Draw"}:
            winner = w
            break

    if winner == " " and board_wires:
        winner = winner_from_board(board_wires[-1])

    lines = []
    lines.append("TicTacToe Saved Game")
    lines.append("=" * 70)
    lines.append(f"gameId: {game_id}")
    lines.append("=" * 70)
    lines.append("mini-Format:")
    lines.append("---------")

    if board_wires:
        lines.extend(board_wires)
    else:
        lines.append("---------")

    lines.append("=" * 70)
    lines.append(f"date: {first_date}")
    lines.append("=" * 70)
    lines.append(f"winner= {winner}")
    lines.append("=" * 70)

    if board_wires:
        for wire in board_wires:
            board = board_wire_to_list(wire)
            lines.append(board_as_text(board))
            lines.append("-" * 70)
    else:
        lines.append(board_as_text([" "] * 9))
        lines.append("-" * 70)

    return "\n".join(lines) + "\n"


def write_game_text(game_id: int, events: list[dict]) -> Path:
    path = SAVE_DIR / game_filename(game_id)
    content = render_game_text(game_id, events)
    with path.open("w", encoding="utf-8") as f:
        f.write(content)
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

    events = merge_event(payload["gameId"], payload)
    path = write_game_text(payload["gameId"], events)

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