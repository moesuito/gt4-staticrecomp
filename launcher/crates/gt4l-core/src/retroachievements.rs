//! RetroAchievements configuration, credential persistence, and `dorequest.php` client.

//!

//! Credentials (`username`, `token`) and options (`enabled`, `hardcore`, `encore`) are stored in

//! `%APPDATA%\GT4Recomp\config\retroachievements.toml` ([`crate::paths::UserDirs::retroachievements_file`]).

//! At launch, [`RaConfig::launch_environment`] passes `GT4_RA_*` environment variables to `gt4vulkan.exe`.



use std::collections::HashSet;

use std::ffi::OsString;

use std::fs;

use std::path::Path;

use std::time::Duration;



use serde::{Deserialize, Serialize};



/// Official RetroAchievements MD5 hash for Gran Turismo 4 (NTSC-U, SCUS-97328 v2.00).

pub const GT4_NTSC_U_RA_HASH: &str = "b7e2e47572f4163a216ea85eed3c6c63";



/// Official RetroAchievements Game ID for Gran Turismo 4 (PlayStation 2).

pub const GT4_RA_GAME_ID: u32 = 20580;



/// Default RetroAchievements server base URL.

pub const DEFAULT_RA_BASE_URL: &str = "https://retroachievements.org";



const REQUEST_TIMEOUT: Duration = Duration::from_secs(12);



fn default_true() -> bool {

    true

}



/// Persistent RetroAchievements configuration saved in `retroachievements.toml`.

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]

pub struct RaConfig {

    #[serde(default = "default_true")]

    pub enabled: bool,

    #[serde(default)]

    pub hardcore: bool,

    #[serde(default)]

    pub encore: bool,

    #[serde(default)]

    pub username: String,

    #[serde(default)]

    pub token: String,

    #[serde(default)]

    pub display_name: String,

    #[serde(default)]

    pub score: u32,

    #[serde(default)]

    pub score_softcore: u32,

}



impl Default for RaConfig {

    fn default() -> Self {

        Self {

            enabled: true,

            hardcore: false,

            encore: false,

            username: String::new(),

            token: String::new(),

            display_name: String::new(),

            score: 0,

            score_softcore: 0,

        }

    }

}



impl RaConfig {

    /// Loads `retroachievements.toml` from disk, returning [`RaConfig::default`] when missing or invalid.

    pub fn load(path: &Path) -> Self {

        let Ok(contents) = fs::read_to_string(path) else {

            return Self::default();

        };

        toml::from_str::<RaConfig>(&contents).unwrap_or_default()

    }



    /// Writes `retroachievements.toml` atomically via a temporary sibling file.

    pub fn save(&self, path: &Path) -> Result<(), String> {

        if let Some(parent) = path.parent() {

            fs::create_dir_all(parent).map_err(|e| format!("cannot create config folder: {e}"))?;

        }

        let body = toml::to_string_pretty(self).map_err(|e| format!("cannot format retroachievements.toml: {e}"))?;

        let tmp = path.with_extension("toml.tmp");

        fs::write(&tmp, body.as_bytes()).map_err(|e| format!("cannot write {}: {e}", tmp.display()))?;

        fs::rename(&tmp, path).map_err(|e| {

            let _ = fs::remove_file(&tmp);

            format!("cannot save {}: {e}", path.display())

        })

    }



    /// Whether an authenticated username and API token are stored.

    pub fn is_signed_in(&self) -> bool {

        !self.username.trim().is_empty() && !self.token.trim().is_empty()

    }



    /// Clears stored credentials while preserving the user's mode toggles.

    pub fn clear_credentials(&mut self) {

        self.username.clear();

        self.token.clear();

        self.display_name.clear();

        self.score = 0;

        self.score_softcore = 0;

    }



    /// Environment variables passed to `gt4vulkan.exe` when RetroAchievements is enabled and signed in.

