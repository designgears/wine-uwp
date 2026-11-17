// SPDX-License-Identifier: LGPL-2.1-or-later
//! Where wine-uwp is, what env.sh resolves to, the manager's own settings, and the installed apps.

use crate::envfile;
use std::collections::BTreeMap;
use std::path::{Component, Path, PathBuf};
use std::process::Command;

/// The wine-uwp tree: the repository, or an installed share/wine-uwp; both have env.sh, run.sh and
/// launch-defaults.env side by side.
#[derive(Clone, Debug)]
pub struct Runtime {
    pub root: PathBuf,
    pub uwp_bin: PathBuf,
    pub prefix: PathBuf,
    pub wine_build: PathBuf,
}

fn is_root(p: &Path) -> bool {
    p.join("env.sh").is_file() && p.join("run.sh").is_file() && p.join("launch-defaults.env").is_file()
}

pub fn find_root() -> Option<PathBuf> {
    if let Ok(r) = std::env::var("WINE_UWP_ROOT") {
        let r = PathBuf::from(r);
        if is_root(&r) {
            return Some(r);
        }
    }
    let exe = std::env::current_exe().ok()?;
    for dir in exe.ancestors().skip(1) {
        if is_root(dir) {
            return Some(dir.to_path_buf());
        }
        let share = dir.join("share/wine-uwp");
        if is_root(&share) {
            return Some(share);
        }
    }
    let cwd = std::env::current_dir().ok()?;
    cwd.ancestors().find(|d| is_root(d)).map(Path::to_path_buf)
}

impl Runtime {
    /// Resolves the tree's defaults by sourcing env.sh, so they are exactly what run.sh would use.
    pub fn load(root: PathBuf) -> Runtime {
        let out = Command::new("bash")
            .arg("-c")
            .arg(r#". "$1/env.sh" >/dev/null 2>&1; printf '%s\n%s\n%s\n' "$UWP_BIN" "$WINEPREFIX" "$WINE_BUILD""#)
            .arg("bash")
            .arg(&root)
            .output();
        let text = out.map(|o| String::from_utf8_lossy(&o.stdout).into_owned()).unwrap_or_default();
        let mut l = text.lines().map(PathBuf::from);
        let uwp_bin = l.next().filter(|p| !p.as_os_str().is_empty()).unwrap_or_else(|| root.join("uwp-bin"));
        let prefix = l.next().filter(|p| !p.as_os_str().is_empty()).unwrap_or_else(|| root.join("prefix"));
        let wine_build = l.next().unwrap_or_default();
        Runtime { root, uwp_bin, prefix, wine_build }
    }

    pub fn wine(&self, build: &Path) -> PathBuf {
        if build.join("wine").is_file() {
            build.join("wine")
        } else {
            PathBuf::from("wine")
        }
    }

    pub fn wineserver(&self, build: &Path) -> PathBuf {
        let s = build.join("server/wineserver");
        if s.is_file() {
            s
        } else {
            PathBuf::from("wineserver")
        }
    }

    pub fn probe_xboxauth(&self) -> Option<PathBuf> {
        let p = self.uwp_bin.join("build-probes/probe-xboxauth.exe");
        p.is_file().then_some(p)
    }
}

/// The manager's settings, a key=value file: per data directory its prefix and Wine build, extra data
/// directories, and per app its launch options.
#[derive(Clone, Debug, Default)]
pub struct Config {
    pub values: BTreeMap<String, String>,
}

pub fn config_path() -> PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".config"));
    base.join("wine-uwp/manager.conf")
}

pub fn state_dir() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/state"));
    base.join("wine-uwp")
}

impl Config {
    pub fn load() -> Config {
        let mut values = BTreeMap::new();
        if let Ok(text) = std::fs::read_to_string(config_path()) {
            for line in text.lines() {
                let line = line.trim();
                if line.starts_with('#') {
                    continue;
                }
                if let Some((k, v)) = line.split_once('=') {
                    values.insert(k.trim().to_string(), v.trim().to_string());
                }
            }
        }
        Config { values }
    }

    pub fn save(&self) -> std::io::Result<()> {
        let path = config_path();
        if let Some(d) = path.parent() {
            std::fs::create_dir_all(d)?;
        }
        let mut text = String::from("# wine-uwp-manager settings\n");
        for (k, v) in &self.values {
            text.push_str(&format!("{k}={v}\n"));
        }
        std::fs::write(path, text)
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.values.get(key).map(String::as_str).filter(|v| !v.is_empty())
    }

