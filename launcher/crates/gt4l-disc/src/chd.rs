//! CHD disc images (TASK-106, issue #90). A `.chd` is MAME's compressed disc format ("Compressed Hunks of Data"), made by
//! `chdman createdvd` (a DVD: the logical data is the plain ISO) or `chdman createcd` (a CD: frames of 2352 sector bytes and
//! 96 subcode bytes, described by track metadata). The reader is the `chd` crate (BSD-3-Clause, pure Rust).
//!
//! - [`ChdDisc`] reads the plain 2048-byte sectors of the disc straight from the CHD, so the fast identity checks run on the
//!   picked file without decompressing it first.
//! - [`convert`] decompresses the whole CHD once, at setup, into a plain ISO in the launcher's own `disc` folder. The game
//!   reads that copy (it only reads plain ISO images); the player's `.chd` is only ever read. Every hunk is checked (the
//!   CRC of each hunk in the `chd` crate) and the whole data against the SHA-1 the CHD header records, so a damaged CHD is
//!   refused and never prepared.
//!
//! Only what PlayStation 2 discs use is accepted: a DVD CHD, or a CD CHD whose first track is a data track (`MODE1`,
//! `MODE1_RAW`, `MODE2`, `MODE2_FORM1`, `MODE2_RAW`). Hard disk and LaserDisc CHDs, GD-ROMs, the old binary CD metadata
//! (`CHCD`) and CHDs that need a parent CHD are refused with a plain message.

use std::fs::{self, File};
use std::io::{self, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use chd::header::Header;
use chd::metadata::{KnownMetadata, Metadata};
use chd::Chd;
use sha1::Sha1;
use sha2::{Digest, Sha256};

use crate::iso::SECTOR_BYTES;
use crate::pins::Identity;

/// The file extension of a CHD image, compared without case.
pub const EXTENSION: &str = "chd";
/// One CD frame in a CHD: 2352 sector bytes, then 96 subcode bytes.
pub const CD_FRAME_BYTES: u32 = 2352 + 96;
/// Hunks decompressed between two progress reports and cancel checks of [`convert`].
const HUNKS_PER_REPORT: u32 = 64;

/// Whether `path` names a CHD image (by its extension).
pub fn is_chd(path: &Path) -> bool {
    path.extension().and_then(|value| value.to_str()).is_some_and(|value| value.eq_ignore_ascii_case(EXTENSION))
}

/// Why a CHD cannot be read.
#[derive(Debug)]
pub enum ChdError {
    /// Not a CHD the launcher reads: not a CHD at all, a hard disk or LaserDisc image, a GD-ROM, an audio CD, or a CHD that
    /// needs its parent CHD.
    Unsupported(String),
    /// Part of the CHD does not decompress, fails its checksum, or the data differs from the SHA-1 of its header.
    Damaged(String),
    Io(io::Error),
}

impl std::fmt::Display for ChdError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ChdError::Unsupported(detail) => write!(formatter, "unsupported CHD: {detail}"),
            ChdError::Damaged(detail) => write!(formatter, "damaged CHD: {detail}"),
            ChdError::Io(error) => write!(formatter, "{error}"),
        }
    }
}

impl std::error::Error for ChdError {}

/// The error a [`ChdDisc`] read returns for a hunk that does not decompress or fails its checksum, inside an
/// `io::Error` of kind `InvalidData` (see [`is_damaged`]).
#[derive(Debug)]
struct DamagedHunk {
    hunk: u32,
    error: chd::Error,
}

impl std::fmt::Display for DamagedHunk {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "hunk {} of the CHD cannot be read: {}", self.hunk, self.error)
    }
}

impl std::error::Error for DamagedHunk {}

/// Whether an I/O error from a [`ChdDisc`] read means the CHD is damaged (and not, say, a disconnected drive).
pub fn is_damaged(error: &io::Error) -> bool {
    error.get_ref().is_some_and(|inner| inner.is::<DamagedHunk>())
}

/// Where the 2048-byte sectors of the disc are in the logical data of the CHD.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Layout {
    /// A DVD (`chdman createdvd`): the logical data is the ISO itself.
    Dvd,
    /// A CD (`chdman createcd`): sector `n` of the ISO is the user data of frame `first_frame + n` of the first track,
    /// `user_data_offset` bytes into the frame.
    Cd { first_frame: u64, user_data_offset: u32 },
}

