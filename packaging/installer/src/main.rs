// wine-uwp-installer: the setup, as a native app styled after Shapr3D (ui/app.slint).
//
// Run through `wine-uwp setup`, which exports the layout: WINE_UWP_ROOT (the scripts), UWP_BIN (the user's data
// directory), WINE_BUILD (the bin directory, with the wine-uwp command). Every step is one of the command-line
// tools, run as a subprocess; their "STAGE", "PROGRESS", "RATE" and "INSTALLED" lines drive the screens and the
// rest is kept for the details view:
//
//   download   packaging/harvest/download_iso.py     Microsoft's evaluation ISO, resumable and checksummed
//   extract    packaging/harvest/harvest.py          only the files wine-uwp needs, into DATA/windows
//   prefix     packaging/new-prefix.sh               the Wine prefix, in DATA/prefix
//   install    packaging/install-app.py              the user's .msixbundle, into DATA/stage
//
// Each finished step leaves a marker, so a closed or failed setup resumes where it stopped.
slint::include_modules!();

use slint::{Model, ModelRc, SharedString, VecModel, Weak};
use std::fs;
use std::io::{BufRead, BufReader};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const ISO_BYTES: u64 = 7_092_807_680;
const PAGE_SOURCE: i32 = 1;
const PAGE_PREPARE: i32 = 2;
const PAGE_PACKAGE: i32 = 3;
const PAGE_INSTALL: i32 = 4;
const PAGE_DONE: i32 = 5;
const PAGE_ERROR: i32 = 6;

#[derive(Clone)]
struct Paths {
    root: PathBuf,
    data: PathBuf,
    bin: PathBuf,
    windows: PathBuf,
    prefix: PathBuf,
    iso: PathBuf,
    py: String,
}

impl Paths {
    fn from_env() -> Paths {
        let exe_dir = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf));
        let bin = std::env::var_os("WINE_BUILD").map(PathBuf::from).or(exe_dir).unwrap_or_default();
        let root = std::env::var_os("WINE_UWP_ROOT")
            .map(PathBuf::from)
            .unwrap_or_else(|| bin.join("../share/wine-uwp"));
        let data = std::env::var_os("UWP_BIN").map(PathBuf::from).unwrap_or_else(|| {
            let base = std::env::var_os("XDG_DATA_HOME")
                .map(PathBuf::from)
                .unwrap_or_else(|| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share"));
            base.join("wine-uwp")
        });
        Paths {
            windows: data.join("windows"),
            prefix: data.join("prefix"),
            iso: data.join("iso/windows11-eval.iso"),
            py: std::env::var("PY").unwrap_or_else(|_| "python3".into()),
            root,
            data,
            bin,
        }
    }
    fn tool(&self, rel: &str) -> String {
        self.root.join("packaging").join(rel).to_string_lossy().into_owned()
    }
    fn windows_ready(&self) -> PathBuf {
        self.windows.join(".wine-uwp-harvested")
    }
    fn prefix_ready(&self) -> PathBuf {
        self.prefix.join(".wine-uwp-ready")
    }
    // what a desktop entry or a detached launch should run: the AppImage itself when there is one, since its
    // mount goes away with this process
    fn launcher(&self) -> String {
        std::env::var("APPIMAGE").unwrap_or_else(|_| self.bin.join("wine-uwp").to_string_lossy().into_owned())
    }
}

struct Shared {
    pid: Option<i32>,
    cancelled: bool,
    iso: Option<PathBuf>,
    package: Option<PathBuf>,
    installed: Option<String>,
    log: String,
    log_pushed: Instant,
    retry_install: bool,
}

type State = Arc<Mutex<Shared>>;

fn human_bytes(n: f64) -> String {
    if n >= 1e9 {
        format!("{:.1} GB", n / 1e9)
    } else if n >= 1e6 {
        format!("{:.0} MB", n / 1e6)
    } else {
        format!("{:.0} KB", n / 1e3)
    }
}

fn human_time(s: f64) -> String {
    let s = s as u64;
    if s < 60 {
        format!("{s} s")
    } else if s < 3600 {
        format!("{} min {:02} s", s / 60, s % 60)
    } else {
        format!("{} h {:02} min", s / 3600, (s % 3600) / 60)
    }
}