    pub fn launch_environment(&self, cache_dir: &Path, language: crate::language::Language) -> Vec<(String, OsString)> {

        if !self.enabled || !self.is_signed_in() {

            return Vec::new();

        }

        let mut vars = vec![

            ("GT4_RA_ENABLED".to_string(), OsString::from("1")),

            (

                "GT4_RA_HARDCORE".to_string(),

                OsString::from(if self.hardcore { "1" } else { "0" }),

            ),

            (

                "GT4_RA_ENCORE".to_string(),

                OsString::from(if self.encore { "1" } else { "0" }),

            ),

            ("GT4_RA_USERNAME".to_string(), OsString::from(self.username.trim())),

            ("GT4_RA_TOKEN".to_string(), OsString::from(self.token.trim())),

            ("GT4_RA_HASH".to_string(), OsString::from(GT4_NTSC_U_RA_HASH)),

            ("GT4_RA_CACHE_DIR".to_string(), cache_dir.as_os_str().to_os_string()),

            (

                "GT4_RA_LANGUAGE".to_string(),

                OsString::from(if language.tag() == "pt-BR" { "pt-BR" } else { "en-US" }),

            ),

        ];

        if let Ok(custom_host) = std::env::var("GT4_RA_HOST") {

            let trimmed = custom_host.trim();

            if !trimmed.is_empty() {

                vars.push(("GT4_RA_HOST".to_string(), OsString::from(trimmed)));

            }

        }

        vars

    }

}



/// Summary of an individual achievement in the Gran Turismo 4 set.

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]

pub struct RaAchievementSummary {

    pub id: u32,

    pub title: String,

    pub description: String,

    pub points: u32,

    pub unlocked: bool,

    pub unlocked_hardcore: bool,

    pub badge_name: String,

    pub type_label: String,

}



/// Overview of the player's Gran Turismo 4 progress on RetroAchievements.

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]

pub struct RaGameOverview {

    pub game_id: u32,

    pub title: String,

    pub total_achievements: u32,

    pub unlocked_achievements: u32,

    pub total_points: u32,

    pub unlocked_points: u32,

    pub achievements: Vec<RaAchievementSummary>,

}



/// Account details returned by `r=login2`.

#[derive(Clone, Debug, PartialEq, Eq)]

pub struct RaAccountSession {

    pub username: String,

    pub display_name: String,

    pub token: String,

    pub score: u32,

    pub score_softcore: u32,

}



/// Percent-encodes a form parameter value according to RFC 3986.

pub fn url_encode(input: &str) -> String {

    let mut out = String::with_capacity(input.len());

    for byte in input.bytes() {

        match byte {

            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {

                out.push(byte as char);

            }

            b' ' => out.push_str("%20"),

            _ => {

                out.push_str(&format!("%{byte:02X}"));

            }

        }

    }

    out

}



fn dorequest_post(base_url: &str, form_body: &str) -> Result<String, String> {

    let url = format!("{}/dorequest.php", base_url.trim_end_matches('/'));

    let agent = ureq::Agent::new_with_config(

        ureq::config::Config::builder()

            .timeout_global(Some(REQUEST_TIMEOUT))

            .http_status_as_error(false)

            .build(),

    );

    let mut response = agent

        .post(&url)

        .header("User-Agent", &format!("GT4Recomp/{}", crate::LAUNCHER_VERSION))

        .header("Content-Type", "application/x-www-form-urlencoded")

        .send(form_body.as_bytes())

        .map_err(|e| format!("Could not reach RetroAchievements: {e}"))?;

    response

        .body_mut()

        .read_to_string()

        .map_err(|e| format!("Invalid response from RetroAchievements: {e}"))

}



#[derive(Deserialize)]

struct RawLoginResponse {

    #[serde(rename = "Success", default)]

    success: bool,

    #[serde(rename = "User", default)]

    user: Option<String>,

    #[serde(rename = "Token", default)]

    token: Option<String>,

    #[serde(rename = "Score", default)]

    score: u32,

    #[serde(rename = "SoftcoreScore", default)]

    softcore_score: u32,

    #[serde(rename = "Error", default)]

    error: Option<String>,

}



/// Parses a `r=login2` JSON response from `dorequest.php`.

