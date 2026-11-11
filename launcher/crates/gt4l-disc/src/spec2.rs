//! Gran Turismo 4 Spec II 1.10: identification and setup (TASK-101, `docs/MULTI-GAME-LAUNCHER-PLAN.md`).
//!
//! Identification is by the **executable**, not by the whole image: SCUS-97436 plus the pinned SHA-256 of the disc's
//! `SCUS_974.36` (the program our derived ELF and plugin map depend on). The known full images are named
//! (`spec2-variants.json`); an image with the pinned executable and an unknown hash is accepted as a "Spec II 1.10 variant"
//! and logged (translations such as the Brazilian Portuguese one only replace language data). Any other SCUS-97436
//! program, including the unmodified GT4 Online Public Beta, is refused: it would need its own patch map.
//!
//! Setup checks the image where it is (decision 0075; see `setup`) and writes the derived ELF (`spec2_elf::derive`) to
//! `games\spec2\prepared`. There is no CORE step: Spec II is one plain ELF. A `.chd` is decompressed into
//! `games\spec2\disc\SCUS-97436.iso` first, as in `setup` (TASK-106).

use std::fs;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::cnf;
use crate::elf;
use crate::iso::Iso;
use crate::pins::Identity;
use crate::setup::{
    self, read_error, DiscReceipt, DiscSummary, GameImage, Receipt, ReceiptIdentity, SetupError, Stage, Unsupported,
    MAX_SYSTEM_CNF_BYTES, RECEIPT_FILE, RECEIPT_SCHEMA,
};
use crate::spec2_elf;
use crate::spec2_pins::Spec2Pins;

/// The prepared ELF (the donor derives the boot name from the file name).
pub const ELF_FILE: &str = "SCUS_974.36";
/// The version text of this game in the UI and the receipt.
pub const VERSION: &str = "1.10";
/// What an image with the pinned executable and an unknown hash is called.
pub const VARIANT_NAME: &str = "Spec II 1.10 variant";
const BOOT_FILE: &str = "SCUS_974.36;1";
const MAX_EXECUTABLE_BYTES: u64 = 16 * 1024 * 1024;

/// Opens the image and reads `SYSTEM.CNF`, with the same refusals as the Gran Turismo 4 check (file, ISO, PS2 disc).
pub(crate) fn open_and_read_system(disc: &Path) -> Result<(Iso, cnf::SystemConfig), SetupError> {
    setup::check_file(disc)?;
    let mut iso = setup::open_image(disc)?;
    let system_entry = iso.find("SYSTEM.CNF;1").map_err(|error| read_error(error, SetupError::NotPlayStation2))?;
    let system_bytes =
        iso.read(&system_entry, MAX_SYSTEM_CNF_BYTES).map_err(|error| read_error(error, SetupError::IncompleteOrModified))?;
    let config = cnf::parse(&system_bytes).map_err(SetupError::NotIso)?;
    Ok((iso, config))
}

/// The fast identity check of a Spec II disc (no full hash): the boot file is read and compared with the pin.
pub fn precheck(disc: &Path, pins: &Spec2Pins) -> Result<DiscSummary, SetupError> {
    let (iso, config) = open_and_read_system(disc)?;
    precheck_opened(iso, config, pins)
}