fn free_bytes(path: &Path) -> u64 {
    let mut p = path.to_path_buf();
    while !p.exists() {
        if !p.pop() {
            return 0;
        }
    }
    let c = std::ffi::CString::new(p.to_string_lossy().as_bytes()).unwrap();
    unsafe {
        let mut st: libc::statvfs = std::mem::zeroed();
        if libc::statvfs(c.as_ptr(), &mut st) == 0 {
            st.f_bavail as u64 * st.f_frsize as u64
        } else {
            0
        }
    }
}

fn installed_apps(paths: &Paths) -> Vec<String> {
    let mut v: Vec<String> = fs::read_dir(paths.data.join("stage"))
        .map(|rd| {
            rd.filter_map(|e| e.ok())
                .filter(|e| e.path().join("identity.env").is_file())
                .map(|e| e.file_name().to_string_lossy().into_owned())
                .collect()
        })
        .unwrap_or_default();
    v.sort();
    v
}

fn app_title(paths: &Paths, app: &str) -> String {
    fs::read_to_string(paths.data.join("stage").join(app).join("identity.env"))
        .ok()
        .and_then(|s| {
            s.lines().find_map(|l| l.strip_prefix("export WINE_UWP_WINDOW_TITLE=").map(|v| v.trim_matches('\'').to_string()))
        })
        .unwrap_or_else(|| app.to_string())
}

fn step(title: &str) -> Step {
    Step { title: title.into(), detail: SharedString::new(), state: 0 }
}

/// Run on the UI thread.
fn on_ui(ui: &Weak<Installer>, f: impl FnOnce(&Installer) + Send + 'static) {
    let _ = ui.upgrade_in_event_loop(move |ui| f(&ui));
}

fn set_step(ui: &Weak<Installer>, i: usize, state: i32, detail: Option<String>) {
    on_ui(ui, move |ui| {
        let m = ui.get_steps();
        if let Some(mut s) = m.row_data(i) {
            s.state = state;
            if let Some(d) = detail {
                s.detail = d.into();
            }
            m.set_row_data(i, s);
        }
    });
}

fn progress(ui: &Weak<Installer>, value: f32, text: String) {
    on_ui(ui, move |ui| {
        ui.set_progress(value);
        ui.set_progress_text(text.into());
    });
}

fn push_log(state: &State, ui: &Weak<Installer>, line: &str, force: bool) {
    let text = {
        let mut s = state.lock().unwrap();
        s.log.push_str(line);
        s.log.push('\n');
        if s.log.len() > 60_000 {
            let cut = s.log.len() - 40_000;
            let at = s.log[cut..].find('\n').map(|i| cut + i + 1).unwrap_or(cut);
            s.log.drain(..at);
        }
        if !force && s.log_pushed.elapsed() < Duration::from_millis(150) {
            return;
        }
        s.log_pushed = Instant::now();
        s.log.clone()
    };
    on_ui(ui, move |ui| ui.set_log_text(text.into()));
}

/// Run one tool; `on_line` sees every output line. Returns the exit code (-1 when cancelled) and the last
/// meaningful line, which is the reason when it failed.
fn run_tool(state: &State, ui: &Weak<Installer>, argv: &[String], env: &[(&str, &str)], mut on_line: impl FnMut(&str)) -> (i32, String) {
    push_log(state, ui, &format!("$ {}", argv.join(" ")), true);
    let mut cmd = Command::new("sh");
    cmd.arg("-c").arg("exec \"$@\" 2>&1").arg("sh").args(argv);
    cmd.stdout(Stdio::piped()).stdin(Stdio::null()).process_group(0);
    for (k, v) in env {
        cmd.env(k, v);
    }
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => return (127, e.to_string()),
    };
    state.lock().unwrap().pid = Some(child.id() as i32);
    let mut last = String::new();
    if let Some(out) = child.stdout.take() {
        for line in BufReader::new(out).lines().map_while(Result::ok) {
            push_log(state, ui, &line, false);
            if !line.is_empty() && !line.starts_with("PROGRESS") && !line.starts_with("RATE") {
                last = line.clone();
            }
            on_line(&line);
        }
    }
    let rc = child.wait().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
    push_log(state, ui, "", true);
    let mut s = state.lock().unwrap();
    s.pid = None;
    if s.cancelled {
        return (-1, last);
    }
    (rc, last)
}

