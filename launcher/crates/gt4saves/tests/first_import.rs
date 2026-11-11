// Issue #224: after importing a PCSX2 card into a library that has no active save, Play asked the game for a new
// profile, because the imported save stayed an inactive slot. An import now makes the only save of the library the
// active one, so Play uses it.

use gt4saves::card::{DirEntry, Node, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
use gt4saves::{Card, Library, SaveNames};
use std::fs;
use std::path::PathBuf;

const GT4: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
const GT4_GAME_SAVE: &str = "BASCUS-97328GAMEDATA";
const REPLAY: &str = "BASCUS-97328R0001";
const STAMP: [u8; 8] = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];

fn scratch(name: &str) -> PathBuf {
    let path = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("first-import-{name}"));
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

fn pattern(len: usize, seed: u8) -> Vec<u8> {
    (0..len).map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed)).collect()
}

fn file(name: &str, data: Vec<u8>) -> Node {
    Node::file(DirEntry::new(name, MODE_FILE_DEFAULT, STAMP, STAMP).unwrap(), data)
}

fn directory(name: &str, children: Vec<Node>) -> Node {
    Node::dir(DirEntry::new(name, MODE_DIR_DEFAULT, STAMP, STAMP).unwrap(), children)
}

fn game_save(garage_seed: u8) -> Node {
    directory(
        GT4_GAME_SAVE,
        vec![
            file("icon.sys", pattern(964, 1)),
            file("icon0.ico", pattern(9796, 2)),
            file("garage", pattern(1_280_064, garage_seed)),
        ],
    )
}

fn replay() -> Node {
    directory(REPLAY, vec![file("icon.sys", pattern(964, 3)), file("replay.rep", pattern(400_001, 4))])
}

fn image_of(items: &[Node]) -> Vec<u8> {
    Card::build(items).unwrap().to_image()
}

/// The names of the items the next Play puts on the card.
fn card_items(library: &Library) -> Vec<String> {
    let image = library.build_card(&library.replays().unwrap()).unwrap();
    Card::from_image(&image).unwrap().root().unwrap().iter().map(Node::name).collect()
}

#[test]
fn a_first_import_becomes_the_active_save_so_play_uses_it() {
    let library = Library::open(&scratch("single"), GT4).unwrap();
    let mut report = library.import_card_image(&image_of(&[game_save(7), replay()])).unwrap();
    assert_eq!(report.added, vec!["Save 01".to_string(), REPLAY.to_string()]);

    library.activate_first_import(&mut report).unwrap();

    assert!(report.activated, "the import report says the save is now the active save");
    assert_eq!(library.active_save().unwrap().as_deref(), Some(GT4_GAME_SAVE));
    // The point of issue #224: the card the game reads holds the imported save, so Play does not ask for a new profile.
    assert_eq!(card_items(&library), vec![GT4_GAME_SAVE.to_string(), REPLAY.to_string()]);
}

#[test]
fn importing_the_same_card_again_on_an_older_library_makes_its_save_active() {
    // A library made before this fix holds the imported save as a slot and has no active save; importing the same card
    // again must still end with that save on the card Play builds.
    let library = Library::open(&scratch("older"), GT4).unwrap();
    let card = image_of(&[game_save(4)]);
    library.import_card_image(&card).unwrap();
    assert_eq!(library.active_save().unwrap(), None);

    let mut report = library.import_card_image(&card).unwrap();
    assert_eq!(report.unchanged, vec![GT4_GAME_SAVE.to_string()]);
    library.activate_first_import(&mut report).unwrap();

    assert!(report.activated);
    assert_eq!(card_items(&library), vec![GT4_GAME_SAVE.to_string()]);
}

#[test]
fn an_import_never_replaces_an_active_save() {
    let library = Library::open(&scratch("keeps-active"), GT4).unwrap();
    library.import_card_image(&image_of(&[game_save(1)])).unwrap();
    library.activate_slot("Save 01").unwrap();

    // A different save arrives: it becomes a slot, and what Play uses does not change.
    let mut report = library.import_card_image(&image_of(&[game_save(2)])).unwrap();
    assert_eq!(report.added, vec!["Save 01".to_string()]);
    library.activate_first_import(&mut report).unwrap();

    assert!(!report.activated);
    assert_eq!(library.slots().unwrap(), vec!["Save 01".to_string()]);
    assert_eq!(card_items(&library), vec![GT4_GAME_SAVE.to_string()]);
}

#[test]
fn two_saves_to_choose_from_leave_the_choice_to_the_player() {
    let library = Library::open(&scratch("two-slots"), GT4).unwrap();
    library.import_card_image(&image_of(&[game_save(1)])).unwrap();
    library.activate_slot("Save 01").unwrap();
    library.new_save().unwrap();
    assert_eq!(library.active_save().unwrap(), None);

    // The player already has one parked save; a card that brings another one leaves both as slots.
    let mut report = library.import_card_image(&image_of(&[game_save(2)])).unwrap();
    assert_eq!(report.added, vec!["Save 02".to_string()]);
    library.activate_first_import(&mut report).unwrap();

    assert!(!report.activated);
    assert_eq!(library.active_save().unwrap(), None);
    assert_eq!(library.slots().unwrap(), vec!["Save 01".to_string(), "Save 02".to_string()]);
}

#[test]
fn activating_an_imported_slot_parks_the_previous_save() {
    // The same sequence the launcher's Saves page runs (gt4l-tauri `activating_a_slot_parks_the_previous_save`), so
    // the expectations there are checked here, where they can run.
    let library = Library::open(&scratch("park"), GT4).unwrap();
    let mut first = library.import_card_image(&image_of(&[game_save(3)])).unwrap();
    library.activate_first_import(&mut first).unwrap();
    assert_eq!(card_items(&library), vec![GT4_GAME_SAVE.to_string()]);

    let mut report = library.import_card_image(&image_of(&[game_save(4)])).unwrap();
    assert_eq!(report.added, vec!["Save 01".to_string()]);
    library.activate_first_import(&mut report).unwrap();
    assert!(!report.activated, "the active save stays the one Play uses");

    library.activate_slot("Save 01").unwrap();
    assert_eq!(library.slots().unwrap(), vec!["Save 02".to_string()]);
    assert_eq!(card_items(&library), vec![GT4_GAME_SAVE.to_string()]);
}

#[test]
fn a_card_without_a_save_does_not_activate_a_parked_save() {
    let library = Library::open(&scratch("replay-only"), GT4).unwrap();
    library.import_card_image(&image_of(&[game_save(1)])).unwrap();
    library.activate_slot("Save 01").unwrap();
    library.new_save().unwrap();

    // Importing only a replay must not quietly turn the parked save into the one Play uses.
    let mut report = library.import_card_image(&image_of(&[replay()])).unwrap();
    assert_eq!(report.added, vec![REPLAY.to_string()]);
    library.activate_first_import(&mut report).unwrap();

    assert!(!report.activated);
    assert_eq!(library.active_save().unwrap(), None);
    assert_eq!(library.slots().unwrap(), vec!["Save 01".to_string()]);
}
