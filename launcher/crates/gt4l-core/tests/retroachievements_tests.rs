use std::fs;
use std::path::{Path, PathBuf};

use gt4l_core::games::{GT4, SPEC2};
use gt4l_core::language::Language;
use gt4l_core::launch;
use gt4l_core::log_export;
use gt4l_core::paths::{DataDirs, UserDirs, UserRootSource};
use gt4l_core::retroachievements::RaConfig;
use gt4l_core::secret::SecretProtector;

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

#[test]
fn fake_protector_encrypt_decrypt_and_injected_failures() {
    let protector = gt4l_core::secret::FakeProtector::default();

    // 1. Normal round-trip
    let plaintext = "secret_session_token_12345";
    let envelope = protector.protect_secret(plaintext).expect("protect succeeds");
    assert!(envelope.starts_with(gt4l_core::secret::FAKE_V1_PREFIX));
    let decrypted = protector.unprotect_secret(&envelope).expect("unprotect succeeds");
    assert_eq!(decrypted, plaintext);

    // 2. Injected protect failure
    let fail_protect = gt4l_core::secret::FakeProtector { fail_protect: true, ..gt4l_core::secret::FakeProtector::default() };
    let err = fail_protect.protect_secret(plaintext).unwrap_err();
    assert!(matches!(err, gt4l_core::secret::SecretError::BackendUnavailable(_)));

    // 3. Injected unprotect failure (simulating NTE_BAD_KEYSET / wrong user)
    let fail_unprotect = gt4l_core::secret::FakeProtector { fail_unprotect: true, wrong_user_code: 0x8009000B, ..gt4l_core::secret::FakeProtector::default() };
    let err2 = fail_unprotect.unprotect_secret(&envelope).unwrap_err();
    match err2 {
        gt4l_core::secret::SecretError::OsError { code, .. } => assert_eq!(code, 0x8009000B),
        other => panic!("expected OsError, got {:?}", other),
    }

    // 4. Corrupted envelopes
    assert!(matches!(
        protector.unprotect_secret("wrong_prefix:v1:010203").unwrap_err(),
        gt4l_core::secret::SecretError::CorruptData(_)
    ));
    assert!(matches!(
        protector.unprotect_secret("fake:v1:not_hex_xyz").unwrap_err(),
        gt4l_core::secret::SecretError::CorruptData(_)
    ));
    assert!(matches!(
        protector.unprotect_secret("fake:v1:123").unwrap_err(),
        gt4l_core::secret::SecretError::CorruptData(_)
    ));
}

#[test]
fn real_windows_dpapi_roundtrip_tampered_blob_and_empty_handling() {
    let protector = gt4l_core::secret::DpapiProtector;

    // 1. Normal DPAPI roundtrip
    let token = "ra_prod_token_test_abc_888";
    let envelope = protector.protect_secret(token).expect("DPAPI protect succeeds");
    assert!(envelope.starts_with(gt4l_core::secret::DPAPI_V1_PREFIX));
    let decrypted = protector.unprotect_secret(&envelope).expect("DPAPI unprotect succeeds");
    assert_eq!(decrypted, token);

    // 2. Empty string roundtrip
    let empty_env = protector.protect_secret("").expect("protect empty");
    assert_eq!(protector.unprotect_secret(&empty_env).expect("unprotect empty"), "");

    // 3. Tampered ciphertext rejection (verifying OS integrity check)
    let hex_part = &envelope[gt4l_core::secret::DPAPI_V1_PREFIX.len()..];
    let mut raw_bytes = gt4l_core::secret::hex_decode(hex_part).expect("hex decode");
    if let Some(b) = raw_bytes.last_mut() {
        *b ^= 0xFF;
    }
    let tampered_hex = gt4l_core::secret::hex_encode(&raw_bytes);
    let tampered_env = format!("{}{}", gt4l_core::secret::DPAPI_V1_PREFIX, tampered_hex);
    let tampered_res = protector.unprotect_secret(&tampered_env);
    assert!(tampered_res.is_err(), "Tampered ciphertext must be rejected by CryptUnprotectData");
    match tampered_res.unwrap_err() {
        gt4l_core::secret::SecretError::OsError { .. } => {}
        other => panic!("expected OsError from tampered DPAPI blob, got {:?}", other),
    }

    // 4. Invalid envelope prefix and invalid hex
    assert!(matches!(
        protector.unprotect_secret("dpapi:v2:0102").unwrap_err(),
        gt4l_core::secret::SecretError::CorruptData(_)
    ));
    assert!(matches!(
        protector.unprotect_secret("dpapi:v1:invalid_non_hex").unwrap_err(),
        gt4l_core::secret::SecretError::CorruptData(_)
    ));
}