/// A CHD opened as a plain disc of 2048-byte sectors. It implements `Read` and `Seek` over the bytes of the ISO the CHD
/// holds, so the ISO 9660 reader reads it like a file.
pub struct ChdDisc {
    chd: Chd<BufReader<File>>,
    layout: Layout,
    sectors: u64,
    hunk_bytes: u64,
    logical_bytes: u64,
    compressed: Vec<u8>,
    hunk: Vec<u8>,
    hunk_number: Option<u32>,
    /// Position in the ISO bytes.
    position: u64,
}

impl ChdDisc {
    /// Opens `path` and works out the layout of its sectors. Reads the header, the hunk map and the metadata, no hunk data.
    pub fn open(path: &Path) -> Result<ChdDisc, ChdError> {
        let file = File::open(path).map_err(ChdError::Io)?;
        check_header(path)?;
        // The `chd` crate turns I/O errors into its own codes, so a failed open of a file that starts like a CHD is a
        // damaged or cut-short file, and anything else is not a CHD.
        let chd = Chd::open(BufReader::new(file), None).map_err(|error| {
            ChdError::Damaged(format!("its header, hunk map or track list cannot be read ({error})"))
        })?;
        Self::from_chd(chd)
    }

    fn from_chd(mut chd: Chd<BufReader<File>>) -> Result<ChdDisc, ChdError> {
        let header = chd.header();
        if header.has_parent() {
            return Err(ChdError::Unsupported("it needs a parent CHD".into()));
        }
        let hunk_bytes = u64::from(header.hunk_size());
        let unit_bytes = u64::from(header.unit_bytes());
        let logical_bytes = header.logical_bytes();
        if hunk_bytes == 0 || logical_bytes == 0 {
            return Err(ChdError::Unsupported("it holds no data".into()));
        }
        let metadata: Vec<Metadata> = chd
            .metadata_refs()
            .try_into()
            .map_err(|error| ChdError::Damaged(format!("its metadata cannot be read ({error})")))?;
        let (layout, sectors) = layout_of(&metadata, hunk_bytes, unit_bytes, logical_bytes)?;
        Ok(ChdDisc {
            chd,
            layout,
            sectors,
            hunk_bytes,
            logical_bytes,
            compressed: Vec::new(),
            hunk: vec![0u8; hunk_bytes as usize],
            hunk_number: None,
            position: 0,
        })
    }

    /// The size of the plain ISO the CHD holds, in bytes.
    pub fn iso_bytes(&self) -> u64 {
        self.sectors * SECTOR_BYTES
    }

    /// [`ChdDisc::iso_bytes`] of the CHD at `path`, from its header and metadata only (no hunk is decompressed).
    pub fn iso_bytes_of(path: &Path) -> Result<u64, ChdError> {
        Ok(ChdDisc::open(path)?.iso_bytes())
    }

    pub fn layout(&self) -> Layout {
        self.layout
    }

    /// Where ISO sector `sector` starts in the logical data of the CHD.
    fn logical_offset(&self, sector: u64) -> u64 {
        match self.layout {
            Layout::Dvd => sector * SECTOR_BYTES,
            Layout::Cd { first_frame, user_data_offset } => {
                (first_frame + sector) * u64::from(CD_FRAME_BYTES) + u64::from(user_data_offset)
            }
        }
    }

    /// Decompresses hunk `number` into `self.hunk`, unless it is already there.
    fn load_hunk(&mut self, number: u32) -> io::Result<()> {
        if self.hunk_number == Some(number) {
            return Ok(());
        }
        self.hunk_number = None;
        // The `chd` crate does not keep the I/O error of a failed read apart from a bad checksum or a hunk that does not
        // decompress (a truncated file is a read error too), so every failure here counts as a damaged CHD.
        self.chd
            .hunk(number)
            .and_then(|mut hunk| hunk.read_hunk_in(&mut self.compressed, &mut self.hunk))
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, DamagedHunk { hunk: number, error }))?;
        self.hunk_number = Some(number);
        Ok(())
    }
}

impl Read for ChdDisc {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        let iso_bytes = self.iso_bytes();
        if buffer.is_empty() || self.position >= iso_bytes {
            return Ok(0);
        }
        let sector = self.position / SECTOR_BYTES;
        let within_sector = self.position % SECTOR_BYTES;
        let offset = self.logical_offset(sector) + within_sector;
        let number = u32::try_from(offset / self.hunk_bytes)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "sector beyond the hunks of the CHD"))?;
        self.load_hunk(number)?;
        // A sector never crosses a hunk: a DVD hunk holds whole sectors and a CD hunk whole frames (checked by `layout_of`).
        let within_hunk = (offset % self.hunk_bytes) as usize;
        let count = buffer.len().min((SECTOR_BYTES - within_sector) as usize);
        buffer[..count].copy_from_slice(&self.hunk[within_hunk..within_hunk + count]);
        self.position += count as u64;
        Ok(count)
    }
}

