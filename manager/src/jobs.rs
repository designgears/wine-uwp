// SPDX-License-Identifier: LGPL-2.1-or-later
//! Starting and stopping apps, installs and sign-in helpers, as child processes.

use crate::runtime::{relative, state_dir, App, Config, Runtime};
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

/// Launch options kept per app in the manager's config.
#[derive(Clone, Debug, Default)]
pub struct LaunchOptions {
    pub winedebug: String,
    pub display: String,
}

pub fn launch_options(cfg: &Config, app: &App) -> LaunchOptions {
    LaunchOptions {
        winedebug: cfg.get(&format!("winedebug:{}", app.id())).unwrap_or("").to_string(),
        display: cfg.get(&format!("display:{}", app.id())).unwrap_or("").to_string(),
    }
}

pub fn logs_dir() -> PathBuf {
    state_dir().join("logs")
}

/// Starts the app through the tree's run.sh, which picks the Wine, prefix and overrides and runs wineuwp-launch.
pub fn launch(rt: &Runtime, cfg: &Config, app: &App) -> std::io::Result<(Child, PathBuf)> {
    let opts = launch_options(cfg, app);
    std::fs::create_dir_all(logs_dir())?;
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let log = logs_dir().join(format!("{}-{}.log", app.stage_name, stamp));
    let build = cfg.build_for(rt, &app.data);
    let mut cmd = Command::new("bash");
    cmd.arg(rt.root.join("run.sh"))
        .current_dir(&rt.root)
        .env("UWP_STAGE", relative(&rt.uwp_bin.join("stage"), &app.stage))
        .env("WINEPREFIX", cfg.prefix_for(rt, &app.data))
        .env("UWP_TIMEOUT", "2592000")
        .env("UWP_LOG", &log)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    if !build.as_os_str().is_empty() {
        cmd.env("WINE_BUILD", &build);
    }
    if !opts.winedebug.is_empty() {
        cmd.env("WINEDEBUG", &opts.winedebug);
    }
    if !opts.display.is_empty() {
        cmd.env("DISPLAY", &opts.display);
    }
    Ok((cmd.spawn()?, log))
}

/// Stops everything in the app's prefix (wineserver -k), as the repository's scripts do.
pub fn stop(rt: &Runtime, cfg: &Config, app: &App) -> std::io::Result<()> {
    let build = cfg.build_for(rt, &app.data);
    Command::new(rt.wineserver(&build))
        .arg("-k")
        .env("WINEPREFIX", cfg.prefix_for(rt, &app.data))
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|_| ())
}

/// The package full names of running app processes, from their environments (also those started elsewhere).
pub fn running_packages() -> Vec<String> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir("/proc") else { return out };
    let me = std::process::id().to_string();
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if !name.bytes().all(|b| b.is_ascii_digit()) || name == me {
            continue;
        }
        // only Windows processes: their exe is a Wine preloader or loader
        let exe = std::fs::read_link(e.path().join("exe")).unwrap_or_default();
        let exe = exe.to_string_lossy();
        if !exe.contains("wine") {
            continue;
        }
        let Ok(env) = std::fs::read(e.path().join("environ")) else { continue };
        for var in env.split(|b| *b == 0) {
            if let Some(v) = var.strip_prefix(b"WINE_UWP_PACKAGE_FULL_NAME=") {
                let v = String::from_utf8_lossy(v).into_owned();
                if !out.contains(&v) {
                    out.push(v);
                }
            }
        }
    }
    out
}

/// A line-oriented background command: each output line goes to `on_line`, then `on_exit` gets success.
pub fn stream(
    mut cmd: Command,
    log: Option<PathBuf>,
    on_line: impl Fn(String) + Send + Sync + 'static,
    on_exit: impl FnOnce(bool) + Send + 'static,
) -> std::io::Result<u32> {
    cmd.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = cmd.spawn()?;
    let pid = child.id();
    let stdout = child.stdout.take().unwrap();
    let stderr = child.stderr.take().unwrap();
    let on_line = std::sync::Arc::new(on_line);
    let log_file = log.and_then(|p| std::fs::File::create(p).ok()).map(|f| std::sync::Arc::new(std::sync::Mutex::new(f)));
    let err_line = on_line.clone();
    let err_log = log_file.clone();
    let err = std::thread::spawn(move || {
        for line in BufReader::new(stderr).lines().map_while(Result::ok) {
            if let Some(f) = &err_log {
                use std::io::Write;
                let _ = writeln!(f.lock().unwrap(), "{line}");
            }
            // Wine's own chatter on stderr; pass only what looks like the tool's
            if !line.contains(":fixme:") && !line.contains(":err:") && !line.starts_with("MESA") && !line.contains("pci id") {
                err_line(line);
            }
        }
    });
    std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Some(f) = &log_file {
                use std::io::Write;
                let _ = writeln!(f.lock().unwrap(), "{line}");
            }
            on_line(line);
        }
        let _ = err.join();
        let ok = child.wait().map(|s| s.success()).unwrap_or(false);
        on_exit(ok);
    });
    Ok(pid)
}