pub(crate) fn precheck_opened(mut iso: Iso, config: cnf::SystemConfig, pins: &Spec2Pins) -> Result<DiscSummary, SetupError> {
    let volume_identifier = iso.volume_identifier().to_string();
    let length_bytes = iso.length_bytes();
    if config.serial != pins.serial {
        return Err(SetupError::Unsupported(Unsupported::OtherGame { serial: config.serial }));
    }
    if length_bytes < iso.volume_space_bytes() {
        return Err(SetupError::IncompleteOrModified);
    }
    let program = if config.boot_path == pins.boot_path {
        match iso.find(BOOT_FILE).and_then(|entry| iso.read(&entry, MAX_EXECUTABLE_BYTES)) {
            Ok(program) => Some(program),
            Err(error) if error.is_damaged_chd() => return Err(SetupError::DamagedChd(error.to_string())),
            Err(_) => None,
        }
    } else {
        None
    };
    let Some(program) = program else {
        return Err(SetupError::Unsupported(Unsupported::Spec2Executable { beta: false }));
    };
    let sha256 = elf::sha256_hex(&program);
    if sha256 != pins.executable.sha256 || program.len() as u64 != pins.executable.size_bytes {
        return Err(SetupError::Unsupported(Unsupported::Spec2Executable { beta: sha256 == pins.beta_boot_sha256 }));
    }
    // TASK-173: the IOP modules are code too; an image is Spec II 1.10 only with 1.10's modules (any data may differ).
    for module in &pins.iop_modules {
        let found = match iso
            .find_path(&module.path)
            .and_then(|entry| if entry.size_bytes == module.identity.size_bytes { iso.read(&entry, module.identity.size_bytes).map(Some) } else { Ok(None) })
        {
            Ok(found) => found,
            Err(error) if error.is_damaged_chd() => return Err(SetupError::DamagedChd(error.to_string())),
            Err(_) => None,
        };
        if found.is_none_or(|bytes| elf::sha256_hex(&bytes) != module.identity.sha256) {
            return Err(SetupError::Unsupported(Unsupported::Spec2Executable { beta: false }));
        }
    }
    Ok(DiscSummary {
        serial: config.serial,
        version: VERSION.to_string(),
        video_mode: config.video_mode,
        volume_identifier,
        size_bytes: length_bytes,
        qualified_version: false,
    })
}

/// Inputs of one Spec II setup run (the folders are the `games\spec2` ones).
pub struct Spec2Request<'a> {
    pub disc: &'a Path,
    /// Where the plain image of a `.chd` is written (`games\spec2\disc`); unused for a `.iso`.
    pub disc_folder: &'a Path,
    pub prepared: &'a Path,
    pub staging: &'a Path,
    pub install_dir: &'a Path,
    pub pins: &'a Spec2Pins,
    pub launcher_version: &'a str,
    pub cancel: &'a AtomicBool,
}

/// The name an image of Spec II is shown and logged under: a known name, or the variant text.
pub fn image_name(pins: &Spec2Pins, identity: &Identity) -> String {
    pins.known_image(identity).map(str::to_string).unwrap_or_else(|| VARIANT_NAME.to_string())
}

/// Runs the whole Spec II setup. On success the prepared folder holds the receipt and the derived ELF, and the image is where
/// it was. Cancel or failure removes the staging folder.
pub fn run(request: &Spec2Request<'_>, progress: &mut dyn FnMut(Stage, f32)) -> Result<Receipt, SetupError> {
    let summary = precheck(request.disc, request.pins)?;
    progress(Stage::Copying, 0.0);
    let result = GameImage::prepare(request.disc, request.disc_folder, &summary.serial, request.cancel, progress).and_then(|mut image| {
        let result = finish(request, &summary, &mut image, progress);
        if result.is_err() {
            image.discard();
        }
        result
    });
    if result.is_err() {
        let _ = fs::remove_dir_all(request.staging);
    }
    result
}