fn fail(state: &State, ui: &Weak<Installer>, title: &str, text: String, install: bool) {
    {
        let mut s = state.lock().unwrap();
        s.retry_install = install;
    }
    let title = title.to_string();
    on_ui(ui, move |ui| {
        ui.set_error_title(title.into());
        ui.set_error_text(text.into());
        ui.set_page(PAGE_ERROR);
    });
}

fn space(ui: &Installer, paths: &Paths) {
    let part = paths.iso.with_extension("iso.part");
    let have = fs::metadata(&paths.iso).or_else(|_| fs::metadata(&part)).map(|m| m.len()).unwrap_or(0);
    let need = if ui.get_use_download() { ISO_BYTES.saturating_sub(have) } else { 0 } + 4_000_000_000;
    let free = free_bytes(&paths.data);
    ui.set_space_ok(free >= need);
    ui.set_space_text(
        format!("Needs about {} free in {}; {} available.", human_bytes(need as f64), paths.data.display(), human_bytes(free as f64)).into(),
    );
}

const PREPARE_STEPS: [&str; 7] = [
    "Download Windows 11 from Microsoft",
    "Check the download",
    "Extract Windows components",
    "Create the Wine environment",
    "Register Windows components",
    "Fetch debugging symbols from Microsoft",
    "Install Windows components",
];

