//! CHD disc images (TASK-106): synthetic DVD and CD CHDs of a synthetic game (no game data) are identified in place, set up
//! into a plain image in the disc folder, and refused when damaged, truncated, of another kind or cancelled.
//!
//! The fixtures are written by the `chd` crate's own writer (feature `encode`, dev only), in the layouts `chdman createdvd`
//! and `chdman createcd` write. `real_chdman_images_match_their_iso` checks the reader against the real chdman when
//! `GT4_CHDMAN` names its program (skipped otherwise).

use std::fs::{self, File};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};

use chd::metadata::KnownMetadata;
use chd::write::{ChdWriteOptions, ChdWriter};
use gt4l_disc::chd::{ChdDisc, Layout, CD_FRAME_BYTES};
use gt4l_disc::identify::{identify, DiscGame};
use gt4l_disc::iso::Iso;
use gt4l_disc::setup::{self, Installed, SetupError, SetupRequest, Stage};
use gt4l_disc::spec2_pins::Spec2Pins;
use gt4l_disc::testing::{build_iso, pins_for, synthetic_game, system_cnf};
use gt4l_disc::{Identity, Pins};

const BODY: &[u8] = b"synthetic guest code, not game data, 0123456789abcdef";
const FILLER_BYTES: usize = 192 * 1024;

struct Fixture {
    root: PathBuf,
}

