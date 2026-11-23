-- schema.sql: Database schema for Online Caro (Gomoku) Game Server
-- Targeting SQLite3 with Foreign Keys, Cascading Deletion, and Strict Constraints

PRAGMA foreign_keys = ON;

-- 1. Users & Credentials
CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT UNIQUE NOT NULL COLLATE NOCASE,
    salt          TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    points        INTEGER NOT NULL DEFAULT 0 CHECK (points >= 0),
    wins          INTEGER NOT NULL DEFAULT 0 CHECK (wins >= 0),
    draws         INTEGER NOT NULL DEFAULT 0 CHECK (draws >= 0),
    losses        INTEGER NOT NULL DEFAULT 0 CHECK (losses >= 0),
    created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- 2. Games (Match Sessions)
CREATE TABLE IF NOT EXISTS games (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    player_x_id   INTEGER NOT NULL REFERENCES users(id),
    player_o_id   INTEGER NOT NULL REFERENCES users(id),
    status        TEXT NOT NULL DEFAULT 'ONGOING' CHECK (status IN ('ONGOING', 'FINISHED', 'INTERRUPTED')),
    outcome       TEXT,
    reason        TEXT,
    winner_id     INTEGER REFERENCES users(id),
    created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    settled_at    DATETIME,
    CHECK (player_x_id <> player_o_id)
);

-- 3. Moves (Match Placement Log)
CREATE TABLE IF NOT EXISTS moves (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    game_id       INTEGER NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    seq           INTEGER NOT NULL CHECK (seq BETWEEN 1 AND 225),
    user_id       INTEGER NOT NULL REFERENCES users(id),
    stone         TEXT NOT NULL CHECK (stone IN ('X', 'O')),
    row           INTEGER NOT NULL CHECK (row BETWEEN 0 AND 14),
    col           INTEGER NOT NULL CHECK (col BETWEEN 0 AND 14),
    created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE (game_id, seq),
    UNIQUE (game_id, row, col)
);

-- 4. Optimization & Lookup Indexes
CREATE INDEX IF NOT EXISTS idx_users_username ON users(username);
CREATE INDEX IF NOT EXISTS idx_users_ranking  ON users(points DESC, wins DESC, username ASC);
CREATE INDEX IF NOT EXISTS idx_games_players  ON games(player_x_id, player_o_id);
CREATE INDEX IF NOT EXISTS idx_games_status   ON games(status);
CREATE INDEX IF NOT EXISTS idx_moves_game_seq  ON moves(game_id, seq);