fn prepare(state: State, ui: Weak<Installer>, paths: Paths, use_download: bool, keep_iso: bool) {
    // 1-2: the image
    let iso = if use_download {
        if paths.iso.exists() && paths.iso.with_extension("iso.verified").exists() {
            set_step(&ui, 0, 2, Some("Already downloaded".into()));
            set_step(&ui, 1, 2, Some(String::new()));
            paths.iso.clone()
        } else if paths.windows_ready().exists() {
            set_step(&ui, 0, 2, Some("Not needed: the components are already extracted".into()));
            set_step(&ui, 1, 2, Some(String::new()));
            paths.iso.clone()
        } else {
            let _ = fs::create_dir_all(paths.iso.parent().unwrap());
            set_step(&ui, 0, 1, Some("Contacting Microsoft…".into()));
            let mut rate = 0f64;
            let u = ui.clone();
            let (rc, reason) = run_tool(&state, &ui, &[paths.py.clone(), paths.tool("harvest/download_iso.py"), paths.iso.to_string_lossy().into()], &[], |l| {
                if let Some(r) = l.strip_prefix("RATE ") {
                    rate = r.trim().parse().unwrap_or(0.0);
                } else if let Some(p) = l.strip_prefix("PROGRESS ") {
                    let v: Vec<f64> = p.split_whitespace().filter_map(|x| x.parse().ok()).collect();
                    if v.len() == 2 && v[1] > 0.0 {
                        let mut text = format!("{} of {}", human_bytes(v[0]), human_bytes(v[1]));
                        if rate > 0.0 {
                            text += &format!(" · {}/s · {} left", human_bytes(rate), human_time((v[1] - v[0]) / rate));
                        }
                        progress(&u, (v[0] / v[1]) as f32, text.clone());
                        set_step(&u, 0, 1, Some(text));
                    }
                } else if l.starts_with("STAGE Verifying") {
                    set_step(&u, 0, 2, Some("Downloaded from microsoft.com".into()));
                    set_step(&u, 1, 1, Some("Checking the SHA-256 checksum".into()));
                    progress(&u, -1.0, "Checking the download…".into());
                } else if let Some(v) = l.strip_prefix("VERIFIED ") {
                    set_step(&u, 1, 2, Some(v.to_string()));
                } else if l.starts_with("UNVERIFIED ") {
                    set_step(&u, 1, 2, Some("A newer image than this setup was tested with".into()));
                }
            });
            if rc != 0 {
                set_step(&ui, 0, 3, None);
                return fail(&state, &ui, if rc < 0 { "Setup stopped" } else { "The download stopped" },
                    format!("{} Running the setup again continues where it stopped.", if reason.is_empty() { "The download did not finish.".into() } else { reason }), false);
            }
            let _ = fs::write(paths.iso.with_extension("iso.verified"), "");
            paths.iso.clone()
        }
    } else {
        let iso = state.lock().unwrap().iso.clone().unwrap_or_default();
        set_step(&ui, 0, 2, Some(format!("Using {}", iso.file_name().unwrap_or_default().to_string_lossy())));
        set_step(&ui, 1, 2, Some("Checked by the extraction".into()));
        iso
    };

    // 3: extract
    if paths.windows_ready().exists() {
        set_step(&ui, 2, 2, Some("Already extracted".into()));
    } else {
        set_step(&ui, 2, 1, Some("Opening the image…".into()));
        progress(&ui, -1.0, "Opening the image…".into());
        let partial = paths.data.join("windows.partial");
        let _ = fs::remove_dir_all(&partial);
        let u = ui.clone();
        let (rc, reason) = run_tool(&state, &ui, &[paths.py.clone(), paths.tool("harvest/harvest.py"), iso.to_string_lossy().into(), partial.to_string_lossy().into()], &[], |l| {
            if let Some(p) = l.strip_prefix("PROGRESS ") {
                let v: Vec<f64> = p.split_whitespace().filter_map(|x| x.parse().ok()).collect();
                if v.len() == 2 {
                    progress(&u, (v[0] / v[1].max(1.0)) as f32, format!("Extracting… {} %", v[0]));
                }
            } else if let Some(s) = l.strip_prefix("STAGE ") {
                set_step(&u, 2, 1, Some(s.to_string()));
            }
        });
        if rc != 0 {
            set_step(&ui, 2, 3, None);
            return fail(&state, &ui, "Could not extract the Windows components", if reason.is_empty() { "The image could not be read.".into() } else { reason }, false);
        }
        let _ = fs::remove_dir_all(&paths.windows);
        let _ = fs::rename(&partial, &paths.windows);
        let _ = fs::write(paths.windows_ready(), "");
        let missing = fs::read_to_string(paths.windows.join("harvest-missing.txt")).map(|s| s.lines().count()).unwrap_or(0);
        set_step(&ui, 2, 2, Some(if missing > 0 { format!("Done; {missing} optional files are not in this image") } else { "Done".into() }));
        if iso == paths.iso && !keep_iso {
            let _ = fs::remove_file(&paths.iso);
            let _ = fs::remove_file(paths.iso.with_extension("iso.verified"));
        }
    }

    // 4-7: the prefix
    if paths.prefix_ready().exists() {
        for i in 3..7 {
            set_step(&ui, i, 2, Some(String::new()));
        }
    } else {
        let _ = fs::remove_dir_all(&paths.prefix);
        let stages: [(&str, usize); 5] = [
            ("Creating the Wine prefix", 3),
            ("Registering", 4),
            ("Resolving Windows symbols", 5),
            ("Fetching symbols", 5),
            ("Installing Windows components", 6),
        ];
        let mut current = 3usize;
        set_step(&ui, 3, 1, Some("Starting Wine".into()));
        progress(&ui, -1.0, "Setting up Wine…".into());
        let u = ui.clone();
        let (rc, reason) = run_tool(&state, &ui, &["bash".into(), paths.tool("new-prefix.sh"), paths.windows.to_string_lossy().into(), paths.prefix.to_string_lossy().into()], &[("WINEDEBUG", "-all")], |l| {
            if let Some(what) = l.strip_prefix("STAGE ") {
                if let Some(&(_, k)) = stages.iter().find(|(p, _)| what.starts_with(p)) {
                    for j in current..k {
                        set_step(&u, j, 2, Some(String::new()));
                    }
                    current = k;
                    set_step(&u, k, 1, Some(what.to_string()));
                }
                progress(&u, -1.0, what.to_string());
            } else if l.starts_with("installed:") || l.starts_with("registered") || l.starts_with("verified:") {
                progress(&u, -1.0, l.to_string());
            }
        });
        if rc != 0 {
            set_step(&ui, current, 3, None);
            return fail(&state, &ui, "Could not create the Wine environment", if reason.is_empty() { "new-prefix.sh failed.".into() } else { reason }, false);
        }
        for i in 3..7 {
            set_step(&ui, i, 2, Some(String::new()));
        }
        let _ = fs::write(paths.prefix_ready(), "");
    }
    progress(&ui, 1.0, "Done".into());
    let given = state.lock().unwrap().package.clone();
    let info = given.and_then(|f| package_info(&paths, &f).ok());
    std::thread::sleep(Duration::from_millis(600));
    on_ui(&ui, move |ui| {
        if let Some(info) = info {
            apply_package(ui, info);
        }
        ui.set_page(PAGE_PACKAGE);
    });
}