    pub fn set(&mut self, key: &str, value: &str) {
        if value.is_empty() {
            self.values.remove(key);
        } else {
            self.values.insert(key.to_string(), value.to_string());
        }
    }

    /// The data directories: the tree's own uwp-bin, every uwp-bin/*/ with a stage/, and those added by hand.
    pub fn data_dirs(&self, rt: &Runtime) -> Vec<PathBuf> {
        let mut dirs = vec![rt.uwp_bin.clone()];
        if let Ok(rd) = std::fs::read_dir(&rt.uwp_bin) {
            let mut extra: Vec<PathBuf> =
                rd.flatten().map(|e| e.path()).filter(|p| p.join("stage").is_dir()).collect();
            extra.sort();
            dirs.extend(extra);
        }
        if let Some(list) = self.get("datadirs") {
            for d in list.split(':').filter(|d| !d.is_empty()) {
                let d = PathBuf::from(d);
                if !dirs.contains(&d) {
                    dirs.push(d);
                }
            }
        }
        dirs
    }

    pub fn prefix_for(&self, rt: &Runtime, data: &Path) -> PathBuf {
        self.get(&format!("prefix:{}", data.display())).map(PathBuf::from).unwrap_or_else(|| rt.prefix.clone())
    }

    pub fn build_for(&self, rt: &Runtime, data: &Path) -> PathBuf {
        self.get(&format!("build:{}", data.display())).map(PathBuf::from).unwrap_or_else(|| rt.wine_build.clone())
    }
}

/// One installed app: a stage directory with an identity.env.
#[derive(Clone, Debug)]
pub struct App {
    pub stage: PathBuf,
    pub data: PathBuf,
    pub stage_name: String,
    pub title: String,
    pub family: String,
    pub full_name: String,
    pub version: String,
    pub icon: Option<PathBuf>,
}

impl App {
    pub fn id(&self) -> String {
        self.stage.display().to_string()
    }
}

pub fn discover(rt: &Runtime, cfg: &Config) -> Vec<App> {
    let mut apps = Vec::new();
    for data in cfg.data_dirs(rt) {
        let Ok(rd) = std::fs::read_dir(data.join("stage")) else { continue };
        let mut stages: Vec<PathBuf> = rd.flatten().map(|e| e.path()).collect();
        stages.sort();
        for stage in stages {
            let ident = stage.join("identity.env");
            // stage-app.sh leftovers, update copies (store.new, store.pre-update-*) and frameworks have none
            if !ident.is_file() || stage.file_name().is_some_and(|n| n.to_string_lossy().contains('.')) {
                continue;
            }
            let vals = envfile::read(&ident);
            let get = |k: &str| vals.iter().rev().find(|e| e.name == k).map(|e| e.value.clone()).unwrap_or_default();
            let full_name = get("WINE_UWP_PACKAGE_FULL_NAME");
            let stage_name = stage.file_name().unwrap().to_string_lossy().into_owned();
            let title = Some(get("WINE_UWP_WINDOW_TITLE")).filter(|t| !t.is_empty()).unwrap_or_else(|| stage_name.clone());
            let version = full_name.split('_').nth(1).unwrap_or("").to_string();
            apps.push(App {
                icon: find_icon(&stage),
                family: get("WINE_UWP_PACKAGE_FAMILY_NAME"),
                stage_name,
                title,
                full_name,
                version,
                data: data.clone(),
                stage,
            });
        }
    }
    apps
}