impl Seek for ChdDisc {
    fn seek(&mut self, position: SeekFrom) -> io::Result<u64> {
        let target = match position {
            SeekFrom::Start(offset) => Some(offset),
            SeekFrom::End(offset) => self.iso_bytes().checked_add_signed(offset),
            SeekFrom::Current(offset) => self.position.checked_add_signed(offset),
        };
        self.position = target.ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "seek before the start of the disc"))?;
        Ok(self.position)
    }
}

/// The magic of a CHD file, and the size of a CHD v5 header (the version current chdman writes).
const MAGIC: &[u8; 8] = b"MComprHD";
const V5_HEADER_BYTES: u64 = 124;
const V5_VERSION: u32 = 5;
/// Where the hunk map offset is in a CHD v5 header.
const V5_MAP_OFFSET: usize = 40;

/// Reads the bytes at the start of a CHD that say whether it can be opened at all: a CHD v5 with no hunk map was never
/// finished (chdman writes the map at the end), so it is refused as damaged before the `chd` crate trips over it.
fn check_header(path: &Path) -> Result<(), ChdError> {
    let mut file = File::open(path).map_err(ChdError::Io)?;
    let mut header = [0u8; V5_HEADER_BYTES as usize];
    let read = file.read(&mut header).map_err(ChdError::Io)?;
    if read < 16 || &header[..8] != MAGIC {
        return Err(ChdError::Unsupported("it is not a CHD file".into()));
    }
    let version = u32::from_be_bytes([header[12], header[13], header[14], header[15]]);
    if version != V5_VERSION {
        // Older versions are read by the `chd` crate; their map is at a fixed place.
        return Ok(());
    }
    if (read as u64) < V5_HEADER_BYTES {
        return Err(ChdError::Damaged("its header is cut short".into()));
    }
    let map_offset = u64::from_be_bytes(header[V5_MAP_OFFSET..V5_MAP_OFFSET + 8].try_into().expect("8 bytes"));
    if map_offset == 0 {
        return Err(ChdError::Damaged("it has no hunk map: the file was never finished or was cut short".into()));
    }
    Ok(())
}

/// The layout of the sectors and their count, from the metadata and the sizes of the header.
fn layout_of(metadata: &[Metadata], hunk_bytes: u64, unit_bytes: u64, logical_bytes: u64) -> Result<(Layout, u64), ChdError> {
    let tag = |entry: &Metadata, name: &[u8; 4]| entry.metatag == u32::from_be_bytes(*name);
    if metadata.iter().any(|entry| tag(entry, b"CHGD") || tag(entry, b"CHGT")) {
        return Err(ChdError::Unsupported("it is a GD-ROM image".into()));
    }
    if metadata.iter().any(|entry| tag(entry, b"GDDD") || tag(entry, b"AVAV") || tag(entry, b"AVLD")) {
        return Err(ChdError::Unsupported("it is a hard disk or LaserDisc image".into()));
    }
    if metadata.iter().any(|entry| tag(entry, b"CHCD")) {
        return Err(ChdError::Unsupported("it has the old binary CD track list; make it again with a current chdman".into()));
    }
    let tracks: Vec<CdTrack> = metadata
        .iter()
        .filter(|entry| KnownMetadata::is_cdrom(entry.metatag))
        .map(|entry| CdTrack::parse(&entry.value))
        .collect::<Result<_, _>>()?;
    if tracks.is_empty() {
        // A DVD: whole 2048-byte sectors, hunks of whole sectors.
        if unit_bytes != SECTOR_BYTES || hunk_bytes % SECTOR_BYTES != 0 || logical_bytes % SECTOR_BYTES != 0 {
            return Err(ChdError::Unsupported(format!(
                "it is not a disc image (units of {unit_bytes} bytes, hunks of {hunk_bytes} bytes, {logical_bytes} bytes)"
            )));
        }
        return Ok((Layout::Dvd, logical_bytes / SECTOR_BYTES));
    }
    if unit_bytes != u64::from(CD_FRAME_BYTES) || hunk_bytes % u64::from(CD_FRAME_BYTES) != 0 {
        return Err(ChdError::Unsupported(format!("its CD frames are {unit_bytes} bytes in hunks of {hunk_bytes} bytes")));
    }
    let Some(first) = tracks.iter().find(|track| track.number == 1) else {
        return Err(ChdError::Unsupported("its track list has no track 1".into()));
    };
    let Some(user_data_offset) = user_data_offset(&first.kind) else {
        return Err(ChdError::Unsupported(format!("its first track is {}, not a data track", first.kind)));
    };
    // Track 1 starts at frame 0 of the CHD. A pregap stored in the file (`PGTYPE:V...`) comes first and is counted in FRAMES.
    let stored_pregap = if first.pregap_in_file { first.pregap } else { 0 };
    if stored_pregap >= first.frames {
        return Err(ChdError::Unsupported("its first track has no data frames".into()));
    }
    let sectors = first.frames - stored_pregap;
    if (stored_pregap + sectors) * u64::from(CD_FRAME_BYTES) > logical_bytes {
        return Err(ChdError::Damaged("its first track is longer than its data".into()));
    }
    Ok((Layout::Cd { first_frame: stored_pregap, user_data_offset }, sectors))
}