fn main() {
    let paths = Paths::from_env();
    let ui = Installer::new().expect("cannot create the window");
    let state: State = Arc::new(Mutex::new(Shared {
        pid: None,
        cancelled: false,
        iso: None,
        package: None,
        installed: None,
        log: String::new(),
        log_pushed: Instant::now(),
        retry_install: false,
    }));
    ui.set_can_add_menu(std::env::var_os("FLATPAK_ID").is_none());
    ui.set_done_icon(slint::Image::load_from_svg_data(include_bytes!("../ui/icons/logo.svg")).unwrap_or_default());
    ui.set_package_icon(slint::Image::load_from_svg_data(include_bytes!("../ui/icons/logo.svg")).unwrap_or_default());

    // wine-uwp setup [--iso FILE] [PACKAGE]: pre-filled choices (and what scripted tests use instead of a dialog)
    let mut args = std::env::args().skip(1);
    let mut arg_package = None;
    while let Some(a) = args.next() {
        if a == "--iso" {
            if let Some(f) = args.next() {
                let f = PathBuf::from(f);
                ui.set_iso_name(f.file_name().unwrap_or_default().to_string_lossy().into_owned().into());
                ui.set_use_download(false);
                state.lock().unwrap().iso = Some(f);
            }
        } else {
            arg_package = Some(PathBuf::from(a));
        }
    }
    if let Some(f) = arg_package.filter(|f| f.is_file()) {
        // read once the Windows components exist (the framework check needs them); until then just remember it
        if paths.windows_ready().exists() {
            if let Ok(info) = package_info(&paths, &f) {
                apply_package(&ui, info);
            }
        }
        state.lock().unwrap().package = Some(f);
    }

    // where to start: a finished setup shows its app, a prepared one goes straight to the package
    let apps = installed_apps(&paths);
    if paths.windows_ready().exists() && paths.prefix_ready().exists() {
        if let Some(app) = apps.first() {
            show_done(&ui, &paths, app);
            state.lock().unwrap().installed = Some(app.clone());
        } else {
            ui.set_page(PAGE_PACKAGE);
        }
    }

    {
        let (w, p) = (ui.as_weak(), paths.clone());
        ui.on_get_started(move || {
            let ui = w.unwrap();
            space(&ui, &p);
            ui.set_page(PAGE_SOURCE);
        });
    }
    {
        let (w, s, p) = (ui.as_weak(), state.clone(), paths.clone());
        ui.on_choose_iso(move || {
            let (w, s, p) = (w.clone(), s.clone(), p.clone());
            std::thread::spawn(move || {
                if let Some(f) = rfd::FileDialog::new().set_title("Choose a Windows 11 ISO").add_filter("Disc images", &["iso", "ISO"]).pick_file() {
                    let name = f.file_name().unwrap_or_default().to_string_lossy().into_owned();
                    s.lock().unwrap().iso = Some(f);
                    on_ui(&w, move |ui| {
                        ui.set_iso_name(name.into());
                        ui.set_use_download(false);
                        space(ui, &p);
                    });
                }
            });
        });
    }
    {
        let (w, s, p) = (ui.as_weak(), state.clone(), paths.clone());
        ui.on_prepare(move || {
            let ui = w.unwrap();
            let steps: Vec<Step> = PREPARE_STEPS.iter().map(|t| step(t)).collect();
            ui.set_steps(ModelRc::new(VecModel::from(steps)));
            ui.set_progress(0.0);
            ui.set_progress_text("".into());
            ui.set_page(PAGE_PREPARE);
            {
                let mut st = s.lock().unwrap();
                st.cancelled = false;
                st.log.clear();
            }
            let (use_download, keep) = (ui.get_use_download(), ui.get_keep_iso());
            let (w, s, p) = (w.clone(), s.clone(), p.clone());
            std::thread::spawn(move || prepare(s, w, p, use_download, keep));
        });
    }
    {
        let (w, s, p) = (ui.as_weak(), state.clone(), paths.clone());
        ui.on_choose_package(move || {
            let (w, s, p) = (w.clone(), s.clone(), p.clone());
            std::thread::spawn(move || {
                let Some(f) = rfd::FileDialog::new()
                    .set_title("Choose the Shapr3D package")
                    .add_filter("App packages", &["msixbundle", "msix", "appxbundle", "appx"])
                    .pick_file()
                else {
                    return;
                };
                match package_info(&p, &f) {
                    Ok(info) => {
                        s.lock().unwrap().package = Some(f);
                        on_ui(&w, move |ui| apply_package(ui, info));
                    }
                    Err(e) => on_ui(&w, move |ui| {
                        ui.set_has_package(false);
                        ui.set_package_ok(false);
                        ui.set_package_error(e.into());
                    }),
                }
            });
        });
    }
    {
        let (w, s, p) = (ui.as_weak(), state.clone(), paths.clone());
        ui.on_install(move || {
            let ui = w.unwrap();
            let steps: Vec<Step> = ["Read the package", "Unpack the app", "Install frameworks"].iter().map(|t| step(t)).collect();
            ui.set_steps(ModelRc::new(VecModel::from(steps)));
            ui.set_progress(0.0);
            ui.set_progress_text("".into());
            ui.set_page(PAGE_INSTALL);
            {
                let mut st = s.lock().unwrap();
                st.cancelled = false;
                st.log.clear();
            }
            let package = s.lock().unwrap().package.clone().unwrap_or_default();
            let (w, s, p) = (w.clone(), s.clone(), p.clone());
            std::thread::spawn(move || install(s, w, p, package));
        });
    }
    {
        let s = state.clone();
        ui.on_cancel(move || {
            let mut st = s.lock().unwrap();
            st.cancelled = true;
            if let Some(pid) = st.pid {
                unsafe { libc::kill(-pid, libc::SIGTERM) };
            }
        });
    }
    {
        let (w, s) = (ui.as_weak(), state.clone());
        ui.on_retry(move || {
            let ui = w.unwrap();
            if s.lock().unwrap().retry_install {
                ui.set_page(PAGE_PACKAGE);
            } else {
                ui.invoke_prepare();
            }
        });
    }
    {
        let (w, s, p) = (ui.as_weak(), state.clone(), paths.clone());
        ui.on_launch(move || {
            let ui = w.unwrap();
            let Some(app) = s.lock().unwrap().installed.clone() else { return };
            if ui.get_can_add_menu() && ui.get_add_menu() {
                let _ = write_desktop_entry(&p, &app);
            }
            ui.set_launching(true);
            let _ = Command::new(p.launcher()).arg("run").arg(&app).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).process_group(0).spawn();
            let w = w.clone();
            slint::Timer::single_shot(Duration::from_secs(4), move || {
                if let Some(ui) = w.upgrade() {
                    let _ = ui.hide();
                }
            });
        });
    }
    {
        let w = ui.as_weak();
        ui.on_close_window(move || {
            let _ = w.unwrap().hide();
        });
    }
    ui.on_open_url(|url| {
        let _ = Command::new("xdg-open").arg(url.as_str()).spawn();
    });

    ui.run().expect("event loop failed");
    // closing the window mid-step stops the step; what is finished is kept
    let st = state.lock().unwrap();
    if let Some(pid) = st.pid {
        unsafe { libc::kill(-pid, libc::SIGTERM) };
    }
}