/// wineuwp-install for a Store id, a file or a URL, into a data directory.
pub fn install_command(rt: &Runtime, cfg: &Config, source: &str, data: &Path, store: bool) -> Command {
    let build = cfg.build_for(rt, data);
    let mut cmd = Command::new(rt.wine(&build));
    cmd.arg("wineuwp-install").arg(source).arg(data).arg("--windows").arg(windows_tree(rt));
    if store {
        cmd.arg("--store");
    }
    base_env(&mut cmd, rt, cfg, data);
    cmd
}

/// A profile's install script (`#! install:`), relative to the tree, with DATADIR and the optional version.
pub fn recipe_command(rt: &Runtime, cfg: &Config, script: &str, data: &Path, version: &str) -> Command {
    let mut cmd = Command::new("bash");
    cmd.arg(rt.root.join(script)).arg(data);
    if !version.trim().is_empty() {
        cmd.arg(version.trim());
    }
    base_env(&mut cmd, rt, cfg, data);
    cmd
}

fn base_env(cmd: &mut Command, rt: &Runtime, cfg: &Config, data: &Path) {
    let build = cfg.build_for(rt, data);
    cmd.current_dir(&rt.root)
        .env("WINEPREFIX", cfg.prefix_for(rt, data))
        .env("WINE_UWP_ROOT", &rt.root)
        .env("WINEDEBUG", "-all");
    if !build.as_os_str().is_empty() {
        cmd.env("WINE_BUILD", &build);
        let path = std::env::var("PATH").unwrap_or_default();
        cmd.env("PATH", format!("{}:{}", build.display(), path));
    }
}

fn windows_tree(rt: &Runtime) -> PathBuf {
    std::env::var_os("WIN11").map(PathBuf::from).unwrap_or_else(|| rt.uwp_bin.join("win11"))
}

/// winexboxauth's self-test through the probe: `status` or `clientdevice CLIENT`.
pub fn xboxauth_command(rt: &Runtime, cfg: &Config, data: &Path, args: &[&str]) -> Option<Command> {
    let probe = rt.probe_xboxauth()?;
    let build = cfg.build_for(rt, data);
    let mut cmd = Command::new(rt.wine(&build));
    cmd.arg(&probe).args(args);
    base_env(&mut cmd, rt, cfg, data);
    cmd.current_dir(probe.parent().unwrap());
    Some(cmd)
}

/// The newest log files for an app, newest first.
pub fn app_logs(app: &App) -> Vec<PathBuf> {
    let Ok(rd) = std::fs::read_dir(logs_dir()) else { return Vec::new() };
    let prefix = format!("{}-", app.stage_name);
    let mut logs: Vec<PathBuf> =
        rd.flatten().map(|e| e.path()).filter(|p| p.file_name().is_some_and(|n| n.to_string_lossy().starts_with(&prefix))).collect();
    logs.sort();
    logs.reverse();
    logs
}

/// The last `max` bytes of a log, from a line start.
pub fn tail(path: &Path, max: u64) -> String {
    use std::io::{Read, Seek, SeekFrom};
    let Ok(mut f) = std::fs::File::open(path) else { return String::new() };
    let len = f.metadata().map(|m| m.len()).unwrap_or(0);
    let start = len.saturating_sub(max);
    let _ = f.seek(SeekFrom::Start(start));
    let mut buf = Vec::new();
    let _ = f.read_to_end(&mut buf);
    let text = String::from_utf8_lossy(&buf).into_owned();
    if start > 0 {
        text.split_once('\n').map(|(_, r)| r.to_string()).unwrap_or(text)
    } else {
        text
    }
}