/// Where the 2048 user bytes are in a stored frame of a data track of this type (the layout chdman writes: a track of a
/// smaller sector size is stored at the start of the 2352-byte frame).
fn user_data_offset(kind: &str) -> Option<u32> {
    match kind {
        "MODE1" | "MODE2_FORM1" => Some(0),
        "MODE2" => Some(8),
        "MODE1_RAW" => Some(16),
        "MODE2_RAW" => Some(24),
        _ => None,
    }
}

/// One entry of the text track list of a CD CHD (`CHTR` or `CHT2`).
#[derive(Clone, Debug, PartialEq, Eq)]
struct CdTrack {
    number: u32,
    kind: String,
    frames: u64,
    pregap: u64,
    /// `PGTYPE` starts with `V`: the pregap frames are stored in the file.
    pregap_in_file: bool,
}

impl CdTrack {
    /// Parses `TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:1234 PREGAP:0 PGTYPE:MODE2_RAW PGSUB:NONE POSTGAP:0` (the `CHT2`
    /// form; `CHTR` stops after FRAMES). The text may end with a NUL.
    fn parse(value: &[u8]) -> Result<CdTrack, ChdError> {
        let text = String::from_utf8_lossy(value);
        let text = text.trim_end_matches('\0');
        let field = |name: &str| {
            text.split_whitespace().find_map(|part| part.strip_prefix(name).and_then(|rest| rest.strip_prefix(':')))
        };
        let number = |name: &str| -> Result<u64, ChdError> {
            field(name)
                .ok_or_else(|| ChdError::Damaged(format!("a track entry has no {name}: {text}")))?
                .parse::<u64>()
                .map_err(|_| ChdError::Damaged(format!("a track entry has a bad {name}: {text}")))
        };
        let kind = field("TYPE").ok_or_else(|| ChdError::Damaged(format!("a track entry has no TYPE: {text}")))?;
        let track = number("TRACK")?;
        Ok(CdTrack {
            number: u32::try_from(track).map_err(|_| ChdError::Damaged(format!("a track entry has a bad TRACK: {text}")))?,
            kind: kind.to_string(),
            frames: number("FRAMES")?,
            pregap: if field("PREGAP").is_some() { number("PREGAP")? } else { 0 },
            pregap_in_file: field("PGTYPE").is_some_and(|value| value.starts_with('V')),
        })
    }
}

/// How a [`convert`] run ended without a finished image.
#[derive(Debug)]
pub enum ConvertError {
    Chd(ChdError),
    Cancelled,
}

impl From<ChdError> for ConvertError {
    fn from(error: ChdError) -> Self {
        ConvertError::Chd(error)
    }
}

impl From<io::Error> for ConvertError {
    fn from(error: io::Error) -> Self {
        ConvertError::Chd(ChdError::Io(error))
    }
}

/// Decompresses the CHD at `source` into the plain ISO `target`, and returns the size and SHA-256 of that ISO.
///
/// Every hunk is decompressed once, in order: each one passes its own check in the `chd` crate, and the whole logical data
/// must match the SHA-1 in the header (CHD v4 and v5 record the SHA-1 of the data alone, v3 of the data). `progress` gets
/// the fraction done; `cancel` stops the run between hunks. On any error `target` is removed, so a half-written image is
/// never left behind.
pub fn convert(source: &Path, target: &Path, cancel: &AtomicBool, progress: &mut dyn FnMut(f32)) -> Result<Identity, ConvertError> {
    let result = convert_into(source, target, cancel, progress);
    if result.is_err() {
        let _ = fs::remove_file(target);
    }
    result
}

