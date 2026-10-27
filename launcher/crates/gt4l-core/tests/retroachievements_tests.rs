use std::fs;
use std::path::{Path, PathBuf};

use gt4l_core::games::{GT4, SPEC2};
use gt4l_core::language::Language;
use gt4l_core::launch;
use gt4l_core::log_export;
use gt4l_core::paths::{DataDirs, UserDirs, UserRootSource};
use gt4l_core::retroachievements::RaConfig;

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("gt4-core-ra-test-{}-{name}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn retroachievements_config_roundtrips_in_installed_and_portable_layouts() {
    let root = temp_dir("layouts");

    // 1. Installed layout (%APPDATA%\GT4Recomp\config)
    let appdata = root.join("AppData").join("Roaming").join("GT4Recomp");
    let user_installed = UserDirs::new(appdata.clone(), UserRootSource::AppData);
    let ra_file_installed = user_installed.retroachievements_file();
    assert_eq!(ra_file_installed, appdata.join("config").join("retroachievements.toml"));

    let mut cfg = RaConfig::default();
    cfg.username = "Driver99".to_string();
    cfg.token = "tok_installed_abc".to_string();
    cfg.hardcore = true;
    cfg.save(&ra_file_installed).unwrap();

    let loaded = RaConfig::load(&ra_file_installed);
    assert_eq!(loaded.username, "Driver99");
    assert_eq!(loaded.token, "tok_installed_abc");
    assert!(loaded.hardcore);

    // 2. Portable layout (<install>\user\config)
    let portable_user = root.join("portable").join("user");
    let user_portable = UserDirs::new(portable_user.clone(), UserRootSource::Override);
    let ra_file_portable = user_portable.retroachievements_file();
    assert_eq!(ra_file_portable, portable_user.join("config").join("retroachievements.toml"));

    cfg.username = "PortableDriver".to_string();
    cfg.token = "tok_portable_xyz".to_string();
    cfg.hardcore = false;
    cfg.encore = true;
    cfg.save(&ra_file_portable).unwrap();

    let loaded_portable = RaConfig::load(&ra_file_portable);
    assert_eq!(loaded_portable.username, "PortableDriver");
    assert_eq!(loaded_portable.token, "tok_portable_xyz");
    assert!(!loaded_portable.hardcore);
    assert!(loaded_portable.encore);

    let _ = fs::remove_dir_all(root);
}

#[test]
fn retroachievements_cache_explicitly_resolves_under_gt4_in_modern_layout() {
    let root = temp_dir("cache");
    let data_root = root.join("data");
    let dirs = DataDirs::new(&data_root);

    // Default dirs is GT4
    let cache_gt4 = dirs.retroachievements_cache();
    assert_eq!(cache_gt4, data_root.join("games").join("gt4").join("runtime").join("ra_cache"));

    // dirs switched to SPEC2 still resolves cache explicitly to GT4
    let dirs_spec2 = dirs.for_game(&SPEC2);
    let cache_spec2 = dirs_spec2.retroachievements_cache();
    assert_eq!(cache_spec2, data_root.join("games").join("gt4").join("runtime").join("ra_cache"));

    let _ = fs::remove_dir_all(root);
}

#[test]
fn switching_games_preserves_shared_retroachievements_file() {
    let root = temp_dir("shared_ra_file");
    let user = UserDirs::new(root.join("user"), UserRootSource::Override);

    let file_gt4 = user.for_game(&GT4).retroachievements_file();
    let file_spec2 = user.for_game(&SPEC2).retroachievements_file();

    assert_eq!(file_gt4, file_spec2, "retroachievements.toml path must be shared across games");
    assert_eq!(file_gt4, root.join("user").join("config").join("retroachievements.toml"));

    let _ = fs::remove_dir_all(root);
}

#[test]
fn hostile_inherited_retroachievements_environment_is_scrubbed() {
    let hostile_keys = [
        "GT4_RA_ENABLED",
        "GT4_RA_HARDCORE",
        "GT4_RA_ENCORE",
        "GT4_RA_USERNAME",
        "GT4_RA_TOKEN",
        "GT4_RA_PASSWORD",
        "GT4_RA_HASH",
        "GT4_RA_HOST",
        "GT4_RA_CACHE_DIR",
        "GT4_RA_LANGUAGE",
    ];

    for key in hostile_keys {
        assert!(launch::is_scrubbed(key), "Hostile key {key} must be scrubbed");
    }

    // Verify plan_pcsx2_launch removes all hostile keys into removed_inherited
    let root = temp_dir("launch_scrub");
    let game_dir = root.join("game");
    fs::create_dir_all(&game_dir).unwrap();

    let exe = game_dir.join("gt4vulkan.exe");
    let elf = game_dir.join("SCUS_973.28");
    let disc = root.join("disc.iso");
    let card = root.join("card.ps2");
    let session = root.join("session");
    fs::create_dir_all(&session).unwrap();

    let inputs = launch::LaunchInputs {
        game_executable: &exe,
        elf: &elf,
        disc: &disc,
        card: &card,
        session_dir: &session,
    };
    let settings = gt4l_core::settings::Settings::default();
    let textures = root.join("textures");
    let gs_exe = root.join("gs").join("v1.0.0").join("gt4-gs.exe");
    let installed = gt4l_core::gs_release::Installed {
        version: gt4l_core::gs_release::MINIMUM_VERSION,
        executable: gs_exe,
        executable_sha256: "0".repeat(64),
    };
    let pcsx2_paths = launch::Pcsx2Paths::for_session(&installed, &root.join("gs"), &session, &textures);

    let inherited: Vec<String> = hostile_keys.iter().map(|s| s.to_string()).collect();
    let plan = launch::plan_pcsx2_launch(&inputs, &settings, &pcsx2_paths, None, &inherited);

    for key in hostile_keys {
        assert!(plan.removed_inherited.contains(&key.to_string()), "{key} was not removed from inherited env");
        assert!(!plan.environment.iter().any(|(k, _)| k == key), "{key} should not be in pure launch plan env");
    }

    let _ = fs::remove_dir_all(root);
}

#[test]
fn diagnostic_export_forbids_retroachievements_file() {
    assert!(log_export::is_forbidden("retroachievements.toml"));
    assert!(log_export::is_forbidden("C:\\Users\\User\\AppData\\Roaming\\GT4Recomp\\config\\retroachievements.toml"));
    assert!(log_export::is_forbidden("user/config/retroachievements.toml"));
    assert!(log_export::is_forbidden("retroachievements.toml.tmp"));
}

#[test]
fn launch_environment_includes_language_tag() {
    let mut cfg = RaConfig::default();
    cfg.enabled = true;
    cfg.username = "Driver".to_string();
    cfg.token = "tok".to_string();

    let cache = Path::new("C:\\cache");
    let en_vars = cfg.launch_environment(cache, Language::fallback());
    let lang_en = en_vars.iter().find(|(k, _)| k == "GT4_RA_LANGUAGE").unwrap();
    assert_eq!(lang_en.1, "en-US");

    let pt = Language::from_tag("pt-BR").unwrap();
    let pt_vars = cfg.launch_environment(cache, pt);
    let lang_pt = pt_vars.iter().find(|(k, _)| k == "GT4_RA_LANGUAGE").unwrap();
    assert_eq!(lang_pt.1, "pt-BR");
}