impl Fixture {
    fn new(name: &str) -> Fixture {
        let root = std::env::temp_dir().join(format!("gt4l-chd-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        Fixture { root }
    }

    fn path(&self, name: &str) -> PathBuf {
        self.root.join(name)
    }

    fn disc_folder(&self) -> PathBuf {
        self.path("disc")
    }

    /// The files in the disc folder, sorted.
    fn disc_files(&self) -> Vec<String> {
        let mut names: Vec<String> = fs::read_dir(self.disc_folder())
            .map(|entries| entries.map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned()).collect())
            .unwrap_or_default();
        names.sort();
        names
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

/// Bytes that do not compress (a fixed linear congruential sequence), so the hunks of the CHD hold real data.
fn filler(seed: u32) -> Vec<u8> {
    let mut state = seed;
    (0..FILLER_BYTES)
        .map(|_| {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            (state >> 24) as u8
        })
        .collect()
}

/// A synthetic Gran Turismo 4 disc and the pins that accept it.
fn good_image_and_pins() -> (Vec<u8>, Pins) {
    let game = synthetic_game(0x0010_0000, 0x0010_0008, BODY);
    let image = build_iso(
        "GT4",
        &[
            ("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![0xA5; 3000]),
            ("FILLER.BIN;1", filler(7)),
        ],
    );
    let pins = pins_for(&image, &game, "SCUS-97328", "2.00");
    (image, pins)
}

/// Spec II pins that never match, so `identify` checks the disc as Gran Turismo 4.
fn other_spec2_pins() -> Spec2Pins {
    let nothing = Identity { size_bytes: 0, sha256: String::new() };
    Spec2Pins {
        serial: "SCUS-00000".into(),
        boot_path: "/SCUS_000.00;1".into(),
        system_version: "1.00".into(),
        executable: nothing.clone(),
        derived: nothing,
        known_images: Vec::new(),
        iop_modules: Vec::new(),
        beta_boot_sha256: String::new(),
        patches: Vec::new(),
    }
}

/// Writes `image` as a DVD CHD (`chdman createdvd`: 2048-byte units, the logical data is the ISO).
fn write_dvd_chd(path: &Path, image: &[u8]) {
    let mut writer = ChdWriter::new(File::create(path).unwrap(), ChdWriteOptions::dvd()).unwrap();
    // chdman also writes an empty `DVD ` metadata entry, which the `chd` writer refuses; the reader does not need it (the
    // real chdman images of `real_chdman_images_match_their_iso` have it).
    writer.write_all(image).unwrap();
    writer.finish().unwrap();
}

/// Writes `image` as a one-track CD CHD (`chdman createcd`): frames of 2352 sector bytes and 96 subcode bytes, the track
/// padded to a multiple of four frames. `kind` is the track type; the user data sits where that type puts it.
fn write_cd_chd(path: &Path, image: &[u8], kind: &str) {
    let (offset, raw_mode) = match kind {
        "MODE1" | "MODE2_FORM1" => (0, None),
        "MODE2" => (8, None),
        "MODE1_RAW" => (16, Some(1u8)),
        "MODE2_RAW" => (24, Some(2u8)),
        other => panic!("no layout for {other}"),
    };
    let frames = image.len() / 2048;
    let mut data = Vec::new();
    for sector in image.chunks(2048) {
        let mut frame = vec![0u8; CD_FRAME_BYTES as usize];
        if let Some(mode) = raw_mode {
            frame[..12].copy_from_slice(&[0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0]);
            frame[15] = mode;
        }
        frame[offset..offset + 2048].copy_from_slice(sector);
        data.extend_from_slice(&frame);
    }
    while !(data.len() / CD_FRAME_BYTES as usize).is_multiple_of(4) {
        data.extend_from_slice(&[0u8; CD_FRAME_BYTES as usize]);
    }
    let mut writer = ChdWriter::new(File::create(path).unwrap(), ChdWriteOptions::cd()).unwrap();
    let track = format!("TRACK:1 TYPE:{kind} SUBTYPE:NONE FRAMES:{frames} PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0\0");
    writer.add_metadata(KnownMetadata::CdRomTrack2, track.as_bytes(), true).unwrap();
    writer.write_all(&data).unwrap();
    writer.finish().unwrap();
}

/// Reads every byte of the disc inside a CHD through the ISO 9660 reader's source.
fn read_whole(path: &Path) -> Vec<u8> {
    let mut disc = ChdDisc::open(path).unwrap();
    let mut bytes = Vec::new();
    std::io::Read::read_to_end(&mut disc, &mut bytes).unwrap();
    bytes
}

fn run_setup(fixture: &Fixture, disc: &Path, pins: &Pins, cancel: &AtomicBool) -> Result<setup::Receipt, SetupError> {
    let prepared = fixture.path("prepared");
    let staging = fixture.path("prepared.tmp");
    let install_dir = fixture.path("launcher");
    let disc_folder = fixture.disc_folder();
    let request = SetupRequest {
        disc,
        disc_folder: &disc_folder,
        prepared: &prepared,
        staging: &staging,
        install_dir: &install_dir,
        pins,
        launcher_version: "test",
        cancel,
    };
    setup::run(&request, &mut |_, _| {})
}

#[test]
fn a_dvd_chd_reads_as_its_iso_and_is_identified_in_place() {
    let fixture = Fixture::new("dvd-read");
    let (image, pins) = good_image_and_pins();
    let chd = fixture.path("Gran Turismo 4.chd");
    write_dvd_chd(&chd, &image);
    assert!(fs::metadata(&chd).unwrap().len() < image.len() as u64, "the fixture is compressed");

    let disc = ChdDisc::open(&chd).unwrap();
    assert_eq!(disc.layout(), Layout::Dvd);
    assert_eq!(disc.iso_bytes(), image.len() as u64);
    assert_eq!(read_whole(&chd), image, "the reader gives the ISO back byte for byte");

    let mut iso = Iso::open(&chd).unwrap();
    assert_eq!(iso.volume_identifier(), "GT4");
    let entry = iso.find("SYSTEM.CNF;1").unwrap();
    assert_eq!(iso.read(&entry, 4096).unwrap(), system_cnf("SCUS_973.28", "2.00", "NTSC"));

    let summary = setup::precheck(&chd, &pins).expect("the CHD passes the fast check");
    assert!(summary.qualified_version);
    assert_eq!(summary.size_bytes, image.len() as u64);
    let found = identify(&chd, &pins, &other_spec2_pins()).unwrap();
    assert_eq!(found.game, DiscGame::Gt4);
    assert!(!fixture.disc_folder().exists(), "identifying writes nothing");
}

#[test]
fn every_cd_track_type_reads_as_its_iso() {
    let fixture = Fixture::new("cd-read");
    let (image, pins) = good_image_and_pins();
    for (kind, offset) in [("MODE1", 0), ("MODE2_FORM1", 0), ("MODE2", 8), ("MODE1_RAW", 16), ("MODE2_RAW", 24)] {
        let chd = fixture.path(&format!("{kind}.chd"));
        write_cd_chd(&chd, &image, kind);
        let disc = ChdDisc::open(&chd).unwrap();
        assert_eq!(disc.layout(), Layout::Cd { first_frame: 0, user_data_offset: offset }, "{kind}");
        assert_eq!(disc.iso_bytes(), image.len() as u64, "{kind}: the padding frames are not part of the disc");
        assert_eq!(read_whole(&chd), image, "{kind}");
        setup::precheck(&chd, &pins).unwrap_or_else(|error| panic!("{kind}: {error}"));
    }
}

#[test]
fn setup_from_a_chd_writes_a_plain_image_the_game_reads() {
    for cd in [false, true] {
        let fixture = Fixture::new(if cd { "setup-cd" } else { "setup-dvd" });
        let (image, pins) = good_image_and_pins();
        let chd = fixture.path("Gran Turismo 4.chd");
        if cd {
            write_cd_chd(&chd, &image, "MODE2_RAW");
        } else {
            write_dvd_chd(&chd, &image);
        }
        let chd_before = fs::read(&chd).unwrap();
        let cancel = AtomicBool::new(false);
        let mut stages: Vec<(Stage, f32)> = Vec::new();
        let prepared = fixture.path("prepared");
        let staging = fixture.path("prepared.tmp");
        let install_dir = fixture.path("launcher");
        let disc_folder = fixture.disc_folder();
        let request = SetupRequest {
            disc: &chd,
            disc_folder: &disc_folder,
            prepared: &prepared,
            staging: &staging,
            install_dir: &install_dir,
            pins: &pins,
            launcher_version: "test",
            cancel: &cancel,
        };
        let receipt = setup::run(&request, &mut |stage, fraction| stages.push((stage, fraction))).expect("setup from a CHD succeeds");

        assert!(stages.iter().any(|(stage, fraction)| *stage == Stage::Copying && *fraction == 1.0), "the copy row reaches 100%");
        assert_eq!(stages.last().copied(), Some((Stage::Preparing, 1.0)));
        let copy = fixture.disc_folder().join("SCUS-97328.iso");
        assert_eq!(fs::read(&copy).unwrap(), image, "the plain image is the ISO the CHD holds");
        assert_eq!(fixture.disc_files(), vec!["SCUS-97328.iso".to_string()], "no partial file is left");
        assert_eq!(fs::read(&chd).unwrap(), chd_before, "the player's CHD is only read");

        let recorded = receipt.image.clone().unwrap();
        assert_eq!(recorded.linked_path, None, "the game reads the launcher's own copy, not the CHD");
        assert_eq!(recorded.file, "SCUS-97328.iso");
        assert_eq!(recorded.size_bytes, image.len() as u64);
        assert_eq!(receipt.disc.path, chd.display().to_string(), "the receipt names the file the player picked");
        assert_eq!(receipt.disc.sha256, pins.disc.sha256);
        assert!(receipt.qualified_dump, "the decompressed image is the qualified dump");

        match setup::check_installed(&prepared, &fixture.disc_folder(), &install_dir, &pins) {
            Installed::Ready { image, serial, .. } => {
                assert_eq!(image, copy);
                assert_eq!(serial, "SCUS-97328");
            }
            other => panic!("expected Ready, got {other:?}"),
        }
        // The CHD is not needed after setup.
        fs::remove_file(&chd).unwrap();
        assert!(matches!(
            setup::check_installed(&prepared, &fixture.disc_folder(), &install_dir, &pins),
            Installed::Ready { .. }
        ));
    }
}

#[test]
fn setting_up_again_replaces_the_earlier_copy() {
    let fixture = Fixture::new("again");
    let (image, pins) = good_image_and_pins();
    let chd = fixture.path("game.chd");
    write_dvd_chd(&chd, &image);
    let cancel = AtomicBool::new(false);
    run_setup(&fixture, &chd, &pins, &cancel).unwrap();
    run_setup(&fixture, &chd, &pins, &cancel).expect("a second setup replaces the copy");
    assert_eq!(fixture.disc_files(), vec!["SCUS-97328.iso".to_string()]);
    assert!(matches!(
        setup::check_installed(&fixture.path("prepared"), &fixture.disc_folder(), &fixture.path("launcher"), &pins),
        Installed::Ready { .. }
    ));
}

#[test]
fn a_cancelled_setup_keeps_the_earlier_install() {
    let fixture = Fixture::new("cancel");
    let (image, pins) = good_image_and_pins();
    let chd = fixture.path("game.chd");
    write_dvd_chd(&chd, &image);
    let cancel = AtomicBool::new(false);
    run_setup(&fixture, &chd, &pins, &cancel).unwrap();
    let copy = fixture.disc_folder().join("SCUS-97328.iso");
    let copy_before = fs::metadata(&copy).unwrap().modified().unwrap();

    // Cancel while the CHD is being decompressed.
    let prepared = fixture.path("prepared");
    let staging = fixture.path("prepared.tmp");
    let install_dir = fixture.path("launcher");
    let disc_folder = fixture.disc_folder();
    let request = SetupRequest {
        disc: &chd,
        disc_folder: &disc_folder,
        prepared: &prepared,
        staging: &staging,
        install_dir: &install_dir,
        pins: &pins,
        launcher_version: "test",
        cancel: &cancel,
    };
    let result = setup::run(&request, &mut |stage, fraction| {
        if stage == Stage::Copying && fraction > 0.0 {
            cancel.store(true, Ordering::Relaxed);
        }
    });
    assert!(matches!(result, Err(SetupError::Cancelled)), "{result:?}");
    assert_eq!(fixture.disc_files(), vec!["SCUS-97328.iso".to_string()], "the partial image is removed");
    assert_eq!(fs::metadata(&copy).unwrap().modified().unwrap(), copy_before, "the earlier copy is untouched");
    assert!(!staging.exists());
    assert!(matches!(setup::check_installed(&prepared, &disc_folder, &install_dir, &pins), Installed::Ready { .. }));
}

#[test]
fn a_damaged_chd_is_refused_and_leaves_nothing() {
    let fixture = Fixture::new("damaged");
    let (image, pins) = good_image_and_pins();
    let good = fixture.path("good.chd");
    write_dvd_chd(&good, &image);
    let bytes = fs::read(&good).unwrap();

    // One byte changed in the hunk data (the header is the first 124 bytes; the map is at the end of the file).
    let mut damaged_bytes = bytes.clone();
    let middle = bytes.len() / 2;
    damaged_bytes[middle] ^= 0x5A;
    let damaged = fixture.path("damaged.chd");
    fs::write(&damaged, &damaged_bytes).unwrap();
    let cancel = AtomicBool::new(false);
    let error = run_setup(&fixture, &damaged, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::DamagedChd(_)), "{error:?}");
    assert!(error.is_disc_refusal());
    assert_eq!(
        error.user_message(),
        "This .chd file is damaged: part of it does not decompress or fails its check. Make it again from your disc, or use a plain .iso."
    );
    assert!(fixture.disc_files().is_empty(), "no image is left: {:?}", fixture.disc_files());
    assert!(!fixture.path("prepared").exists() && !fixture.path("prepared.tmp").exists());

    // Cut short: the map at the end is gone.
    let truncated = fixture.path("truncated.chd");
    fs::write(&truncated, &bytes[..bytes.len() * 3 / 4]).unwrap();
    let error = run_setup(&fixture, &truncated, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::DamagedChd(_)), "{error:?}");
    assert!(error.is_disc_refusal());
    assert!(fixture.disc_files().is_empty());

    // A CHD whose map was never written (the header's map offset is 0, as in a run of chdman that stopped before its end).
    let unfinished_bytes: Vec<u8> = bytes
        .iter()
        .enumerate()
        .map(|(index, byte)| if (40..48).contains(&index) { 0 } else { *byte })
        .collect();
    let unfinished = fixture.path("unfinished.chd");
    fs::write(&unfinished, &unfinished_bytes).unwrap();
    let error = run_setup(&fixture, &unfinished, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::DamagedChd(_)), "{error:?}");
    assert!(error.user_message().starts_with("This .chd file is damaged"));
    assert!(fixture.disc_files().is_empty());
}

#[test]
fn a_chd_whose_data_differs_from_its_sha1_is_refused() {
    let fixture = Fixture::new("sha1");
    let (image, pins) = good_image_and_pins();
    // An uncompressed CHD has no codec check on its hunks; a changed byte is caught by the SHA-1 of the header.
    let path = fixture.path("plain.chd");
    let options = ChdWriteOptions::dvd().compression(&[]).hash_uncompressed(true);
    let mut writer = ChdWriter::new(File::create(&path).unwrap(), options).unwrap();
    writer.write_all(&image).unwrap();
    writer.finish().unwrap();
    assert_eq!(read_whole(&path), image);

    let mut bytes = fs::read(&path).unwrap();
    let last_data_byte = bytes.len() - 1;
    bytes[last_data_byte] ^= 0x01;
    fs::write(&path, &bytes).unwrap();
    let cancel = AtomicBool::new(false);
    let error = run_setup(&fixture, &path, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::DamagedChd(_)), "{error:?}");
    assert!(fixture.disc_files().is_empty());
}

