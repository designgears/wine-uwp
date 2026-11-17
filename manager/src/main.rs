// SPDX-License-Identifier: LGPL-2.1-or-later
//! wine-uwp-manager: the installed apps across data directories, their layered settings, installs, logs and
//! Xbox Live accounts. Everything it does goes through the tree's own tools (run.sh, wineuwp-install, the
//! profiles), so the manager never has a second idea of how an app runs.

mod envfile;
mod jobs;
mod runtime;

use runtime::{App, Config, Runtime};
use slint::{Model, ModelRc, SharedString, StandardListViewItem, VecModel};
use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;

slint::include_modules!();

struct State {
    rt: Runtime,
    cfg: Config,
    apps: Vec<App>,
    local: Vec<(String, String)>,
    logs: Vec<PathBuf>,
    children: Vec<(String, std::process::Child)>,
}

fn image(path: &Option<PathBuf>) -> Option<slint::Image> {
    path.as_ref().and_then(|p| slint::Image::load_from_path(p).ok())
}

fn refresh_apps(ui: &MainWindow, st: &mut State) {
    st.cfg = Config::load();
    st.apps = runtime::discover(&st.rt, &st.cfg);
    let running = jobs::running_packages();
    st.children.retain_mut(|(_, c)| matches!(c.try_wait(), Ok(None)));
    let items: Vec<AppItem> = st
        .apps
        .iter()
        .map(|a| {
            let icon = image(&a.icon);
            AppItem {
                id: a.id().into(),
                title: a.title.clone().into(),
                subtitle: format!("{}  ·  {}", a.version, a.data.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()).into(),
                has_icon: icon.is_some(),
                icon: icon.unwrap_or_default(),
                running: running.contains(&a.full_name),
            }
        })
        .collect();
    ui.set_apps(ModelRc::new(VecModel::from(items)));
    let dirs: Vec<SharedString> = st.cfg.data_dirs(&st.rt).iter().map(|d| d.display().to_string().into()).collect();
    ui.set_datadirs(ModelRc::new(VecModel::from(dirs)));
    let recipes: Vec<RecipeItem> = runtime::recipes(&st.rt)
        .into_iter()
        .map(|r| RecipeItem { family: r.family.into(), name: r.name.into(), script: r.script.into(), datadir: r.default_dir.into() })
        .collect();
    ui.set_recipes(ModelRc::new(VecModel::from(recipes)));
    ui.set_status_text(format!("{} apps in {}", st.apps.len(), st.rt.root.display()).into());
    let sel = ui.get_selected();
    if sel >= st.apps.len() as i32 {
        ui.set_selected(-1);
    }
    if ui.get_selected() >= 0 {
        show_app(ui, st, false);
    }
}

fn show_app(ui: &MainWindow, st: &mut State, reload_local: bool) {
    let Some(app) = st.apps.get(ui.get_selected() as usize).cloned() else { return };
    let running = jobs::running_packages().contains(&app.full_name);
    ui.set_d_title(app.title.clone().into());
    ui.set_d_family(app.family.clone().into());
    ui.set_d_version(app.version.clone().into());
    let icon = image(&app.icon);
    ui.set_d_has_icon(icon.is_some());
    ui.set_d_icon(icon.unwrap_or_default());
    ui.set_d_running(running);
    ui.set_d_stage(app.stage.display().to_string().into());
    ui.set_d_data(app.data.display().to_string().into());
    ui.set_d_profile(runtime::profile_path(&st.rt, &app).map(|p| p.display().to_string()).unwrap_or_default().into());
    if reload_local {
        ui.set_d_prefix(st.cfg.get(&format!("prefix:{}", app.data.display())).unwrap_or("").into());
        ui.set_d_build(st.cfg.get(&format!("build:{}", app.data.display())).unwrap_or("").into());
        let o = jobs::launch_options(&st.cfg, &app);
        ui.set_d_winedebug(o.winedebug.into());
        ui.set_d_display(o.display.into());
        st.local = envfile::local_pairs(&app.stage.join("local.env"));
        set_local(ui, st);
        st.logs = jobs::app_logs(&app);
        let names: Vec<SharedString> = st.logs.iter().map(|p| p.file_name().unwrap().to_string_lossy().into_owned().into()).collect();
        ui.set_d_logs(ModelRc::new(VecModel::from(names)));
        ui.set_d_log_index(0);
        ui.set_d_log_text(st.logs.first().map(|p| jobs::tail(p, 200_000)).unwrap_or_else(|| "No logs yet: logs of runs started here appear in this list.".into()).into());
    }
    let rows: Vec<ModelRc<StandardListViewItem>> = runtime::effective_env(&st.rt, &app)
        .into_iter()
        .map(|v| {
            ModelRc::new(VecModel::from(vec![
                StandardListViewItem::from(SharedString::from(v.name)),
                StandardListViewItem::from(SharedString::from(v.value)),
                StandardListViewItem::from(SharedString::from(v.source)),
            ]))
        })
        .collect();
    ui.set_d_env(ModelRc::new(VecModel::from(rows)));
}