pub fn parse_login_response(json_text: &str, fallback_user: &str) -> Result<RaAccountSession, String> {

    let parsed: RawLoginResponse = serde_json::from_str(json_text)

        .map_err(|_| "Unexpected response from RetroAchievements.".to_string())?;

    if !parsed.success {

        return Err(parsed

            .error

            .filter(|msg| !msg.trim().is_empty())

            .unwrap_or_else(|| "Invalid RetroAchievements username or password.".to_string()));

    }

    let token = parsed

        .token

        .filter(|t| !t.trim().is_empty())

        .ok_or_else(|| "RetroAchievements did not return a session token.".to_string())?;

    let display_name = parsed

        .user

        .filter(|u| !u.trim().is_empty())

        .unwrap_or_else(|| fallback_user.trim().to_string());

    Ok(RaAccountSession {

        username: fallback_user.trim().to_string(),

        display_name,

        token,

        score: parsed.score,

        score_softcore: parsed.softcore_score,

    })

}



/// Signs in with a RetroAchievements username and password via `r=login2`.

pub fn login_with_password(base_url: &str, username: &str, password: &str) -> Result<RaAccountSession, String> {

    let user_trimmed = username.trim();

    if user_trimmed.is_empty() {

        return Err("Enter your RetroAchievements username.".to_string());

    }

    if password.is_empty() {

        return Err("Enter your RetroAchievements password.".to_string());

    }

    let body = format!(

        "r=login2&u={}&p={}",

        url_encode(user_trimmed),

        url_encode(password)

    );

    let json = dorequest_post(base_url, &body)?;

    parse_login_response(&json, user_trimmed)

}



/// Refreshes user score and verifies the stored token via `r=login2`.

pub fn login_with_token(base_url: &str, username: &str, token: &str) -> Result<RaAccountSession, String> {

    let user_trimmed = username.trim();

    let token_trimmed = token.trim();

    if user_trimmed.is_empty() || token_trimmed.is_empty() {

        return Err("Not signed in to RetroAchievements.".to_string());

    }

    let body = format!(

        "r=login2&u={}&t={}",

        url_encode(user_trimmed),

        url_encode(token_trimmed)

    );

    let json = dorequest_post(base_url, &body)?;

    parse_login_response(&json, user_trimmed)

}



#[derive(Deserialize)]

struct RawPatchEnvelope {

    #[serde(rename = "Success", default)]

    success: bool,

    #[serde(rename = "Error", default)]

    error: Option<String>,

    #[serde(rename = "PatchData", default)]

    patch_data: Option<RawPatchData>,

}



#[derive(Deserialize)]

struct RawPatchData {

    #[serde(rename = "ID", default)]

    id: u32,

    #[serde(rename = "Title", default)]

    title: String,

    #[serde(rename = "Achievements", default)]

    achievements: Vec<RawAchievementDef>,

}



#[derive(Deserialize)]

struct RawAchievementDef {

    #[serde(rename = "ID", default)]

    id: u32,

    #[serde(rename = "Title", default)]

    title: String,

    #[serde(rename = "Description", default)]

    description: String,

    #[serde(rename = "Points", default)]

    points: u32,

    #[serde(rename = "Flags", default)]

    flags: u32,

    #[serde(rename = "BadgeName", default)]

    badge_name: String,

    #[serde(rename = "Type", default)]

    ach_type: Option<String>,

}



#[derive(Deserialize)]

struct RawStartSessionResponse {

    #[serde(rename = "Success", default)]

    success: bool,

    #[serde(rename = "Unlocks", default)]

    unlocks: Vec<RawUnlockEntry>,

    #[serde(rename = "HardcoreUnlocks", default)]

    hardcore_unlocks: Vec<RawUnlockEntry>,

}



#[derive(Deserialize)]

struct RawUnlockEntry {

    #[serde(rename = "ID", default)]

    id: u32,

}



/// Combines `r=patch` and `r=startsession` JSON responses into a [`RaGameOverview`].