fn finish(
    request: &Spec2Request<'_>,
    summary: &DiscSummary,
    image: &mut GameImage,
    progress: &mut dyn FnMut(Stage, f32),
) -> Result<Receipt, SetupError> {
    let pins = request.pins;
    let identity = image.identity.clone();
    let identity = &identity;
    progress(Stage::Checking, 0.0);
    if identity.size_bytes != summary.size_bytes {
        return Err(SetupError::IncompleteOrModified);
    }
    let name = image_name(pins, identity);
    progress(Stage::Checking, 1.0);
    if request.cancel.load(Ordering::Relaxed) {
        return Err(SetupError::Cancelled);
    }

    progress(Stage::Preparing, 0.0);
    let (original, derived) = extract_and_derive(&image.path, pins)?;
    progress(Stage::Preparing, 0.5);
    if request.staging.exists() {
        fs::remove_dir_all(request.staging)?;
    }
    fs::create_dir_all(request.staging)?;
    setup::write_synced(&request.staging.join(ELF_FILE), &derived)?;
    // The last point where a cancel leaves everything as it was: the commit below may replace an earlier decompressed image.
    if request.cancel.load(Ordering::Relaxed) {
        return Err(SetupError::Cancelled);
    }

    let image_receipt = image.commit(request.disc, request.install_dir)?;
    let receipt = Receipt {
        schema_version: RECEIPT_SCHEMA,
        launcher_version: request.launcher_version.to_string(),
        payload: "bundled".to_string(),
        game: "spec2".to_string(),
        disc: DiscReceipt {
            path: request.disc.display().to_string(),
            size_bytes: identity.size_bytes,
            sha256: identity.sha256.clone(),
            serial: summary.serial.clone(),
            version: summary.version.clone(),
            volume_identifier: summary.volume_identifier.clone(),
        },
        image: Some(image_receipt),
        qualified_dump: pins.known_image(identity).is_some(),
        image_name: name,
        core: ReceiptIdentity::default(),
        original: Some(ReceiptIdentity { size_bytes: original.len() as u64, sha256: elf::sha256_hex(&original) }),
        elf: ReceiptIdentity { size_bytes: derived.len() as u64, sha256: elf::sha256_hex(&derived) },
        prepared_unix_seconds: setup::unix_seconds(),
    };
    let receipt_bytes = serde_json::to_vec_pretty(&receipt).map_err(|error| SetupError::Internal(error.to_string()))?;
    setup::write_synced(&request.staging.join(RECEIPT_FILE), &receipt_bytes)?;
    setup::swap_into_place(request.staging, request.prepared)?;
    progress(Stage::Preparing, 1.0);
    Ok(receipt)
}

/// Reads `SCUS_974.36` from the image, checks it against the pin, derives the ELF and checks that too.
fn extract_and_derive(image: &Path, pins: &Spec2Pins) -> Result<(Vec<u8>, Vec<u8>), SetupError> {
    let mut iso = Iso::open(image).map_err(|error| SetupError::Io(error.to_string()))?;
    let entry = iso.find(BOOT_FILE).map_err(|error| SetupError::Internal(error.to_string()))?;
    let original = iso.read(&entry, MAX_EXECUTABLE_BYTES).map_err(|error| SetupError::Internal(error.to_string()))?;
    let found = Identity { size_bytes: original.len() as u64, sha256: elf::sha256_hex(&original) };
    if found != pins.executable {
        return Err(SetupError::Internal("SCUS_974.36 differs from the pinned identity".into()));
    }
    let derived = spec2_elf::derive(&original, &pins.patches).map_err(SetupError::Internal)?;
    let derived_identity = Identity { size_bytes: derived.len() as u64, sha256: elf::sha256_hex(&derived) };
    if derived_identity != pins.derived {
        return Err(SetupError::Internal("the derived ELF differs from the pinned identity".into()));
    }
    Ok((original, derived))
}

/// Fast check: the prepared ELF exists with the pinned size.
pub fn verify_prepared(prepared: &Path, pins: &Spec2Pins) -> Result<(), String> {
    let size = fs::metadata(prepared.join(ELF_FILE)).map_err(|_| format!("{ELF_FILE} is missing"))?.len();
    if size != pins.derived.size_bytes {
        return Err(format!("{ELF_FILE} has size {size}, expected {}", pins.derived.size_bytes));
    }
    Ok(())
}

/// The cheap check of every Spec II launch (receipt, prepared size, image size and time; no hash).
pub fn check_installed(prepared: &Path, disc_folder: &Path, install_dir: &Path, pins: &Spec2Pins) -> setup::Installed {
    setup::check_installed_with(prepared, disc_folder, install_dir, &|folder| verify_prepared(folder, pins))
}
