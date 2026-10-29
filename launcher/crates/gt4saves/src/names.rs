//! The names of one game's items on a memory card (TASK-101 item 1, owner 2026-10-23).
//!
//! Gran Turismo 4 writes its game data as `BASCUS-97328GAMEDATA`; Spec II writes `BASCUS-97436GAMEDATA`. Each game's
//! replays start with that game's serial. The names come from the game table of the launcher (`gt4l_core::games`), so
//! no library rule depends on one global name.

/// The names a game's items carry on a card.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SaveNames {
    /// The game data folder, fixed by the game (`BASCUS-97328GAMEDATA`).
    pub game_save: &'static str,
    /// The serial that starts every replay of the game. A replay is this prefix, a letter (`R` full replay, `B` best
    /// lap) and four digits (`BASCUS-97328R0001`).
    pub replay_prefix: &'static str,
}

impl SaveNames {
    pub const fn new(game_save: &'static str, replay_prefix: &'static str) -> SaveNames {
        SaveNames { game_save, replay_prefix }
    }

    /// True for this game's game data folder.
    pub fn is_game_save(&self, name: &str) -> bool {
        name == self.game_save
    }

    /// True for this game's replay names: the prefix, a letter `R` or `B`, and four digits.
    pub fn is_replay(&self, name: &str) -> bool {
        let bytes = name.as_bytes();
        let letter = self.replay_prefix.len();
        name.len() == letter + 5
            && name.starts_with(self.replay_prefix)
            && matches!(bytes[letter], b'R' | b'B')
            && bytes[letter + 1..].iter().all(u8::is_ascii_digit)
    }

    /// True when this game owns the item: its game data or one of its replays.
    pub fn owns(&self, name: &str) -> bool {
        self.is_game_save(name) || self.is_replay(name)
    }

    /// The name of full replay `number` (`BASCUS-97328R0001` for 1).
    pub fn full_replay_name(&self, number: u32) -> String {
        format!("{}R{number:04}", self.replay_prefix)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const GT4: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
    const SPEC2: SaveNames = SaveNames::new("BASCUS-97436GAMEDATA", "BASCUS-97436");

    #[test]
    fn each_game_owns_only_its_own_names() {
        assert!(GT4.owns("BASCUS-97328GAMEDATA"));
        assert!(GT4.owns("BASCUS-97328R0001"));
        assert!(GT4.owns("BASCUS-97328B0042"));
        assert!(!GT4.owns("BASCUS-97436GAMEDATA"));
        assert!(!GT4.owns("BASCUS-97436R0001"));
        assert!(SPEC2.owns("BASCUS-97436GAMEDATA"));
        assert!(SPEC2.owns("BASCUS-97436R0001"));
        assert!(!SPEC2.owns("BASCUS-97328GAMEDATA"));
        assert!(!SPEC2.owns("BASCUS-97328R0001"));
    }

    #[test]
    fn replay_names_need_a_letter_and_four_digits() {
        assert!(!GT4.is_replay("BASCUS-97328X0001"));
        assert!(!GT4.is_replay("BASCUS-97328R001"));
        assert!(!GT4.is_replay("BASCUS-97328R00012"));
        assert!(!GT4.is_replay("BASCUS-97328R00a1"));
        assert!(!GT4.is_replay("BASCUS-9732"));
    }

    #[test]
    fn a_new_replay_name_keeps_the_games_prefix() {
        assert_eq!(GT4.full_replay_name(1), "BASCUS-97328R0001");
        assert_eq!(SPEC2.full_replay_name(12), "BASCUS-97436R0012");
    }
}