fn set_local(ui: &MainWindow, st: &State) {
    let rows: Vec<EnvRow> = st.local.iter().map(|(n, v)| EnvRow { name: n.clone().into(), value: v.clone().into() }).collect();
    ui.set_d_local(ModelRc::new(VecModel::from(rows)));
}

fn data_dir_arg(rt: &Runtime, text: &str) -> PathBuf {
    let p = PathBuf::from(text.trim());
    if p.is_absolute() {
        p
    } else {
        rt.root.join(p)
    }
}

/// The install progress, fed from the background command's lines.
fn install_line(ui: &MainWindow, line: &str) {
    let mut out = ui.get_inst_output().to_string();
    if let Some(rest) = line.strip_prefix("PROGRESS ") {
        let mut it = rest.split_whitespace().filter_map(|n| n.parse::<f64>().ok());
        if let (Some(d), Some(t)) = (it.next(), it.next()) {
            if t > 0.0 {
                ui.set_inst_progress((d / t) as f32);
            }
        }
        return;
    }
    if let Some(stage) = line.strip_prefix("STAGE ") {
        ui.set_inst_stage(stage.into());
        ui.set_inst_progress(0.0);
    }
    // curl's progress meter rewrites one line; keep the log readable
    if line.contains('\r') || line.trim().starts_with('%') {
        return;
    }
    out.push_str(line);
    out.push('\n');
    if out.len() > 60_000 {
        out = out[out.len() - 50_000..].to_string();
    }
    ui.set_inst_output(out.into());
}

fn run_install(ui: &MainWindow, st: &State, cmd: std::process::Command, what: String) {
    ui.set_inst_busy(true);
    ui.set_inst_stage(format!("Installing {what}").into());
    ui.set_inst_progress(0.0);
    ui.set_inst_output("".into());
    let _ = std::fs::create_dir_all(jobs::logs_dir());
    let log = jobs::logs_dir().join(format!("install-{}.log", std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)));
    let w1 = ui.as_weak();
    let w2 = ui.as_weak();
    let r = jobs::stream(
        cmd,
        Some(log),
        move |line| {
            let w = w1.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = w.upgrade() {
                    install_line(&ui, &line);
                }
            });
        },
        move |ok| {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = w2.upgrade() {
                    ui.set_inst_busy(false);
                    ui.set_inst_stage(if ok { "Installed".into() } else { "The install failed: see the output".into() });
                    if ok {
                        ui.set_inst_progress(1.0);
                    }
                    ui.invoke_refresh();
                }
            });
        },
    );
    if let Err(e) = r {
        ui.set_inst_busy(false);
        ui.set_inst_stage(format!("Could not start: {e}").into());
    }
    let _ = st;
}

/// Account status per prefix in use, in the background (each check starts Wine).
fn refresh_accounts(ui: &MainWindow, st: &State) {
    if ui.get_accounts_busy() {
        return;
    }
    let rt = st.rt.clone();
    let cfg = st.cfg.clone();
    // one entry per prefix, with the data directories using it and the title clients their apps need
    let mut prefixes: Vec<(PathBuf, PathBuf, Vec<String>, Vec<String>)> = Vec::new();
    for app in &st.apps {
        let prefix = cfg.prefix_for(&rt, &app.data);
        let need = runtime::effective_env(&rt, app).into_iter().find(|v| v.name == "WINE_UWP_XBOX_TITLE_APP").map(|v| v.value);
        match prefixes.iter_mut().find(|p| p.0 == prefix) {
            Some(p) => {
                let d = app.data.display().to_string();
                if !p.2.contains(&d) {
                    p.2.push(d);
                }
                if let Some(n) = need {
                    if !p.3.contains(&n) {
                        p.3.push(n);
                    }
                }
            }
            None => prefixes.push((prefix, app.data.clone(), vec![app.data.display().to_string()], need.into_iter().collect())),
        }
    }
    if rt.probe_xboxauth().is_none() {
        ui.set_accounts(ModelRc::new(VecModel::from(vec![AccountItem {
            datadir: "".into(),
            prefix: "".into(),
            status: "probe-xboxauth.exe is not built (./build.sh)".into(),
            clients: "".into(),
            needs: "".into(),
        }])));
        return;
    }
    ui.set_accounts_busy(true);
    let w = ui.as_weak();
    std::thread::spawn(move || {
        let mut items = Vec::new();
        for (prefix, data, dirs, needs) in prefixes {
            let mut status = String::from("Status unavailable: this prefix's Wine build has no winexboxauth status");
            let mut clients = Vec::new();
            if let Some(mut cmd) = jobs::xboxauth_command(&rt, &cfg, &data, &["status"]) {
                if let Ok(out) = cmd.output() {
                    for line in String::from_utf8_lossy(&out.stdout).lines() {
                        if let Some(rest) = line.strip_prefix("ACCOUNT ") {
                            let p: Vec<&str> = rest.splitn(3, ' ').collect();
                            status = if p[0] == "signed-in" {
                                format!("Signed in as {}", p.get(2).copied().unwrap_or("?"))
                            } else {
                                "Not signed in: sign in from a game or the Xbox app".to_string()
                            };
                        }
                        if let Some(c) = line.strip_prefix("CLIENT ") {
                            clients.push(c.to_string());
                        }
                    }
                }
            }
            let missing = needs.iter().find(|n| !clients.contains(n)).cloned().unwrap_or_default();
            items.push((dirs.join(", "), prefix.display().to_string(), status, clients.join(", "), missing));
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = w.upgrade() {
                let model: Vec<AccountItem> = items
                    .into_iter()
                    .map(|(d, p, s, c, n)| AccountItem { datadir: d.into(), prefix: p.into(), status: s.into(), clients: c.into(), needs: n.into() })
                    .collect();
                ui.set_accounts(ModelRc::new(VecModel::from(model)));
                ui.set_accounts_busy(false);
            }
        });
    });
}