#[test]
fn legacy_plaintext_migration_from_124_125_config_and_clean_disk_replacement() {
    let root = temp_dir("migration_clean");
    let cfg_path = root.join("retroachievements.toml");

    let legacy_toml = r#"
# RetroAchievements configuration from 1.2.5
username = "VeteranPilot"
token = "tok_plaintext_legacy_999"
display_name = "VeteranPilot"
score = 4500
score_softcore = 350
enabled = true
hardcore = true
encore = false
"#;
    fs::write(&cfg_path, legacy_toml).unwrap();

    let protector = gt4l_core::secret::FakeProtector::default();

    // First load triggers automatic migration
    let loaded = RaConfig::load_with(&cfg_path, &protector);
    assert_eq!(loaded.username, "VeteranPilot");
    assert_eq!(loaded.token, "tok_plaintext_legacy_999");
    assert!(loaded.is_signed_in());
    assert!(loaded.token_protected.is_some());
    assert!(loaded.token_error.is_none());

    // Verify disk content: MUST NOT contain plaintext token, MUST contain protected envelope
    let disk_raw = fs::read_to_string(&cfg_path).unwrap();
    assert!(!disk_raw.contains("tok_plaintext_legacy_999"), "Plaintext token must not remain on disk after migration");
    assert!(disk_raw.contains("token_protected = \"fake:v1:"));
    assert!(!root.join("retroachievements.toml.tmp").exists(), "Temporary migration file must be cleaned");

    // Second load reads directly from the protected envelope
    let reloaded = RaConfig::load_with(&cfg_path, &protector);
    assert_eq!(reloaded.username, "VeteranPilot");
    assert_eq!(reloaded.token, "tok_plaintext_legacy_999");
    assert!(reloaded.is_signed_in());
    assert_eq!(reloaded.score, 4500);

    let _ = fs::remove_dir_all(root);
}

#[test]
fn failed_migration_leaves_original_plaintext_file_untouched() {
    let root = temp_dir("migration_fail");
    let cfg_path = root.join("retroachievements.toml");

    let legacy_toml = r#"username = "SafeUser"
token = "tok_legacy_do_not_destroy"
enabled = true
hardcore = false
"#;
    fs::write(&cfg_path, legacy_toml).unwrap();

    // Use a protector that fails during protection
    let fail_protector = gt4l_core::secret::FakeProtector { fail_protect: true, ..gt4l_core::secret::FakeProtector::default() };
    let loaded = RaConfig::load_with(&cfg_path, &fail_protector);

    // In-memory state: migration failed, so token is not signed in
    assert!(!loaded.is_signed_in());
    assert!(loaded.token_error.is_some());

    // Crucial check: Disk file MUST be 100% untouched!
    let disk_after = fs::read_to_string(&cfg_path).unwrap();
    assert_eq!(disk_after, legacy_toml, "Disk file must remain 100% untouched when migration fails");
    assert!(!root.join("retroachievements.toml.tmp").exists());

    let _ = fs::remove_dir_all(root);
}

#[test]
fn unprotectable_token_preserves_ciphertext_and_non_secret_settings() {
    let root = temp_dir("unprotectable_token");
    let cfg_path = root.join("retroachievements.toml");

    let protector = gt4l_core::secret::FakeProtector::default();

    let mut cfg = RaConfig::default();
    cfg.username = "DriverMovingPC".to_string();
    cfg.display_name = "Driver Moving PC".to_string();
    cfg.token = "tok_valid_before_move".to_string();
    cfg.score = 3200;
    cfg.score_softcore = 400;
    cfg.hardcore = true;
    cfg.encore = true;
    cfg.save_with(&cfg_path, &protector).unwrap();

    // Verify file is saved with protected token
    let disk_before = fs::read_to_string(&cfg_path).unwrap();
    assert!(!disk_before.contains("tok_valid_before_move"));
    assert!(disk_before.contains("token_protected = \"fake:v1:"));

    // Simulate moving portable folder to another Windows machine / user account
    let wrong_user_protector = gt4l_core::secret::FakeProtector { fail_unprotect: true, wrong_user_code: 0x8009000B, ..gt4l_core::secret::FakeProtector::default() };
    let loaded = RaConfig::load_with(&cfg_path, &wrong_user_protector);

    // Account is not signed in, but non-secret fields and ciphertext are intact!
    assert!(!loaded.is_signed_in());
    assert_eq!(loaded.token, "");
    assert!(loaded.token_error.is_some());
    assert!(loaded.token_protected.is_some());
    assert_eq!(loaded.username, "DriverMovingPC");
    assert_eq!(loaded.display_name, "Driver Moving PC");
    assert_eq!(loaded.score, 3200);
    assert_eq!(loaded.score_softcore, 400);
    assert!(loaded.hardcore);
    assert!(loaded.encore);

    // Environment variables for game must be EMPTY because account cannot be unlocked
    let env_vars = loaded.launch_environment(Path::new("C:\\cache"), Language::fallback());
    assert!(env_vars.is_empty(), "Unprotectable token must NOT pass credentials or enable RA for game");

    // Updating a session option (e.g. toggling hardcore) preserves ciphertext on disk!
    let mut updated_options = loaded;
    updated_options.hardcore = false;
    updated_options.save_with(&cfg_path, &wrong_user_protector).unwrap();

    let disk_after = fs::read_to_string(&cfg_path).unwrap();
    assert!(disk_after.contains("token_protected = \"fake:v1:"), "Ciphertext must be preserved when saving options");
    assert!(!disk_after.contains("tok_valid_before_move"), "Plaintext must never be written");
    assert!(disk_after.contains("hardcore = false"), "Option toggle must be updated");

    let _ = fs::remove_dir_all(root);
}

