//! The launcher UI locale files (`launcher/crates/gt4l-tauri/ui/locales/<tag>.json`) hold every player-facing text:
//! the pages read them with `GT4.t`, and `build.rs` takes the `rust.*` keys for the messages Rust shows. `messages.rs`
//! tests the `rust.*` keys only; these tests cover the whole set, because a page key has no other check: every language
//! has the same keys and `{placeholders}`, no file repeats a key, no player text names an internal task, and every key a
//! page asks for exists.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

/// The folder that holds `index.json` and the locale files, from this crate.
fn locales_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../gt4l-tauri/ui/locales")
}

/// The UI folder of the launcher, which holds the pages and `index.html`.
fn ui_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../gt4l-tauri/ui")
}

/// The language tags of `index.json`, in their order: the first one is the fallback.
fn languages() -> Vec<String> {
    let path = locales_dir().join("index.json");
    let text = fs::read_to_string(&path).unwrap_or_else(|error| panic!("{} is not readable: {error}", path.display()));
    let list: Vec<serde_json::Value> =
        serde_json::from_str(&text).unwrap_or_else(|error| panic!("{} is not valid JSON: {error}", path.display()));
    list.iter()
        .map(|entry| entry["tag"].as_str().unwrap_or_else(|| panic!("{} has an entry without a tag", path.display())).to_string())
        .collect()
}

/// The texts of one language, and every key that appears more than once in its file.
fn read_locale(tag: &str) -> (BTreeMap<String, String>, Vec<String>) {
    let path = locales_dir().join(format!("{tag}.json"));
    let text = fs::read_to_string(&path).unwrap_or_else(|error| panic!("{} is not readable: {error}", path.display()));
    let object: serde_json::Map<String, serde_json::Value> = serde_json::from_str(&text)
        .unwrap_or_else(|error| panic!("{} is not valid JSON: {error}", path.display()));
    let mut texts = BTreeMap::new();
    for (key, value) in object {
        let text = value.as_str().unwrap_or_else(|| panic!("{key} is not a string in {tag}"));
        texts.insert(key, text.to_string());
    }
    (texts, repeated_keys(&text))
}

/// The keys written more than once in a locale file. `serde_json` keeps the last one silently, which would hide a
/// translation behind an older text; the files keep one `"key": "text",` per line.
fn repeated_keys(text: &str) -> Vec<String> {
    let mut counts: BTreeMap<&str, u32> = BTreeMap::new();
    for line in text.lines() {
        let Some(rest) = line.trim_start().strip_prefix('"') else { continue };
        let Some(end) = rest.find("\":") else { continue };
        *counts.entry(&rest[..end]).or_insert(0) += 1;
    }
    counts.into_iter().filter(|(_, count)| *count > 1).map(|(key, _)| key.to_string()).collect()
}

/// The `{name}` placeholders of a text, sorted, without duplicates.
fn placeholders(text: &str) -> Vec<String> {
    let mut names = Vec::new();
    let mut rest = text;
    while let Some(start) = rest.find('{') {
        let Some(end) = rest[start..].find('}') else { break };
        names.push(rest[start + 1..start + end].to_string());
        rest = &rest[start + end + 1..];
    }
    names.sort();
    names.dedup();
    names
}

#[test]
fn every_language_has_the_same_keys_and_placeholders() {
    let tags = languages();
    let fallback_tag = &tags[0];
    let (fallback, repeated) = read_locale(fallback_tag);
    assert!(fallback.len() > 500, "{fallback_tag} holds the texts of the launcher");
    assert!(repeated.is_empty(), "{fallback_tag} repeats keys: {repeated:?}");
    for (key, text) in &fallback {
        assert!(!text.trim().is_empty(), "{fallback_tag} {key} is empty");
    }
    for tag in &tags[1..] {
        let (texts, repeated) = read_locale(tag);
        assert!(repeated.is_empty(), "{tag} repeats keys: {repeated:?}");
        let missing: Vec<&String> = fallback.keys().filter(|key| !texts.contains_key(*key)).collect();
        assert!(missing.is_empty(), "{tag} has no text for {missing:?}");
        let extra: Vec<&String> = texts.keys().filter(|key| !fallback.contains_key(*key)).collect();
        assert!(extra.is_empty(), "{tag} has keys {fallback_tag} does not: {extra:?}");
        for (key, text) in &texts {
            assert!(!text.trim().is_empty(), "{tag} {key} is empty");
            assert_eq!(placeholders(text), placeholders(&fallback[key]), "{tag} {key} uses other placeholders");
        }
    }
}

#[test]
fn no_player_text_names_an_internal_task() {
    for tag in languages() {
        let (texts, _) = read_locale(&tag);
        for (key, text) in &texts {
            assert!(!text.contains("TASK-"), "{tag} {key} names an internal task: {text}");
        }
    }
}

#[test]
fn every_key_a_page_asks_for_exists() {
    let (fallback, _) = read_locale(&languages()[0]);
    let mut sources = Vec::new();
    collect_sources(&ui_dir().join("js"), &mut sources);
    sources.push(ui_dir().join("index.html"));
    let mut missing = Vec::new();
    for path in &sources {
        let text = fs::read_to_string(path).unwrap_or_else(|error| panic!("{} is not readable: {error}", path.display()));
        for key in asked_keys(&text) {
            if !fallback.contains_key(&key) {
                missing.push(format!("{}: {key}", path.display()));
            }
        }
    }
    assert!(missing.is_empty(), "the pages ask for keys that no locale has:\n{}", missing.join("\n"));
}

/// Adds every `.js` file under `folder` (recursively) to `out`.
fn collect_sources(folder: &Path, out: &mut Vec<PathBuf>) {
    let entries = fs::read_dir(folder).unwrap_or_else(|error| panic!("{} is not readable: {error}", folder.display()));
    for entry in entries {
        let path = entry.expect("a folder entry").path();
        if path.is_dir() {
            collect_sources(&path, out);
        } else if path.extension().is_some_and(|extension| extension == "js") {
            out.push(path);
        }
    }
    out.sort();
}

/// The literal keys of the `t("...")` calls in a page source (`GT4.t("home.play")`, or a page's own `t("ra.title")`).
/// A call starts at a boundary, so the `t("` inside `format("...")` and `split("...")` is not one; a key that ends with
/// a dot is a prefix built at run time (`"choice.update_channel." + channel`) and is skipped.
fn asked_keys(source: &str) -> Vec<String> {
    let mut keys = Vec::new();
    let mut from = 0;
    while let Some(found) = source[from..].find("t(\"") {
        let start = from + found;
        from = start + 3;
        if source[..start].chars().next_back().is_some_and(|before| before.is_alphanumeric() || before == '_') {
            continue;
        }
        let rest = &source[start + 3..];
        let Some(end) = rest.find('"') else { break };
        let key = &rest[..end];
        if key.is_empty() || key.ends_with('.') || !key.contains('.') {
            continue;
        }
        if !key.chars().all(|character| character.is_ascii_alphanumeric() || matches!(character, '.' | '_' | '-')) {
            continue;
        }
        keys.push(key.to_string());
    }
    keys
}