#[test]
fn chds_that_are_not_ps2_discs_are_refused_with_a_plain_message() {
    let fixture = Fixture::new("kinds");
    let (image, pins) = good_image_and_pins();
    let cancel = AtomicBool::new(false);
    let message = "This .chd file is not a PlayStation 2 DVD or CD image that GT4Recomp can read (for example a hard disk image, or one that needs a parent .chd). Make it again from your disc with chdman, or use a plain .iso.";

    // A plain ISO renamed to .chd.
    let renamed = fixture.path("renamed.chd");
    fs::write(&renamed, &image).unwrap();
    let error = run_setup(&fixture, &renamed, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::UnsupportedChd(_)), "{error:?}");
    assert_eq!(error.user_message(), message);
    assert!(error.is_disc_refusal());

    // A hard disk CHD (512-byte units and geometry metadata).
    let hard_disk = fixture.path("disk.chd");
    let mut writer = ChdWriter::new(File::create(&hard_disk).unwrap(), ChdWriteOptions::hard_disk()).unwrap();
    writer.add_metadata(KnownMetadata::HardDisk, &b"CYLS:1,HEADS:1,SECS:64,BPS:512\0"[..], true).unwrap();
    writer.write_all(&vec![0u8; 64 * 512]).unwrap();
    writer.finish().unwrap();
    assert!(matches!(run_setup(&fixture, &hard_disk, &pins, &cancel), Err(SetupError::UnsupportedChd(_))));

    // An audio CD.
    let audio = fixture.path("audio.chd");
    let mut writer = ChdWriter::new(File::create(&audio).unwrap(), ChdWriteOptions::cd()).unwrap();
    let track = b"TRACK:1 TYPE:AUDIO SUBTYPE:NONE FRAMES:8 PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0\0";
    writer.add_metadata(KnownMetadata::CdRomTrack2, &track[..], true).unwrap();
    writer.write_all(&vec![0u8; 8 * CD_FRAME_BYTES as usize]).unwrap();
    writer.finish().unwrap();
    assert!(matches!(run_setup(&fixture, &audio, &pins, &cancel), Err(SetupError::UnsupportedChd(_))));
    assert!(fixture.disc_files().is_empty());
}