#[test]
fn real_windows_dpapi_end_to_end_migration_and_persistence() {
    let root = temp_dir("dpapi_e2e");
    let cfg_path = root.join("retroachievements.toml");

    let legacy_toml = r#"username = "WindowsUser99"
token = "real_dpapi_test_tok_777"
display_name = "WindowsUser99"
score = 800
"#;
    fs::write(&cfg_path, legacy_toml).unwrap();

    // 1. Load triggers real Windows DPAPI migration
    let loaded = RaConfig::load(&cfg_path);
    assert_eq!(loaded.username, "WindowsUser99");
    assert_eq!(loaded.token, "real_dpapi_test_tok_777");
    assert!(loaded.is_signed_in());
    assert!(loaded.token_protected.is_some());

    // 2. Verify disk has dpapi envelope and no plaintext
    let raw = fs::read_to_string(&cfg_path).unwrap();
    assert!(!raw.contains("real_dpapi_test_tok_777"), "DPAPI migrated file must not contain plaintext");
    assert!(raw.contains("token_protected = \"dpapi:v1:"));

    // 3. Reload directly using real DPAPI
    let reloaded = RaConfig::load(&cfg_path);
    assert_eq!(reloaded.username, "WindowsUser99");
    assert_eq!(reloaded.token, "real_dpapi_test_tok_777");
    assert!(reloaded.is_signed_in());

    // 4. Logout / clear credentials removes protected token and plaintext
    let mut logged_out = reloaded;
    logged_out.clear_credentials();
    logged_out.save(&cfg_path).unwrap();

    let disk_cleared = fs::read_to_string(&cfg_path).unwrap();
    assert!(!disk_cleared.contains("token_protected"), "Cleared credentials must not have token_protected");
    assert!(!disk_cleared.contains("token ="), "Cleared credentials must not have token");

    let reload_cleared = RaConfig::load(&cfg_path);
    assert!(!reload_cleared.is_signed_in());
    assert_eq!(reload_cleared.username, "");
    assert_eq!(reload_cleared.token, "");
    assert!(reload_cleared.token_protected.is_none());

    let _ = fs::remove_dir_all(root);
}

#[test]
fn two_portable_profiles_and_installed_profile_independence() {
    let root = temp_dir("multi_profile_independence");

    let installed_dir = root.join("installed").join("config");
    let portable_a_dir = root.join("portable_a").join("user").join("config");
    let portable_b_dir = root.join("portable_b").join("user").join("config");

    let inst_path = installed_dir.join("retroachievements.toml");
    let port_a_path = portable_a_dir.join("retroachievements.toml");
    let port_b_path = portable_b_dir.join("retroachievements.toml");

    let protector = gt4l_core::secret::FakeProtector::default();

    // 1. Setup Installed Profile
    let mut cfg_inst = RaConfig::default();
    cfg_inst.username = "InstPilot".to_string();
    cfg_inst.token = "tok_inst_111".to_string();
    cfg_inst.hardcore = true;
    cfg_inst.save_with(&inst_path, &protector).unwrap();

    // 2. Setup Portable A
    let mut cfg_port_a = RaConfig::default();
    cfg_port_a.username = "PortPilotA".to_string();
    cfg_port_a.token = "tok_port_a_222".to_string();
    cfg_port_a.hardcore = false;
    cfg_port_a.save_with(&port_a_path, &protector).unwrap();

    // 3. Setup Portable B
    let mut cfg_port_b = RaConfig::default();
    cfg_port_b.username = "PortPilotB".to_string();
    cfg_port_b.token = "tok_port_b_333".to_string();
    cfg_port_b.encore = true;
    cfg_port_b.save_with(&port_b_path, &protector).unwrap();

    // 4. Verify completely independent loading and no crosstalk
    let load_inst = RaConfig::load_with(&inst_path, &protector);
    let load_a = RaConfig::load_with(&port_a_path, &protector);
    let load_b = RaConfig::load_with(&port_b_path, &protector);

    assert_eq!(load_inst.username, "InstPilot");
    assert_eq!(load_inst.token, "tok_inst_111");
    assert!(load_inst.hardcore);

    assert_eq!(load_a.username, "PortPilotA");
    assert_eq!(load_a.token, "tok_port_a_222");
    assert!(!load_a.hardcore);

    assert_eq!(load_b.username, "PortPilotB");
    assert_eq!(load_b.token, "tok_port_b_333");
    assert!(load_b.encore);

    // 5. Mutating one profile does not touch the others
    let mut mutated_a = load_a;
    mutated_a.clear_credentials();
    mutated_a.save_with(&port_a_path, &protector).unwrap();

    let check_inst = RaConfig::load_with(&inst_path, &protector);
    let check_b = RaConfig::load_with(&port_b_path, &protector);
    assert_eq!(check_inst.username, "InstPilot");
    assert_eq!(check_b.username, "PortPilotB");

    let _ = fs::remove_dir_all(root);
}
