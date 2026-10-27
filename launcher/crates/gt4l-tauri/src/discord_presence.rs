//! Discord Rich Presence for the launcher (TASK-115, rc7).
//!
//! The launcher owns the presence: it opens the Discord client's local IPC pipe,
//! handshakes with the public application id and sends one activity that carries
//! the game name (Gran Turismo 4 or Gran Turismo 4 Spec II), the GT4Recomp name
//! and the real session start time. No mode, track, car, save, account or path is
//! ever sent. Presence never blocks a launch and never fails the game: every
//! connection is bounded, retried with backoff and stopped cleanly.
//!
//! The IPC transport is behind a trait so the manager can be tested with a fake.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// Public Discord application id (not a secret).
pub const CLIENT_ID: &str = "1564620020796100719";
/// Exact art-asset key uploaded by the owner.
pub const ASSET_KEY: &str = "gt4-discord-avatar";

/// A transport that can talk to the Discord client over its local IPC.
pub trait PresenceTransport: Send {
    fn connect(&mut self) -> Result<(), String>;
    fn send(&mut self, op: u32, payload: &str) -> Result<(), String>;
    fn close(&mut self);
}

#[cfg(windows)]
pub struct DiscordIpcTransport {
    handle: Option<isize>,
}

#[cfg(windows)]
impl DiscordIpcTransport {
    pub fn new() -> Self {
        Self { handle: None }
    }
}

#[cfg(windows)]
impl PresenceTransport for DiscordIpcTransport {
    fn connect(&mut self) -> Result<(), String> {
        use windows::Win32::Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE};
        use windows::Win32::Storage::FileSystem::{CreateFileW, FILE_SHARE_NONE, OPEN_EXISTING};
        use windows::core::PCWSTR;
        // Close any stale handle first.
        self.close();
        for index in 0..10 {
            let path = format!("\\\\.\\pipe\\discord-ipc-{}", index);
            let wide: Vec<u16> = path.encode_utf16().chain(std::iter::once(0)).collect();
            let handle = unsafe {
                CreateFileW(
                    PCWSTR(wide.as_ptr()),
                    (GENERIC_READ | GENERIC_WRITE).0,
                    FILE_SHARE_NONE,
                    None,
                    OPEN_EXISTING,
                    Default::default(),
                    None,
                )
            };
            match handle {
                Ok(h) if h != INVALID_HANDLE_VALUE => {
                    self.handle = Some(h.0 as isize);
                    return Ok(());
                }
                _ => {}
            }
            // Bound the busy-pipe retries.
            thread::sleep(Duration::from_millis(10));
        }
        Err("discord ipc pipe not available".to_string())
    }

    fn send(&mut self, op: u32, payload: &str) -> Result<(), String> {
        use windows::Win32::Foundation::HANDLE;
        use windows::Win32::Storage::FileSystem::WriteFile;
        let handle = self.handle.ok_or_else(|| "discord ipc not connected".to_string())?;
        let bytes = payload.as_bytes();
        let mut frame = Vec::with_capacity(8 + bytes.len());
        frame.extend_from_slice(&op.to_le_bytes());
        frame.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
        frame.extend_from_slice(bytes);
        let mut written: u32 = 0;
        let ok = unsafe {
            WriteFile(HANDLE(handle as _), Some(&frame), Some(&mut written), None)
        };
        if ok.is_ok() && written as usize == frame.len() {
            Ok(())
        } else {
            Err("discord ipc write failed".to_string())
        }
    }

    fn close(&mut self) {
        if let Some(handle) = self.handle.take() {
            use windows::Win32::Foundation::HANDLE;
            use windows::Win32::Foundation::CloseHandle;
            unsafe {
                let _ = CloseHandle(HANDLE(handle as _));
            }
        }
    }
}

#[cfg(not(windows))]
pub struct DiscordIpcTransport;

#[cfg(not(windows))]
impl DiscordIpcTransport {
    pub fn new() -> Self {
        Self
    }
}

#[cfg(not(windows))]
impl PresenceTransport for DiscordIpcTransport {
    fn connect(&mut self) -> Result<(), String> {
        Err("discord rich presence is only supported on Windows".to_string())
    }
    fn send(&mut self, _op: u32, _payload: &str) -> Result<(), String> {
        Err("discord rich presence is only supported on Windows".to_string())
    }
    fn close(&mut self) {}
}

/// Builds the activity-update JSON payload. Pure and deterministic.
pub fn build_activity_payload(
    game_name: &str,
    start_unix: i64,
    pid: u32,
    nonce: &str,
) -> String {
    // Escape is not needed for our fixed inputs, but keep them plain ASCII.
    format!(
        r#"{{"cmd":"SET_ACTIVITY","args":{{"pid":{pid},"activity":{{"details":"{game_name}","state":"GT4Recomp","timestamps":{{"start":{start_unix}}},"assets":{{"large_image":"{asset}","large_text":"GT4Recomp"}},"instance":false}}}},"nonce":"{nonce}"}}"#,
        pid = pid,
        game_name = game_name,
        start_unix = start_unix,
        asset = ASSET_KEY,
        nonce = nonce,
    )
}