#[test]
fn another_game_in_a_chd_gets_the_same_message_as_in_an_iso() {
    let fixture = Fixture::new("other-game");
    let (_, pins) = good_image_and_pins();
    let image = build_iso("OTHER", &[("SYSTEM.CNF;1", system_cnf("SLUS_123.45", "1.00", "NTSC"))]);
    let iso = fixture.path("other.iso");
    fs::write(&iso, &image).unwrap();
    let chd = fixture.path("other.chd");
    write_dvd_chd(&chd, &image);
    let from_iso = setup::precheck(&iso, &pins).unwrap_err().user_message();
    let from_chd = setup::precheck(&chd, &pins).unwrap_err().user_message();
    assert_eq!(from_chd, from_iso);
}

/// Opt-in: `GT4_CHDMAN=<path to chdman>` makes DVD and CD CHDs of the synthetic disc with the real chdman and checks that
/// the reader gives the ISO back byte for byte and that setup accepts them.
#[test]
fn real_chdman_images_match_their_iso() {
    let Ok(chdman) = std::env::var("GT4_CHDMAN") else {
        eprintln!("GT4_CHDMAN is not set: skipped");
        return;
    };
    let fixture = Fixture::new("chdman");
    let (image, pins) = good_image_and_pins();
    let iso = fixture.path("game.iso");
    fs::write(&iso, &image).unwrap();
    // A CD track of raw MODE2 sectors, as a cue sheet and its .bin.
    let bin = fixture.path("game.bin");
    let mut raw = Vec::new();
    for sector in image.chunks(2048) {
        let mut frame = vec![0u8; 2352];
        frame[..12].copy_from_slice(&[0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0]);
        frame[15] = 2;
        frame[24..24 + 2048].copy_from_slice(sector);
        raw.extend_from_slice(&frame);
    }
    fs::write(&bin, &raw).unwrap();
    let cue = fixture.path("game.cue");
    fs::write(&cue, "FILE \"game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n").unwrap();

    let cases: [(&str, &[&str]); 3] = [
        ("dvd.chd", &["createdvd", "-i", "game.iso"]),
        ("cd-mode2.chd", &["createcd", "-i", "game.cue"]),
        ("cd-iso.chd", &["createcd", "-i", "game.iso"]),
    ];
    for (name, arguments) in cases {
        let status = Command::new(&chdman)
            .args(arguments)
            .args(["-o", name, "-f"])
            .current_dir(&fixture.root)
            .output()
            .expect("chdman runs");
        assert!(status.status.success(), "{name}: {}", String::from_utf8_lossy(&status.stderr));
        let chd = fixture.path(name);
        assert_eq!(read_whole(&chd), image, "{name}");
        setup::precheck(&chd, &pins).unwrap_or_else(|error| panic!("{name}: {error}"));
        let cancel = AtomicBool::new(false);
        run_setup(&fixture, &chd, &pins, &cancel).unwrap_or_else(|error| panic!("{name}: {error}"));
        assert_eq!(fs::read(fixture.disc_folder().join("SCUS-97328.iso")).unwrap(), image, "{name}");
    }
}