struct PackageInfo {
    title: String,
    meta: String,
    icon: Option<PathBuf>,
    frameworks: Vec<(String, String, bool)>,
    error: String,
    ok: bool,
}

fn package_info(paths: &Paths, package: &Path) -> Result<PackageInfo, String> {
    let icon = std::env::temp_dir().join(format!("wine-uwp-installer-{}.png", std::process::id()));
    let _ = fs::remove_file(&icon);
    let out = Command::new(&paths.py)
        .arg(paths.tool("install-app.py"))
        .arg(package)
        .args(["--info", "--icon"])
        .arg(&icon)
        .arg("--windows")
        .arg(&paths.windows)
        .output()
        .map_err(|e| e.to_string())?;
    if !out.status.success() {
        let text = if out.stderr.is_empty() { &out.stdout } else { &out.stderr };
        return Err(String::from_utf8_lossy(text).trim().lines().last().unwrap_or("This file is not an app package.").to_string());
    }
    let info: serde_json::Value = serde_json::from_slice(&out.stdout).map_err(|e| e.to_string())?;
    let s = |k: &str| info[k].as_str().unwrap_or("").to_string();
    let arch = s("architecture");
    let frameworks: Vec<(String, String, bool)> = info["frameworks"]
        .as_array()
        .map(|a| {
            a.iter()
                .map(|f| (f["name"].as_str().unwrap_or("").to_string(), format!("{} or later", f["min_version"].as_str().unwrap_or("")), f["available"].as_bool().unwrap_or(false)))
                .collect()
        })
        .unwrap_or_default();
    let all = frameworks.iter().all(|f| f.2);
    let arch_ok = arch.eq_ignore_ascii_case("x64") || arch.eq_ignore_ascii_case("neutral");
    Ok(PackageInfo {
        title: s("title"),
        meta: format!("Version {} · {} · {}", s("version"), s("publisher"), arch),
        icon: info["icon"].is_string().then_some(icon),
        error: if !arch_ok {
            format!("This package is built for {arch}; wine-uwp needs x64.")
        } else if !all {
            "Some frameworks this package needs are not in the Windows components.".into()
        } else {
            String::new()
        },
        ok: all && arch_ok,
        frameworks,
    })
}