pub fn parse_game_overview(patch_json: &str, session_json: &str) -> Result<RaGameOverview, String> {

    let patch_env: RawPatchEnvelope = serde_json::from_str(patch_json)

        .map_err(|_| "Could not parse achievement data from RetroAchievements.".to_string())?;

    if !patch_env.success {

        return Err(patch_env

            .error

            .unwrap_or_else(|| "RetroAchievements rejected the achievement set request.".to_string()));

    }

    let patch = patch_env

        .patch_data

        .ok_or_else(|| "RetroAchievements returned no achievement data for Gran Turismo 4.".to_string())?;



    let mut softcore_ids = HashSet::new();

    let mut hardcore_ids = HashSet::new();

    if let Ok(sess) = serde_json::from_str::<RawStartSessionResponse>(session_json) {

        if sess.success {

            for entry in sess.unlocks {

                softcore_ids.insert(entry.id);

            }

            for entry in sess.hardcore_unlocks {

                hardcore_ids.insert(entry.id);

                softcore_ids.insert(entry.id);

            }

        }

    }



    let mut total_achievements = 0u32;

    let mut unlocked_achievements = 0u32;

    let mut total_points = 0u32;

    let mut unlocked_points = 0u32;

    let mut achievements = Vec::new();



    for def in patch.achievements {

        // Flags == 3 is Core (promoted) achievements in rcheevos.

        if def.flags != 3 {

            continue;

        }

        let unlocked_hc = hardcore_ids.contains(&def.id);

        let unlocked = unlocked_hc || softcore_ids.contains(&def.id);

        total_achievements += 1;

        total_points += def.points;

        if unlocked {

            unlocked_achievements += 1;

            unlocked_points += def.points;

        }

        let type_label = match def.ach_type.as_deref().unwrap_or("") {

            "progression" => "Progression",

            "win_condition" => "Win Condition",

            "missable" => "Missable",

            _ => "",

        }

        .to_string();



        achievements.push(RaAchievementSummary {

            id: def.id,

            title: def.title,

            description: def.description,

            points: def.points,

            unlocked,

            unlocked_hardcore: unlocked_hc,

            badge_name: def.badge_name,

            type_label,

        });

    }



    Ok(RaGameOverview {

        game_id: if patch.id != 0 { patch.id } else { GT4_RA_GAME_ID },

        title: if patch.title.is_empty() {

            "Gran Turismo 4".to_string()

        } else {

            patch.title

        },

        total_achievements,

        unlocked_achievements,

        total_points,

        unlocked_points,

        achievements,

    })

}



/// Fetches the player's Gran Turismo 4 achievement list and unlock progress from RetroAchievements.

pub fn fetch_game_overview(base_url: &str, username: &str, token: &str) -> Result<RaGameOverview, String> {

    let u_enc = url_encode(username.trim());

    let t_enc = url_encode(token.trim());

    let patch_body = format!("r=patch&u={u_enc}&t={t_enc}&g={GT4_RA_GAME_ID}");

    let session_body = format!("r=startsession&u={u_enc}&t={t_enc}&g={GT4_RA_GAME_ID}&h=1&m={GT4_NTSC_U_RA_HASH}");

    let patch_json = dorequest_post(base_url, &patch_body)?;

    let session_json = dorequest_post(base_url, &session_body).unwrap_or_default();

    parse_game_overview(&patch_json, &session_json)

}



#[cfg(test)]

mod tests {

    use super::*;

    use std::path::PathBuf;



    fn scratch_file(name: &str) -> PathBuf {

        let dir = std::env::temp_dir().join(format!("gt4l-ra-{}-{name}", std::process::id()));

        let _ = fs::remove_dir_all(&dir);

        fs::create_dir_all(&dir).unwrap();

        dir.join("retroachievements.toml")

    }



    #[test]