fn main() -> Result<(), slint::PlatformError> {
    let Some(root) = runtime::find_root() else {
        eprintln!("wine-uwp-manager: no wine-uwp tree found (set WINE_UWP_ROOT to the directory with env.sh and run.sh)");
        std::process::exit(1);
    };
    let ui = MainWindow::new()?;
    let st = Rc::new(RefCell::new(State {
        rt: Runtime::load(root),
        cfg: Config::load(),
        apps: Vec::new(),
        local: Vec::new(),
        logs: Vec::new(),
        children: Vec::new(),
    }));
    refresh_apps(&ui, &mut st.borrow_mut());
    if !st.borrow().apps.is_empty() {
        ui.set_selected(0);
        show_app(&ui, &mut st.borrow_mut(), true);
    }
    ui.set_recipe_datadir("".into());

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_refresh(move || {
        let ui = w.unwrap();
        refresh_apps(&ui, &mut s.borrow_mut());
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_select(move |i| {
        let ui = w.unwrap();
        ui.set_selected(i);
        show_app(&ui, &mut s.borrow_mut(), true);
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_play(move || {
        let ui = w.unwrap();
        let mut st = s.borrow_mut();
        let Some(app) = st.apps.get(ui.get_selected() as usize).cloned() else { return };
        match jobs::launch(&st.rt, &st.cfg, &app) {
            Ok((child, log)) => {
                st.children.push((app.id(), child));
                ui.set_status_text(format!("Started {}; log {}", app.title, log.display()).into());
            }
            Err(e) => ui.set_status_text(format!("Could not start {}: {e}", app.title).into()),
        }
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_stop(move || {
        let ui = w.unwrap();
        let st = s.borrow();
        let Some(app) = st.apps.get(ui.get_selected() as usize).cloned() else { return };
        let (rt, cfg) = (st.rt.clone(), st.cfg.clone());
        ui.set_status_text(format!("Stopping {} (everything in its prefix)", app.title).into());
        std::thread::spawn(move || {
            let _ = jobs::stop(&rt, &cfg, &app);
        });
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_save_options(move || {
        let ui = w.unwrap();
        let mut st = s.borrow_mut();
        let Some(app) = st.apps.get(ui.get_selected() as usize).cloned() else { return };
        st.cfg.set(&format!("prefix:{}", app.data.display()), ui.get_d_prefix().trim());
        st.cfg.set(&format!("build:{}", app.data.display()), ui.get_d_build().trim());
        st.cfg.set(&format!("winedebug:{}", app.id()), ui.get_d_winedebug().trim());
        st.cfg.set(&format!("display:{}", app.id()), ui.get_d_display().trim());
        match st.cfg.save() {
            Ok(()) => ui.set_status_text(format!("Saved to {}", runtime::config_path().display()).into()),
            Err(e) => ui.set_status_text(format!("Could not save: {e}").into()),
        }
    });

    let s = st.clone();
    ui.on_local_edit(move |i, n, v| {
        if let Some(row) = s.borrow_mut().local.get_mut(i as usize) {
            *row = (n.to_string(), v.to_string());
        }
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_local_add(move || {
        let ui = w.unwrap();
        let mut st = s.borrow_mut();
        st.local.push((String::new(), String::new()));
        set_local(&ui, &st);
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_local_remove(move |i| {
        let ui = w.unwrap();
        let mut st = s.borrow_mut();
        if (i as usize) < st.local.len() {
            st.local.remove(i as usize);
        }
        set_local(&ui, &st);
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_local_save(move || {
        let ui = w.unwrap();
        let mut st = s.borrow_mut();
        let Some(app) = st.apps.get(ui.get_selected() as usize).cloned() else { return };
        let path = app.stage.join("local.env");
        let r = if st.local.iter().all(|(n, _)| n.trim().is_empty()) {
            std::fs::remove_file(&path).or_else(|e| if e.kind() == std::io::ErrorKind::NotFound { Ok(()) } else { Err(e) })
        } else {
            envfile::write_local(&path, &st.local)
        };
        match r {
            Ok(()) => ui.set_status_text("Saved local.env; it applies from the next start".into()),
            Err(e) => ui.set_status_text(format!("Could not save local.env: {e}").into()),
        }
        show_app(&ui, &mut st, true);
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_select_log(move |i| {
        let ui = w.unwrap();
        let st = s.borrow();
        if let Some(p) = st.logs.get(i as usize) {
            ui.set_d_log_text(jobs::tail(p, 200_000).into());
        }
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_refresh_log(move || {
        let ui = w.unwrap();
        let st = s.borrow();
        if let Some(p) = st.logs.get(ui.get_d_log_index() as usize) {
            ui.set_d_log_text(jobs::tail(p, 200_000).into());
        }
    });

    ui.on_open_folder(|path| {
        let _ = std::process::Command::new("xdg-open").arg(path.as_str()).spawn();
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_install(move || {
        let ui = w.unwrap();
        let st = s.borrow();
        let source = ui.get_inst_source().trim().to_string();
        let data = ui.get_inst_datadir().trim().to_string();
        if source.is_empty() || data.is_empty() {
            ui.set_inst_stage("Give a package and a data directory".into());
            ui.set_inst_output("".into());
            return;
        }
        let data = data_dir_arg(&st.rt, &data);
        let _ = std::fs::create_dir_all(&data);
        let cmd = jobs::install_command(&st.rt, &st.cfg, &source, &data, ui.get_inst_store());
        run_install(&ui, &st, cmd, source);
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_install_recipe(move |i| {
        let ui = w.unwrap();
        let st = s.borrow();
        let Some(r) = runtime::recipes(&st.rt).into_iter().nth(i as usize) else { return };
        let dir = Some(ui.get_recipe_datadir().trim().to_string()).filter(|d| !d.is_empty()).unwrap_or(r.default_dir.clone());
        if dir.is_empty() {
            ui.set_inst_stage("Give a data directory".into());
            return;
        }
        let data = data_dir_arg(&st.rt, &dir);
        let cmd = jobs::recipe_command(&st.rt, &st.cfg, &r.script, &data, &ui.get_recipe_version());
        run_install(&ui, &st, cmd, r.name);
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_refresh_accounts(move || {
        let ui = w.unwrap();
        refresh_accounts(&ui, &s.borrow());
    });

    let w = ui.as_weak();
    let s = st.clone();
    ui.on_approve(move |datadir, client| {
        let ui = w.unwrap();
        let st = s.borrow();
        let data = PathBuf::from(datadir.split(", ").next().unwrap_or(""));
        let Some(cmd) = jobs::xboxauth_command(&st.rt, &st.cfg, &data, &["clientdevice", client.as_str()]) else { return };
        ui.set_device_code("Starting...".into());
        let w1 = ui.as_weak();
        let w2 = ui.as_weak();
        let _ = jobs::stream(
            cmd,
            None,
            move |line| {
                if let Some(code) = line.split("Enter the code").nth(1) {
                    let code = code.trim().to_string();
                    let w = w1.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = w.upgrade() {
                            ui.set_device_code(code.into());
                        }
                    });
                }
            },
            move |ok| {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = w2.upgrade() {
                        ui.set_device_code(if ok { "".into() } else { "The approval did not finish".into() });
                        ui.invoke_refresh_accounts();
                    }
                });
            },
        );
    });

    // running state, every two seconds
    let w = ui.as_weak();
    let s = st.clone();
    let timer = slint::Timer::default();
    timer.start(slint::TimerMode::Repeated, std::time::Duration::from_secs(2), move || {
        let Some(ui) = w.upgrade() else { return };
        let running = jobs::running_packages();
        let st = s.borrow();
        let model = ui.get_apps();
        for (i, app) in st.apps.iter().enumerate() {
            if let Some(mut item) = model.row_data(i) {
                let r = running.contains(&app.full_name);
                if item.running != r {
                    item.running = r;
                    model.set_row_data(i, item);
                }
            }
        }
        if let Some(app) = st.apps.get(ui.get_selected() as usize) {
            ui.set_d_running(running.contains(&app.full_name));
        }
    });

    let _ = Path::new("");
    ui.run()
}
