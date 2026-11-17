// SPDX-License-Identifier: LGPL-2.1-or-later
//! The env files wineuwp-launch reads, with its exact semantics: one NAME=value per line, `#` comments, an optional
//! `export `, surrounding quotes removed, nothing expanded, and NAME+=value appending with ';'.

use std::path::Path;

#[derive(Clone, Debug)]
pub struct Entry {
    pub name: String,
    pub value: String,
    pub append: bool,
}

pub fn parse(text: &str) -> Vec<Entry> {
    let mut out = Vec::new();
    for raw in text.lines() {
        let line = raw.trim_end_matches(['\r', ' ']).trim_start_matches([' ', '\t']);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let line = line.strip_prefix("export ").unwrap_or(line);
        let Some(eq) = line.find('=') else { continue };
        if eq == 0 {
            continue;
        }
        let (mut name, mut value) = (&line[..eq], &line[eq + 1..]);
        let append = name.ends_with('+');
        if append {
            name = &name[..name.len() - 1];
        }
        let b = value.as_bytes();
        if b.len() >= 2 && (b[0] == b'\'' || b[0] == b'"') && b[b.len() - 1] == b[0] {
            value = &value[1..value.len() - 1];
        }
        out.push(Entry { name: name.to_string(), value: value.to_string(), append });
    }
    out
}

pub fn read(path: &Path) -> Vec<Entry> {
    std::fs::read_to_string(path).map(|t| parse(&t)).unwrap_or_default()
}

/// One variable of the effective environment and the layer that set it last.
#[derive(Clone, Debug)]
pub struct Value {
    pub name: String,
    pub value: String,
    pub source: String,
}

/// Applies layers in order, as the launcher does (a `keep` layer does not replace what is already set).
pub fn layer(layers: &[(&str, Vec<Entry>, bool)]) -> Vec<Value> {
    let mut vals: Vec<Value> = Vec::new();
    for (source, entries, keep) in layers {
        for e in entries {
            match vals.iter_mut().find(|v| v.name == e.name) {
                Some(v) if e.append => {
                    if !v.value.is_empty() {
                        v.value.push(';');
                    }
                    v.value.push_str(&e.value);
                    v.source = format!("{} + {}", v.source, source);
                }
                Some(_) if *keep => {}
                Some(v) => {
                    v.value = e.value.clone();
                    v.source = source.to_string();
                }
                None => vals.push(Value { name: e.name.clone(), value: e.value.clone(), source: source.to_string() }),
            }
        }
    }
    vals
}

/// Writes local.env from NAME=value pairs, keeping a header that says what the file is.
pub fn write_local(path: &Path, pairs: &[(String, String)]) -> std::io::Result<()> {
    let mut text = String::from(
        "# This install's own settings, read last by wineuwp-launch (after the defaults and the app's profile).\n\
         # Written by wine-uwp-manager; values are literal, NAME+=value appends with ';'.\n",
    );
    for (n, v) in pairs {
        let n = n.trim();
        if n.is_empty() {
            continue;
        }
        text.push_str(n);
        text.push('=');
        text.push_str(v);
        text.push('\n');
    }
    let tmp = path.with_extension("env.tmp");
    std::fs::write(&tmp, text)?;
    std::fs::rename(tmp, path)
}

/// local.env as editable pairs (an appending line keeps its `+` in the name).
pub fn local_pairs(path: &Path) -> Vec<(String, String)> {
    read(path)
        .into_iter()
        .map(|e| (if e.append { format!("{}+", e.name) } else { e.name }, e.value))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_like_the_launcher() {
        let e = parse("# c\nexport A='x y'\nB+=c;d\n =bad\nC=\"q\"\n");
        assert_eq!(e.len(), 3);
        assert_eq!(e[0].value, "x y");
        assert!(e[1].append && e[1].name == "B");
        assert_eq!(e[2].value, "q");
    }

    #[test]
    fn layers_append_and_keep() {
        let v = layer(&[
            ("defaults", parse("A=1\nB=x"), true),
            ("profile", parse("A+=2\nB=y"), false),
            ("local", parse("C=3"), false),
        ]);
        assert_eq!(v[0].value, "1;2");
        assert_eq!(v[0].source, "defaults + profile");
        assert_eq!(v[1].value, "y");
        assert_eq!(v[2].source, "local");
    }
}