    fn config_round_trips_and_clears_credentials_cleanly() {

        let file = scratch_file("roundtrip");

        let mut cfg = RaConfig::default();

        assert!(cfg.enabled);

        assert!(!cfg.is_signed_in());



        cfg.username = "DriverOne".to_string();

        cfg.display_name = "DriverOne".to_string();

        cfg.token = "tok_secret_123".to_string();

        cfg.hardcore = true;

        cfg.encore = true;

        cfg.score = 1500;

        cfg.score_softcore = 300;

        cfg.save(&file).unwrap();



        let loaded = RaConfig::load(&file);

        assert_eq!(loaded, cfg);

        assert!(loaded.is_signed_in());



        let env_vars = loaded.launch_environment(Path::new("C:\\GT4\\data\\runtime\\ra_cache"), crate::language::Language::fallback());

        assert!(env_vars.contains(&("GT4_RA_ENABLED".to_string(), OsString::from("1"))));

        assert!(env_vars.contains(&("GT4_RA_HARDCORE".to_string(), OsString::from("1"))));

        assert!(env_vars.contains(&("GT4_RA_ENCORE".to_string(), OsString::from("1"))));

        assert!(env_vars.contains(&("GT4_RA_USERNAME".to_string(), OsString::from("DriverOne"))));

        assert!(env_vars.contains(&("GT4_RA_TOKEN".to_string(), OsString::from("tok_secret_123"))));

        assert!(env_vars.contains(&("GT4_RA_HASH".to_string(), OsString::from(GT4_NTSC_U_RA_HASH))));

        assert!(env_vars.contains(&("GT4_RA_LANGUAGE".to_string(), OsString::from("en-US"))));



        let pt_lang = crate::language::Language::from_tag("pt-BR").unwrap();

        let pt_vars = loaded.launch_environment(Path::new("C:\\GT4\\data\\runtime\\ra_cache"), pt_lang);

        assert!(pt_vars.contains(&("GT4_RA_LANGUAGE".to_string(), OsString::from("pt-BR"))));



        let mut cleared = loaded;

        cleared.clear_credentials();

        assert!(!cleared.is_signed_in());

        assert!(cleared.hardcore, "mode toggles stay intact on sign-out");

        assert!(cleared.launch_environment(Path::new("C:\\cache"), crate::language::Language::fallback()).is_empty());



        let _ = fs::remove_dir_all(file.parent().unwrap());

    }



    #[test]

    fn parse_login_and_game_overview_handle_real_dorequest_payloads() {

        let login_json = r#"{"Success":true,"User":"BSpecBob","Token":"ra_tok_777","Score":4200,"SoftcoreScore":850,"Messages":1}"#;

        let session = parse_login_response(login_json, "bspecbob").unwrap();

        assert_eq!(session.display_name, "BSpecBob");

        assert_eq!(session.token, "ra_tok_777");

        assert_eq!(session.score, 4200);

        assert_eq!(session.score_softcore, 850);



        let err_json = r#"{"Success":false,"Error":"Invalid User/Password combination."}"#;

        assert_eq!(

            parse_login_response(err_json, "bspecbob").unwrap_err(),

            "Invalid User/Password combination."

        );



        let patch_json = r#"{

            "Success": true,

            "PatchData": {

                "ID": 20580,

                "Title": "Gran Turismo 4",

                "Achievements": [

                    {

                        "ID": 1001,

                        "Title": "National B License",

                        "Description": "Acquire the National B License.",

                        "Points": 10,

                        "Flags": 3,

                        "BadgeName": "11001",

                        "Type": "progression"

                    },

                    {

                        "ID": 1002,

                        "Title": "GT World Championship",

                        "Description": "Win the GT World Championship.",

                        "Points": 50,

                        "Flags": 3,

                        "BadgeName": "11002",

                        "Type": "win_condition"

                    },

                    {

                        "ID": 9999,

                        "Title": "Unofficial Test",

                        "Description": "Should be skipped.",

                        "Points": 5,

                        "Flags": 5,

                        "BadgeName": "99999"

                    }

                ]

            }

        }"#;

        let startsession_json = r#"{

            "Success": true,

            "HardcoreUnlocks": [{"ID": 1001, "When": 1700000100}],

            "Unlocks": []

        }"#;



        let overview = parse_game_overview(patch_json, startsession_json).unwrap();

        assert_eq!(overview.game_id, 20580);

        assert_eq!(overview.title, "Gran Turismo 4");

        assert_eq!(overview.total_achievements, 2);

        assert_eq!(overview.unlocked_achievements, 1);

        assert_eq!(overview.total_points, 60);

        assert_eq!(overview.unlocked_points, 10);

        assert_eq!(overview.achievements.len(), 2);

        assert!(overview.achievements[0].unlocked);

        assert!(overview.achievements[0].unlocked_hardcore);

        assert_eq!(overview.achievements[0].type_label, "Progression");

        assert!(!overview.achievements[1].unlocked);

    }

}

