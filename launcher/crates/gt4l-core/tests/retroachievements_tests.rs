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
#[test]
fn hostile_inherited_retroachievements_environment_with_case_variations_is_scrubbed_and_redacted() {
    let case_variations = [
        "gt4_ra_token",
        "GT4_RA_TOKEN",
        "Gt4_Ra_Token",
        "gt4_ra_password",
        "GT4_RA_PASSWORD",
        "Gt4_Ra_Password",
        "gt4_ra_enabled",
        "GT4_RA_ENABLED",
        "Gt4_Ra_Enabled",
        "gt4_ra_hash",
        "GT4_RA_HASH",
        "gt4_ra_host",
        "GT4_RA_HOST",
        "ps2_disc",
        "PS2_DISC",
        "Ps2_Disc",
    ];

    for key in case_variations {
        assert!(launch::is_scrubbed(key), "Case-variant hostile key {key} must be scrubbed");
    }

    // Passed-through keys stay passed through regardless of case
    assert!(!launch::is_scrubbed("GT4_ASPECT_FOV_EXTRA"));
    assert!(!launch::is_scrubbed("gt4_aspect_fov_extra"));

    let root = temp_dir("case_scrub");
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
    let inherited: Vec<String> = case_variations.iter().map(|s| s.to_string()).collect();

    let mut plan = launch::plan_pcsx2_launch(&inputs, &settings, &pcsx2_paths, None, &inherited);

    // Verify all case-variant hostile keys were placed into removed_inherited
    for key in case_variations {
        assert!(plan.removed_inherited.contains(&key.to_string()), "{key} was not removed from inherited env");
        assert!(!plan.environment.iter().any(|(k, _)| k == key), "{key} should not be in launch plan env");
    }

    // Now inject authorized RA environment into plan and test Debug redaction
    plan.environment.push(("GT4_RA_ENABLED".to_string(), "1".to_string()));
    plan.environment.push(("GT4_RA_USERNAME".to_string(), "Pilot99".to_string()));
    plan.environment.push(("GT4_RA_TOKEN".to_string(), "super_secret_auth_token_xyz".to_string()));

    let debug_output = format!("{:?}", plan);
    assert!(debug_output.contains("[REDACTED]"), "Debug output of LaunchPlan must redact token");
    assert!(!debug_output.contains("super_secret_auth_token_xyz"), "Debug output must NEVER leak secret token!");

    let _ = fs::remove_dir_all(root);
}