fn apply_package(ui: &Installer, info: PackageInfo) {
    ui.set_package_title(info.title.into());
    ui.set_package_meta(info.meta.into());
    if let Some(img) = info.icon.and_then(|p| slint::Image::load_from_path(&p).ok()) {
        ui.set_package_icon(img);
    }
    let model: Vec<Framework> = info.frameworks.into_iter().map(|(n, v, ok)| Framework { name: n.into(), version: v.into(), ok }).collect();
    ui.set_frameworks(ModelRc::new(VecModel::from(model)));
    ui.set_package_ok(info.ok);
    ui.set_package_error(info.error.into());
    ui.set_has_package(true);
}

fn install(state: State, ui: Weak<Installer>, paths: Paths, package: PathBuf) {
    set_step(&ui, 0, 1, None);
    let mut installed = None;
    let u = ui.clone();
    let (rc, reason) = run_tool(&state, &ui, &[paths.py.clone(), paths.tool("install-app.py"), package.to_string_lossy().into(), paths.data.to_string_lossy().into(), "--windows".into(), paths.windows.to_string_lossy().into()], &[], |l| {
        if let Some(s) = l.strip_prefix("STAGE Unpacking") {
            set_step(&u, 0, 2, None);
            set_step(&u, 1, 1, Some(format!("Unpacking{s}")));
        } else if let Some(p) = l.strip_prefix("PROGRESS ") {
            let v: Vec<f64> = p.split_whitespace().filter_map(|x| x.parse().ok()).collect();
            if v.len() == 2 {
                progress(&u, (v[0] / v[1].max(1.0)) as f32, format!("{} %", v[0]));
            }
        } else if l.starts_with("STAGE Installing frameworks") {
            set_step(&u, 1, 2, None);
            set_step(&u, 2, 1, None);
        } else if let Some(f) = l.strip_prefix("framework ") {
            set_step(&u, 2, 1, Some(f.to_string()));
        } else if let Some(a) = l.strip_prefix("INSTALLED ") {
            installed = Some(a.trim().to_string());
        }
    });
    let Some(app) = installed.filter(|_| rc == 0) else {
        return fail(&state, &ui, "Could not install the package", if reason.is_empty() { "install-app.py failed.".into() } else { reason }, true);
    };
    for i in 0..3 {
        set_step(&ui, i, 2, Some(String::new()));
    }
    progress(&ui, 1.0, "Installed".into());
    state.lock().unwrap().installed = Some(app.clone());
    std::thread::sleep(Duration::from_millis(500));
    let p = paths.clone();
    on_ui(&ui, move |ui| show_done(ui, &p, &app));
}