fn convert_into(source: &Path, target: &Path, cancel: &AtomicBool, progress: &mut dyn FnMut(f32)) -> Result<Identity, ConvertError> {
    let mut disc = ChdDisc::open(source)?;
    let expected_sha1 = match disc.chd.header() {
        Header::V3Header(_) => disc.chd.header().sha1(),
        _ => disc.chd.header().raw_sha1(),
    };
    let hunk_count = disc.chd.header().hunk_count();
    let hunk_bytes = disc.hunk_bytes;
    let logical_bytes = disc.logical_bytes;
    let (first_sector_byte, end_sector_byte) = (disc.logical_offset(0), disc.logical_offset(disc.sectors));
    let mut output = BufWriter::with_capacity(4 * 1024 * 1024, File::create(target)?);
    let mut raw_sha1 = Sha1::new();
    let mut iso_sha256 = Sha256::new();
    let mut written = 0u64;

    for number in 0..hunk_count {
        if number % HUNKS_PER_REPORT == 0 {
            if cancel.load(Ordering::Relaxed) {
                return Err(ConvertError::Cancelled);
            }
            progress(number as f32 / hunk_count as f32);
        }
        disc.load_hunk(number).map_err(|error| match is_damaged(&error) {
            true => ChdError::Damaged(error.to_string()),
            false => ChdError::Io(error),
        })?;
        let hunk_start = u64::from(number) * hunk_bytes;
        let valid = hunk_bytes.min(logical_bytes.saturating_sub(hunk_start)) as usize;
        raw_sha1.update(&disc.hunk[..valid]);
        match disc.layout {
            Layout::Dvd => {
                output.write_all(&disc.hunk[..valid])?;
                iso_sha256.update(&disc.hunk[..valid]);
                written += valid as u64;
            }
            Layout::Cd { user_data_offset, .. } => {
                // Each whole frame of the hunk that belongs to the data of the first track gives one sector.
                let frame_bytes = u64::from(CD_FRAME_BYTES);
                let mut frame_start = hunk_start;
                while frame_start + frame_bytes <= hunk_start + valid as u64 {
                    let sector_start = frame_start + u64::from(user_data_offset);
                    if sector_start >= first_sector_byte && sector_start < end_sector_byte {
                        let at = (sector_start - hunk_start) as usize;
                        let sector = &disc.hunk[at..at + SECTOR_BYTES as usize];
                        output.write_all(sector)?;
                        iso_sha256.update(sector);
                        written += SECTOR_BYTES;
                    }
                    frame_start += frame_bytes;
                }
            }
        }
    }
    if written != disc.iso_bytes() {
        return Err(ChdError::Damaged(format!("{written} bytes of data, expected {}", disc.iso_bytes())).into());
    }
    if let Some(expected) = expected_sha1.filter(|sha1| sha1.iter().any(|byte| *byte != 0)) {
        let found: [u8; 20] = raw_sha1.finalize().into();
        if found != expected {
            return Err(ChdError::Damaged("its data differs from the SHA-1 in its header".into()).into());
        }
    }
    output.flush()?;
    output.into_inner().map_err(|error| error.into_error())?.sync_all()?;
    progress(1.0);
    Ok(Identity { size_bytes: written, sha256: crate::setup::hex(&iso_sha256.finalize()) })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_chd_extension_is_recognised_without_case() {
        assert!(is_chd(Path::new("Gran Turismo 4.chd")));
        assert!(is_chd(Path::new("GT4.CHD")));
        assert!(!is_chd(Path::new("GT4.iso")));
        assert!(!is_chd(Path::new("GT4")));
    }

    #[test]
    fn a_cht2_track_entry_is_parsed() {
        let track =
            CdTrack::parse(b"TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:1234 PREGAP:150 PGTYPE:VMODE2_RAW PGSUB:RW POSTGAP:0\0").unwrap();
        assert_eq!(
            track,
            CdTrack { number: 1, kind: "MODE2_RAW".into(), frames: 1234, pregap: 150, pregap_in_file: true }
        );
        let old = CdTrack::parse(b"TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:99").unwrap();
        assert_eq!(old, CdTrack { number: 2, kind: "AUDIO".into(), frames: 99, pregap: 0, pregap_in_file: false });
        assert!(matches!(CdTrack::parse(b"TRACK:1 TYPE:MODE1"), Err(ChdError::Damaged(_))));
    }

    #[test]
    fn the_user_data_of_each_data_track_type() {
        assert_eq!(user_data_offset("MODE1"), Some(0));
        assert_eq!(user_data_offset("MODE2_FORM1"), Some(0));
        assert_eq!(user_data_offset("MODE2"), Some(8));
        assert_eq!(user_data_offset("MODE1_RAW"), Some(16));
        assert_eq!(user_data_offset("MODE2_RAW"), Some(24));
        assert_eq!(user_data_offset("AUDIO"), None);
        assert_eq!(user_data_offset("MODE2_FORM2"), None);
    }
}