/// The package's logo from its manifest, through the resource variants an appx ships (scale-200, targetsize...).
fn find_icon(stage: &Path) -> Option<PathBuf> {
    let manifest = std::fs::read_to_string(stage.join("AppxManifest.xml")).ok()?;
    let mut candidates = Vec::new();
    for attr in ["Square44x44Logo=\"", "Square150x150Logo=\"", "<Logo>"] {
        if let Some(i) = manifest.find(attr) {
            let rest = &manifest[i + attr.len()..];
            let end = rest.find(['"', '<']).unwrap_or(rest.len());
            candidates.push(rest[..end].replace('\\', "/"));
        }
    }
    for rel in candidates {
        let path = stage.join(&rel);
        if path.is_file() {
            return Some(path);
        }
        let (dir, file) = match path.parent() {
            Some(d) => (d.to_path_buf(), path.file_stem()?.to_string_lossy().into_owned()),
            None => continue,
        };
        let ext = path.extension().map(|e| e.to_string_lossy().into_owned()).unwrap_or_else(|| "png".into());
        let mut best: Option<(i32, PathBuf)> = None;
        for e in std::fs::read_dir(&dir).ok()?.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if !n.starts_with(&format!("{file}.")) || !n.ends_with(&format!(".{ext}")) || n.contains("contrast") {
                continue;
            }
            // prefer a plain variant near 100-200 px
            let score = if n.contains("targetsize-96") || n.contains("scale-200") {
                3
            } else if n.contains("altform-unplated") {
                2
            } else {
                1
            };
            if best.as_ref().map_or(true, |(s, _)| score > *s) {
                best = Some((score, e.path()));
            }
        }
        if let Some((_, p)) = best {
            return Some(p);
        }
    }
    None
}

/// The path from `base` to `target`, as run.sh's UWP_STAGE wants it (relative to uwp-bin/stage).
pub fn relative(base: &Path, target: &Path) -> PathBuf {
    let b: Vec<Component> = base.components().collect();
    let t: Vec<Component> = target.components().collect();
    let common = b.iter().zip(&t).take_while(|(x, y)| x == y).count();
    let mut out = PathBuf::new();
    for _ in common..b.len() {
        out.push("..");
    }
    for c in &t[common..] {
        out.push(c.as_os_str());
    }
    out
}

/// The app's effective environment: the defaults, its profile, its local.env.
pub fn effective_env(rt: &Runtime, app: &App) -> Vec<envfile::Value> {
    let defaults = envfile::read(&rt.root.join("launch-defaults.env"));
    let identity = envfile::read(&app.stage.join("identity.env"));
    let profile = profile_path(rt, app);
    let profile_entries = profile.as_deref().map(envfile::read).unwrap_or_default();
    let local = envfile::read(&app.stage.join("local.env"));
    envfile::layer(&[
        ("identity", identity, false),
        ("defaults", defaults, true),
        ("profile", profile_entries, false),
        ("local", local, false),
    ])
}

/// The profile the launcher would load: the tree's library, else the stage's own copy.
pub fn profile_path(rt: &Runtime, app: &App) -> Option<PathBuf> {
    let lib = rt.root.join("profiles").join(format!("{}.env", app.family));
    if !app.family.is_empty() && lib.is_file() {
        return Some(lib);
    }
    let stage = app.stage.join("profile.env");
    stage.is_file().then_some(stage)
}

/// Install recipes: profiles that say how to install their app (`#! name:` and `#! install:` header lines).
#[derive(Clone, Debug)]
pub struct Recipe {
    pub family: String,
    pub name: String,
    pub script: String,
    pub default_dir: String,
}

pub fn recipes(rt: &Runtime) -> Vec<Recipe> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(rt.root.join("profiles")) else { return out };
    let mut files: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "env")).collect();
    files.sort();
    for f in files {
        let Ok(text) = std::fs::read_to_string(&f) else { continue };
        let mut r = Recipe {
            family: f.file_stem().unwrap().to_string_lossy().into_owned(),
            name: String::new(),
            script: String::new(),
            default_dir: String::new(),
        };
        for line in text.lines() {
            let Some(meta) = line.strip_prefix("#!") else { continue };
            if let Some((k, v)) = meta.trim().split_once(':') {
                match k.trim() {
                    "name" => r.name = v.trim().to_string(),
                    "install" => r.script = v.trim().to_string(),
                    "datadir" => r.default_dir = v.trim().to_string(),
                    _ => {}
                }
            }
        }
        if !r.script.is_empty() {
            if r.name.is_empty() {
                r.name = r.family.clone();
            }
            out.push(r);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn relative_paths() {
        assert_eq!(relative(Path::new("/a/b/stage"), Path::new("/a/b/mc/stage/x")), PathBuf::from("../mc/stage/x"));
        assert_eq!(relative(Path::new("/a/b/stage"), Path::new("/a/b/stage/calc")), PathBuf::from("calc"));
    }
}