#[test]
fn the_plain_image_must_fit_before_anything_is_written() {
    let fixture = Fixture::new("space");
    let (image, _) = good_image_and_pins();
    let chd = fixture.path("Gran Turismo 4.chd");
    write_dvd_chd(&chd, &image);
    let iso_bytes = image.len() as u64;
    let needed = iso_bytes + iso_bytes / 100;
    assert_eq!(ChdDisc::iso_bytes_of(&chd).unwrap(), iso_bytes);

    // The platform cannot tell: the write itself reports a full drive.
    assert!(setup::check_chd_space(&chd, &fixture.disc_folder(), None).is_none());
    // One byte short of the figure the check uses.
    let error = setup::check_chd_space(&chd, &fixture.disc_folder(), Some(needed - 1)).expect("refused");
    match &error {
        SetupError::ChdNoSpace { needed: shown, .. } => assert_eq!(*shown, needed, "the message names the figure compared"),
        other => panic!("{other:?}"),
    }
    // Exactly enough room.
    assert!(setup::check_chd_space(&chd, &fixture.disc_folder(), Some(needed)).is_none());
    // A plain `.iso` is read where it is, so no room is needed.
    let iso = fixture.path("plain.iso");
    fs::write(&iso, &image).unwrap();
    assert!(setup::check_chd_space(&iso, &fixture.disc_folder(), Some(0)).is_none());
    assert!(!fixture.disc_folder().exists(), "checking writes nothing");
}