#[test]
fn fake_server_http_login_refresh_and_failure_scenarios() {
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::thread;

    let listener = TcpListener::bind("127.0.0.1:0").expect("bind ephemeral port");
    let port = listener.local_addr().unwrap().port();
    let base_url = format!("http://127.0.0.1:{port}");
    let stop = Arc::new(AtomicBool::new(false));
    let stop_clone = stop.clone();

    let server_thread = thread::spawn(move || {
        listener.set_nonblocking(true).unwrap();
        while !stop_clone.load(Ordering::Relaxed) {
            match listener.accept() {
                Ok((mut stream, _)) => {
                    let mut buf = [0u8; 4096];
                    let mut read_bytes = 0;
                    while read_bytes < buf.len() {
                        match stream.read(&mut buf[read_bytes..]) {
                            Ok(0) => break,
                            Ok(n) => {
                                read_bytes += n;
                                let text = String::from_utf8_lossy(&buf[..read_bytes]);
                                if let Some(pos) = text.find("\r\n\r\n") {
                                    let cl = text.lines()
                                        .find(|l| l.to_ascii_lowercase().starts_with("content-length:"))
                                        .and_then(|l| l.split(':').nth(1))
                                        .and_then(|s| s.trim().parse::<usize>().ok())
                                        .unwrap_or(0);
                                    if read_bytes >= pos + 4 + cl {
                                        break;
                                    }
                                }
                            }
                            Err(_) => break,
                        }
                    }
                    let req = String::from_utf8_lossy(&buf[..read_bytes]);
                    if req.contains("mode_delay") {
                        thread::sleep(std::time::Duration::from_millis(60));
                    }
                    if req.contains("mode_fail") {
                        let body = r#"{"Success":false,"Error":"Invalid user or password."}"#;
                        let resp = format!("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body);
                        let _ = stream.write_all(resp.as_bytes());
                        let _ = stream.flush();
                        let _ = stream.shutdown(std::net::Shutdown::Write);
                        thread::sleep(std::time::Duration::from_millis(15));
                    } else if req.contains("mode_abort") {
                        // Drop stream immediately simulating network disconnect
                        drop(stream);
                    } else {
                        // Success response
                        let body = r#"{"Success":true,"User":"FakeDriver","Token":"tok_fake_789","Score":1500,"SoftcoreScore":200}"#;
                        let resp = format!("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body);
                        let _ = stream.write_all(resp.as_bytes());
                        let _ = stream.flush();
                        let _ = stream.shutdown(std::net::Shutdown::Write);
                        thread::sleep(std::time::Duration::from_millis(15));
                    }
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    thread::sleep(std::time::Duration::from_millis(5));
                }
                Err(_) => break,
            }
        }
    });

    // 1. Password login success
    let session = gt4l_core::retroachievements::login_with_password(&base_url, "FakeDriver", "pass123")
        .expect("login with password succeeds against fake server");
    assert_eq!(session.username, "FakeDriver");
    assert_eq!(session.token, "tok_fake_789");
    assert_eq!(session.score, 1500);

    // 2. Token login success
    let token_session = gt4l_core::retroachievements::login_with_token(&base_url, "FakeDriver", "tok_fake_789")
        .expect("token login succeeds against fake server");
    assert_eq!(token_session.username, "FakeDriver");
    assert_eq!(token_session.token, "tok_fake_789");

    // 3. Delayed login completes cleanly
    let delayed = gt4l_core::retroachievements::login_with_password(&base_url, "FakeDriver", "pass_mode_delay")
        .expect("delayed login succeeds");
    assert_eq!(delayed.username, "FakeDriver");

    // 4. Failed login returns explicit server error
    let fail_res = gt4l_core::retroachievements::login_with_password(&base_url, "FakeDriver", "badpass_mode_fail");
    assert!(fail_res.is_err(), "Invalid credentials must return Err");
    assert!(fail_res.unwrap_err().contains("Invalid user or password"));

    // 5. Aborted connection returns clean error without panic
    let abort_res = gt4l_core::retroachievements::login_with_password(&base_url, "FakeDriver", "pass_mode_abort");
    assert!(abort_res.is_err(), "Aborted connection must return Err");

    stop.store(true, Ordering::Relaxed);
    let _ = server_thread.join();
}

#[test]
fn retroachievements_config_125_compatibility_and_malformed_handling() {
    let root = temp_dir("config_compat");

    // 1. Valid 1.2.5 format with comments and future unknown fields
    let toml_125 = r#"
# RetroAchievements configuration for GT4Recomp 1.2.5
username = "VeteranPilot"
token = "tok_compat_abc"
enabled = true
hardcore = true
encore = false
unknown_future_field = "ignored_safely"

[future_section]
feature_x = 42
"#;
    let cfg_file = root.join("retroachievements.toml");
    fs::write(&cfg_file, toml_125).unwrap();

    let loaded = RaConfig::load(&cfg_file);
    assert_eq!(loaded.username, "VeteranPilot");
    assert_eq!(loaded.token, "tok_compat_abc");
    assert!(loaded.enabled);
    assert!(loaded.hardcore);
    assert!(!loaded.encore);

    // 2. Malformed / corrupted TOML falls back to safe defaults without crashing
    let corrupt_file = root.join("corrupt.toml");
    fs::write(&corrupt_file, b"this is not valid toml = [[{\x00\xFF").unwrap();
    let corrupt_loaded = RaConfig::load(&corrupt_file);
    assert_eq!(corrupt_loaded.username, "");
    assert_eq!(corrupt_loaded.token, "");
    assert!(corrupt_loaded.enabled); // default enabled is true
    assert!(!corrupt_loaded.hardcore);

    // 3. Empty file falls back to defaults
    let empty_file = root.join("empty.toml");
    fs::write(&empty_file, b"").unwrap();
    let empty_loaded = RaConfig::load(&empty_file);
    assert_eq!(empty_loaded.username, "");
    assert_eq!(empty_loaded.token, "");

    let _ = fs::remove_dir_all(root);
}

#[test]
fn diagnostic_export_exclusions_comprehensive() {
    // Explicitly forbidden paths
    assert!(log_export::is_forbidden("retroachievements.toml"));
    assert!(log_export::is_forbidden("retroachievements.toml.tmp"));
    assert!(log_export::is_forbidden("retroachievements.toml.bak"));
    assert!(log_export::is_forbidden("retroachievements.toml.old"));
    assert!(log_export::is_forbidden("config/retroachievements.toml"));
    assert!(log_export::is_forbidden("user/config/retroachievements.toml"));
    assert!(log_export::is_forbidden(r"C:\Users\Pilot\AppData\Roaming\GT4Recomp\config\retroachievements.toml"));

    // Safe, non-forbidden diagnostic files
    assert!(!log_export::is_forbidden("game.log"));
    assert!(!log_export::is_forbidden("misses.txt"));
    assert!(!log_export::is_forbidden("probe.log"));
    assert!(!log_export::is_forbidden("achievements_summary.txt"));
}