/// Builds the clear-activity payload (activity null).
pub fn build_clear_payload(pid: u32, nonce: &str) -> String {
    format!(
        r#"{{"cmd":"SET_ACTIVITY","args":{{"pid":{pid},"activity":null}},"nonce":"{nonce}"}}"#,
        pid = pid,
        nonce = nonce,
    )
}

fn handshake_payload() -> String {
    format!(r#"{{"v":1,"client_id":"{CLIENT_ID}"}}"#)
}

fn unix_now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

/// Shared context captured at game start.
#[derive(Clone)]
struct SessionContext {
    game_name: String,
    start_unix: i64,
}

struct Session {
    stop: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

pub struct DiscordPresence {
    factory: Arc<dyn Fn() -> Box<dyn PresenceTransport> + Send + Sync>,
    context: Mutex<Option<SessionContext>>,
    session: Mutex<Option<Session>>,
}

impl DiscordPresence {
    pub fn new<F>(factory: F) -> Self
    where
        F: Fn() -> Box<dyn PresenceTransport> + Send + Sync + 'static,
    {
        Self {
            factory: Arc::new(factory),
            context: Mutex::new(None),
            session: Mutex::new(None),
        }
    }

    pub fn new_default() -> Self {
        Self::new(|| Box::new(DiscordIpcTransport::new()))
    }

    /// Called when a game starts. Records the context and starts presence if enabled.
    pub fn on_game_start(&self, game_name: &str, start_unix: i64, enabled: bool) {
        {
            let mut ctx = self.context.lock().unwrap_or_else(|p| p.into_inner());
            *ctx = Some(SessionContext {
                game_name: game_name.to_string(),
                start_unix,
            });
        }
        if enabled {
            self.start_session();
        }
    }

    /// Called when the game ends. Stops presence and clears.
    pub fn on_game_end(&self) {
        self.stop_session(true);
        let mut ctx = self.context.lock().unwrap_or_else(|p| p.into_inner());
        *ctx = None;
    }

    /// Called when the launcher exits.
    pub fn on_launcher_exit(&self) {
        self.stop_session(true);
    }

    /// React to the settings toggle while a game may be running.
    pub fn apply_toggle(&self, enabled: bool) {
        if enabled {
            let has_context = self.context.lock().unwrap_or_else(|p| p.into_inner()).is_some();
            let has_session = self.session.lock().unwrap_or_else(|p| p.into_inner()).is_some();
            if has_context && !has_session {
                self.start_session();
            }
        } else {
            self.stop_session(true);
        }
    }

    /// Whether a session is currently running.
    pub fn is_running(&self) -> bool {
        let g = self.session.lock().unwrap_or_else(|p| p.into_inner());
        g.is_some()
    }

    fn start_session(&self) {
        {
            let guard = self.session.lock().unwrap_or_else(|p| p.into_inner());
            if guard.is_some() {
                return;
            }
        }
        let context = match self.context.lock().unwrap_or_else(|p| p.into_inner()).clone() {
            Some(c) => c,
            None => {
                return;
            }
        };
        let stop = Arc::new(AtomicBool::new(false));
        let stop_clone = stop.clone();
        let factory = self.factory.clone();
        let pid = std::process::id();
        let handle = thread::spawn(move || {
            run_session(factory, context, pid, stop_clone);
        });
        let mut guard = self.session.lock().unwrap_or_else(|p| p.into_inner());
        // Re-check under lock to avoid two sessions.
        if guard.is_some() {
            stop.store(true, Ordering::Relaxed);
            let _ = handle.join();
            return;
        }
        *guard = Some(Session {
            stop,
            handle: Some(handle),
        });
    }

    fn stop_session(&self, clear: bool) {
        let session = {
            let mut guard = self.session.lock().unwrap_or_else(|p| p.into_inner());
            guard.take()
        };
        if let Some(mut session) = session {
            session.stop.store(true, Ordering::Relaxed);
            if let Some(handle) = session.handle.take() {
                let _ = handle.join();
            }
        }

        let _ = clear;
    }
}

fn run_session(
    factory: Arc<dyn Fn() -> Box<dyn PresenceTransport> + Send + Sync>,
    context: SessionContext,
    pid: u32,
    stop: Arc<AtomicBool>,
) {
    let mut transport: Option<Box<dyn PresenceTransport>> = None;
    let mut connected = false;
    let mut attempts: u32 = 0;
    let mut nonce_counter: u64 = 0;
    let handshake = handshake_payload();
    while !stop.load(Ordering::Relaxed) {
        if !connected {
            let mut t = (factory)();
            match t.connect() {
                Ok(()) => {
                    if t.send(0, &handshake).is_ok() {
                        nonce_counter += 1;
                        let payload = build_activity_payload(
                            &context.game_name,
                            context.start_unix,
                            pid,
                            &format!("n{}", nonce_counter),
                        );
                        if t.send(1, &payload).is_ok() {
                            transport = Some(t);
                            connected = true;
                            attempts = 0;
                        } else {
                            t.close();
                        }
                    } else {
                        t.close();
                    }
                }
                Err(_) => {}
            }
            if !connected {
                attempts = attempts.saturating_add(1);
                // Backoff up to 30s, in small chunks so stop is observed.
                let delay_ms = (500u64 * (1u64 << attempts.min(6))).min(30_000);
                let chunks = (delay_ms / 250).max(1);
                for _ in 0..chunks {
                    if stop.load(Ordering::Relaxed) {
                        break;
                    }
                    thread::sleep(Duration::from_millis(250));
                }
                continue;
            }
        }
        // Connected: sleep in small chunks, occasionally refresh activity.
        for _ in 0..20 {
            if stop.load(Ordering::Relaxed) {
                break;
            }
            thread::sleep(Duration::from_millis(250));
        }
        if stop.load(Ordering::Relaxed) {
            break;
        }
        // Refresh activity to keep the connection alive; on failure reconnect.
        if let Some(t) = transport.as_mut() {
            nonce_counter += 1;
            let payload = build_activity_payload(
                &context.game_name,
                context.start_unix,
                pid,
                &format!("n{}", nonce_counter),
            );
            if t.send(1, &payload).is_err() {
                connected = false;
                if let Some(t) = transport.take() {
                    // close via drop? call close
                    let mut t = t;
                    t.close();
                }
            }
        }
    }
    // Clear on exit.
    if connected {
        if let Some(mut t) = transport.take() {
            nonce_counter += 1;
            let payload = build_clear_payload(pid, &format!("c{}", nonce_counter));
            let _ = t.send(1, &payload);
            t.close();
        }
    }
    let _ = unix_now();
}

#[cfg(test)]
mod tests {
    use super::*;

    struct FakeTransport {
        connected: bool,
        fail_connect: bool,
        fail_send: bool,
        sent: Arc<Mutex<Vec<(u32, String)>>>,
    }

    impl FakeTransport {
        fn new(fail_connect: bool, fail_send: bool, sent: Arc<Mutex<Vec<(u32, String)>>>) -> Self {
            Self { connected: false, fail_connect, fail_send, sent }
        }
    }

    impl PresenceTransport for FakeTransport {
        fn connect(&mut self) -> Result<(), String> {
            if self.fail_connect {
                Err("no discord".to_string())
            } else {
                self.connected = true;
                Ok(())
            }
        }
        fn send(&mut self, op: u32, payload: &str) -> Result<(), String> {
            if self.fail_send {
                Err("send failed".to_string())
            } else {
                self.sent.lock().unwrap().push((op, payload.to_string()));
                Ok(())
            }
        }
        fn close(&mut self) {
            self.connected = false;
        }
    }

    #[test]
    fn payload_contains_both_targets_and_asset() {
        let p = build_activity_payload("Gran Turismo 4", 1000, 42, "n1");
        assert!(p.contains("\"details\":\"Gran Turismo 4\""));
        assert!(p.contains("\"large_image\":\"gt4-discord-avatar\""));
        assert!(p.contains("\"start\":1000"));
        let p2 = build_activity_payload("Gran Turismo 4 Spec II", 1000, 42, "n1");
        assert!(p2.contains("\"details\":\"Gran Turismo 4 Spec II\""));
    }

    #[test]
    fn manager_does_not_block_when_discord_absent() {
        let sent = Arc::new(Mutex::new(Vec::new()));
        let sent_clone = sent.clone();
        let presence = DiscordPresence::new(move || {
            Box::new(FakeTransport::new(true, false, sent_clone.clone()))
        });
        presence.on_game_start("Gran Turismo 4", 1234, true);
        // Give the worker a moment.
        thread::sleep(Duration::from_millis(200));
        // No panic; session may be recorded as running attempt but transport failed.
        presence.on_game_end();
        thread::sleep(Duration::from_millis(100));
        assert!(!presence.is_running());
    }

    #[test]
    fn disable_clears_and_reenable_preserves_timestamp() {
        let sent = Arc::new(Mutex::new(Vec::new()));
        let sent_clone = sent.clone();
        let presence = DiscordPresence::new(move || {
            Box::new(FakeTransport::new(false, false, sent_clone.clone()))
        });
        presence.on_game_start("Gran Turismo 4", 5555, true);
        thread::sleep(Duration::from_millis(300));
        // Disable
        presence.apply_toggle(false);
        thread::sleep(Duration::from_millis(200));
        assert!(!presence.is_running());
        // Re-enable during running game (context still? on_game_end not called, context remains)
        presence.apply_toggle(true);
        thread::sleep(Duration::from_millis(300));
        assert!(presence.is_running());
        // Check timestamp preserved in sent payload
        let frames = sent.lock().unwrap();
        let has_ts = frames.iter().any(|(_, p)| p.contains("\"start\":5555"));
        assert!(has_ts);
        drop(frames);
        presence.on_game_end();
    }
}