#[test]
fn a_stale_partial_and_replaced_copy_are_cleared_by_the_next_setup() {
    let fixture = Fixture::new("stale");
    let (image, pins) = good_image_and_pins();
    let chd = fixture.path("Gran Turismo 4.chd");
    write_dvd_chd(&chd, &image);
    fs::create_dir_all(fixture.disc_folder()).unwrap();
    fs::write(fixture.disc_folder().join("SCUS-97328.iso.partial"), b"half a run").unwrap();
    fs::write(fixture.disc_folder().join("SCUS-97328.iso.previous"), b"older copy").unwrap();

    let cancel = AtomicBool::new(false);
    run_setup(&fixture, &chd, &pins, &cancel).unwrap();
    assert_eq!(fixture.disc_files(), vec!["SCUS-97328.iso".to_string()], "the leftovers of the killed run are gone");
    assert_eq!(fs::read(fixture.disc_folder().join("SCUS-97328.iso")).unwrap(), image);
}

#[test]
fn a_header_that_asks_for_too_much_memory_is_refused_as_damaged() {
    let fixture = Fixture::new("header");
    let (image, _) = good_image_and_pins();
    let good = fixture.path("good.chd");
    write_dvd_chd(&good, &image);
    let bytes = fs::read(&good).unwrap();

    // A hunk size the `chd` crate would allocate in one block (v5 header, offset 56).
    let mut huge_hunk = bytes.clone();
    huge_hunk[56..60].copy_from_slice(&u32::MAX.to_be_bytes());
    let path = fixture.path("huge-hunk.chd");
    fs::write(&path, &huge_hunk).unwrap();
    assert!(matches!(ChdDisc::open(&path), Err(gt4l_disc::chd::ChdError::Damaged(_))));

    // A logical size that would make the hunk count and the map enormous (offset 32).
    let mut huge_data = bytes.clone();
    huge_data[32..40].copy_from_slice(&u64::MAX.to_be_bytes());
    let path = fixture.path("huge-data.chd");
    fs::write(&path, &huge_data).unwrap();
    assert!(matches!(ChdDisc::open(&path), Err(gt4l_disc::chd::ChdError::Damaged(_))));

    // Zero bytes per hunk is not a file chdman writes either.
    let mut no_hunk = bytes.clone();
    no_hunk[56..60].copy_from_slice(&0u32.to_be_bytes());
    let path = fixture.path("no-hunk.chd");
    fs::write(&path, &no_hunk).unwrap();
    assert!(matches!(ChdDisc::open(&path), Err(gt4l_disc::chd::ChdError::Damaged(_))));
}

#[test]
fn an_empty_disc_folder_is_not_a_failed_install() {
    let fixture = Fixture::new("empty");
    let (_, pins) = good_image_and_pins();
    let prepared = fixture.path("prepared");
    let install_dir = fixture.path("install");
    fs::create_dir_all(fixture.disc_folder()).unwrap();
    assert!(matches!(
        setup::check_installed(&prepared, &fixture.disc_folder(), &install_dir, &pins),
        Installed::NotSetUp
    ));

    // Something in it, without a receipt, is an install that lost its files.
    fs::write(fixture.disc_folder().join("SCUS-97328.iso"), b"x").unwrap();
    assert!(matches!(
        setup::check_installed(&prepared, &fixture.disc_folder(), &install_dir, &pins),
        Installed::Missing { .. }
    ));
}