fn show_done(ui: &Installer, paths: &Paths, app: &str) {
    ui.set_app_name(app_title(paths, app).into());
    let icon = paths.data.join("stage").join(app).join(".wine-uwp-icon.png");
    if let Ok(img) = slint::Image::load_from_path(&icon) {
        ui.set_done_icon(img);
    }
    ui.set_page(PAGE_DONE);
}

/// A menu entry for the app and a handler for its URI schemes (a browser sign-in comes back through one). Inside
/// a Flatpak the Flatpak exports its own, so this is only offered outside one.
fn write_desktop_entry(paths: &Paths, app: &str) -> std::io::Result<()> {
    let base = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share"));
    let apps = base.join("applications");
    fs::create_dir_all(&apps)?;
    let stage = paths.data.join("stage").join(app);
    let title = app_title(paths, app);
    // a data directory chosen with WINE_UWP_DATA has to travel with the entries, or a sign-in link would look for the
    // app in the default one and start a second copy from there
    let launcher = match std::env::var_os("WINE_UWP_DATA") {
        Some(d) => format!("env \"WINE_UWP_DATA={}\" \"{}\"", d.to_string_lossy(), paths.launcher()),
        None => format!("\"{}\"", paths.launcher()),
    };
    let icon = stage.join(".wine-uwp-icon.png");
    let icon = if icon.exists() { icon.to_string_lossy().into_owned() } else { "application-x-executable".into() };
    fs::write(
        apps.join(format!("wine-uwp-{app}.desktop")),
        format!("[Desktop Entry]\nType=Application\nName={title}\nComment=Runs with wine-uwp\nExec={launcher} run {app}\nIcon={icon}\nCategories=Graphics;Engineering;\nStartupWMClass={}.exe\n", title.to_lowercase()),
    )?;
    let schemes: Vec<String> = fs::read_to_string(stage.join(".wine-uwp-protocols")).unwrap_or_default().lines().filter(|l| !l.is_empty()).map(String::from).collect();
    let host = |args: &[&str]| {
        // inside an AppImage, host tools get the host's own environment back (usr/bin/host-run)
        let hr = paths.bin.join("host-run");
        let mut c = if std::env::var_os("APPIMAGE").is_some() && hr.exists() { Command::new(hr) } else { Command::new(args[0]) };
        if std::env::var_os("APPIMAGE").is_some() && paths.bin.join("host-run").exists() {
            c.args(args);
        } else {
            c.args(&args[1..]);
        }
        let _ = c.stdout(Stdio::null()).stderr(Stdio::null()).status();
    };
    if !schemes.is_empty() {
        let handler = format!("wine-uwp-{app}-handler.desktop");
        let mime: String = schemes.iter().map(|s| format!("x-scheme-handler/{s};")).collect();
        fs::write(
            apps.join(&handler),
            format!("[Desktop Entry]\nType=Application\nName={title} sign-in\nNoDisplay=true\nExec={launcher} activate %u\nMimeType={mime}\n"),
        )?;
        for s in &schemes {
            host(&["xdg-mime", "default", &handler, &format!("x-scheme-handler/{s}")]);
        }
    }
    host(&["update-desktop-database", &apps.to_string_lossy()]);
    // KDE resolves handlers from its own cache; until it is rebuilt a sign-in link opens a "Choose Application" dialog
    host(&["kbuildsycoca6"]);
    Ok(())
}
