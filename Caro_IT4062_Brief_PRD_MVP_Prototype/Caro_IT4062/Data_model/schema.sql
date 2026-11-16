PRAGMA foreign_keys = ON;

CREATE TABLE users (
    user_id INTEGER PRIMARY KEY,
    username TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    points INTEGER NOT NULL DEFAULT 0 CHECK (points >= 0),
    wins INTEGER NOT NULL DEFAULT 0 CHECK (wins >= 0),
    draws INTEGER NOT NULL DEFAULT 0 CHECK (draws >= 0),
    losses INTEGER NOT NULL DEFAULT 0 CHECK (losses >= 0)
);

CREATE TABLE matches (
    match_id INTEGER PRIMARY KEY AUTOINCREMENT,
    x_user_id INTEGER NOT NULL REFERENCES users(user_id),
    o_user_id INTEGER NOT NULL REFERENCES users(user_id),
    status TEXT NOT NULL DEFAULT 'ACTIVE' CHECK (status IN ('ACTIVE','FINISHED','CANCELLED')),
    outcome TEXT CHECK (outcome IN ('X_WIN','O_WIN','DRAW')),
    reason TEXT,
    started_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    ended_at TEXT,
    CHECK (x_user_id <> o_user_id),
    CHECK (
        (status = 'ACTIVE' AND outcome IS NULL AND ended_at IS NULL)
        OR (status = 'FINISHED' AND outcome IS NOT NULL AND ended_at IS NOT NULL)
        OR (status = 'CANCELLED' AND outcome IS NULL AND ended_at IS NOT NULL)
    )
);

CREATE TABLE moves (
    match_id INTEGER NOT NULL REFERENCES matches(match_id),
    seq INTEGER NOT NULL CHECK (seq BETWEEN 1 AND 225),
    user_id INTEGER NOT NULL REFERENCES users(user_id),
    stone TEXT NOT NULL CHECK (stone IN ('X','O')),
    row_no INTEGER NOT NULL CHECK (row_no BETWEEN 1 AND 15),
    col_no INTEGER NOT NULL CHECK (col_no BETWEEN 1 AND 15),
    PRIMARY KEY (match_id, seq),
    UNIQUE (match_id, row_no, col_no)
);

CREATE INDEX matches_by_status ON matches(status);
CREATE INDEX ranking_order ON users(points DESC, wins DESC, username);
