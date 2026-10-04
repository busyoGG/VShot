// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! What a codec will let a caller tune, and the values it was given.
//!
//! A format is not only a file suffix: AVIF has a quality and a speed, PNG has
//! a compression level, and Radiance has nothing at all.  Those differences are
//! the format's own business, so each codec declares its parameters here rather
//! than the settings window knowing about any of them by name.
//!
//! The declaration is deliberately a *description* and not a widget: [`ParamKind`]
//! carries the range a control has to be bounded by and the default it opens on,
//! which is everything a window needs to draw a row, without this side having to
//! know what a spin box is.  The same description travels out of the program over
//! `vshot formats --json`, which is how the Qt side builds those rows.
//!
//! Values come back as [`ParamValues`], indexed by the spec's own name.  They are
//! clamped to the declared range rather than refused, matching how every other
//! number in this program is read: a value out of range has a defined answer, and
//! refusing the capture over a quality of 200 would be the wrong trade.

/// The shape of one parameter's value, and the range it lives in.
///
/// The bounds are here rather than in the settings window because the codec is
/// what has to hold them: a window bounded by a different pair would offer a
/// value the encoder silently overrides.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ParamKind {
    /// One of a fixed set of names; the value *is* the name.  `default` is
    /// named rather than taken to be `values[0]`: the order a window lists
    /// choices in is the order that reads best, which is not always the order
    /// that puts the default first — PNG's levels run `none` to `high` while
    /// the one a capture uses unless told otherwise is `fast`.
    Choice {
        values: &'static [&'static str],
        default: &'static str,
    },
    /// A whole number in `min..=max`, moving in `step`s.
    Integer {
        min: i64,
        max: i64,
        step: i64,
        default: i64,
    },
    /// A real number in `min..=max`, shown with `decimals` places.
    Number {
        min: f64,
        max: f64,
        step: f64,
        decimals: u32,
        default: f64,
    },
}

/// One parameter a format accepts.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ParamSpec {
    /// The name the config file and the command line use for it, kebab-case.
    pub name: &'static str,
    /// The row's label, in the settings window.
    pub label: &'static str,
    /// The line under the label saying what the parameter does.
    pub hint: &'static str,
    pub kind: ParamKind,
}

impl ParamSpec {
    /// The value this parameter takes when nothing says otherwise.
    pub fn default_value(&self) -> ParamValue {
        match self.kind {
            ParamKind::Choice { default, .. } => ParamValue::Text(default.to_string()),
            ParamKind::Integer { default, .. } => ParamValue::Integer(default),
            ParamKind::Number { default, .. } => ParamValue::Number(default),
        }
    }

    /// Clamps `value` into this parameter's range, reading it the way its own
    /// kind says.  A value of the wrong kind reads as the default rather than
    /// as a guess.
    ///
    /// Text is accepted wherever a number belongs, because the command line has
    /// nothing but text: `--format-param avif.quality=40` arrives as the string
    /// `40`, and refusing it for not being a number would make every numeric
    /// parameter unreachable from the one place a user is most likely to set
    /// it.  Text that does not parse is a value of the wrong kind, and reads as
    /// the default like any other.
    pub fn coerce(&self, value: ParamValue) -> ParamValue {
        match (self.kind, value) {
            (ParamKind::Choice { values, .. }, ParamValue::Text(text)) => {
                if values.contains(&text.as_str()) {
                    ParamValue::Text(text)
                } else {
                    self.default_value()
                }
            }
            (ParamKind::Choice { .. }, _) => self.default_value(),
            (ParamKind::Integer { min, max, .. }, ParamValue::Integer(number)) => {
                ParamValue::Integer(number.clamp(min, max))
            }
            (ParamKind::Integer { min, max, .. }, ParamValue::Number(number)) => {
                // A config file may hold `4.0` where the parameter is whole;
                // rounding it is friendlier than dropping the user's value.
                ParamValue::Integer((number.round() as i64).clamp(min, max))
            }
            (ParamKind::Integer { min, max, .. }, ParamValue::Text(text)) => {
                match text.trim().parse() {
                    Ok(number) => ParamValue::Integer(i64::clamp(number, min, max)),
                    Err(_) => self.default_value(),
                }
            }
            (ParamKind::Number { min, max, .. }, ParamValue::Number(number)) => {
                if number.is_finite() {
                    ParamValue::Number(number.clamp(min, max))
                } else {
                    self.default_value()
                }
            }
            (ParamKind::Number { min, max, .. }, ParamValue::Integer(number)) => {
                ParamValue::Number((number as f64).clamp(min, max))
            }
            (ParamKind::Number { min, max, .. }, ParamValue::Text(text)) => {
                match text.trim().parse() {
                    Ok(number) => {
                        let number: f64 = number;
                        if number.is_finite() {
                            ParamValue::Number(number.clamp(min, max))
                        } else {
                            self.default_value()
                        }
                    }
                    Err(_) => self.default_value(),
                }
            }
        }
    }
}

/// One parameter's value.
#[derive(Clone, Debug, PartialEq)]
pub enum ParamValue {
    Text(String),
    Integer(i64),
    Number(f64),
}

/// A format's whole set of values, in the order its [`ParamSpec`]s come in.
///
/// Built from the specs first — so every parameter has a value even when
/// nothing was configured — and then overwritten by whatever the config file
/// and the command line had to say.  That is why a lookup can never miss.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ParamValues {
    values: Vec<(&'static str, ParamValue)>,
}

impl ParamValues {
    /// The defaults for `specs`, with nothing configured.
    pub fn defaults(specs: &'static [ParamSpec]) -> Self {
        Self {
            values: specs
                .iter()
                .map(|spec| (spec.name, spec.default_value()))
                .collect(),
        }
    }

    /// Replaces the value of `name`, clamped to that parameter's own range.
    ///
    /// A name no spec declares is dropped: the config file is allowed to carry
    /// a key from a newer build, and the format it belonged to is the one that
    /// would know what to do with it.
    pub fn set(&mut self, specs: &'static [ParamSpec], name: &str, value: ParamValue) {
        let Some(spec) = specs.iter().find(|spec| spec.name == name) else {
            return;
        };
        let value = spec.coerce(value);
        match self.values.iter_mut().find(|(key, _)| *key == name) {
            Some(entry) => entry.1 = value,
            None => self.values.push((spec.name, value)),
        }
    }

    fn get(&self, name: &str) -> Option<&ParamValue> {
        self.values
            .iter()
            .find(|(key, _)| *key == name)
            .map(|(_, value)| value)
    }

    /// The whole number `name` holds, or `fallback` when it holds something
    /// else — which only happens when the caller asks for a parameter the
    /// format does not declare.
    pub fn integer(&self, name: &str, fallback: i64) -> i64 {
        match self.get(name) {
            Some(ParamValue::Integer(value)) => *value,
            _ => fallback,
        }
    }

    /// The real number `name` holds, or `fallback`.
    pub fn number(&self, name: &str, fallback: f64) -> f64 {
        match self.get(name) {
            Some(ParamValue::Number(value)) => *value,
            Some(ParamValue::Integer(value)) => *value as f64,
            _ => fallback,
        }
    }

    /// The name `name` holds, or `fallback`.
    pub fn text<'a>(&'a self, name: &str, fallback: &'a str) -> &'a str {
        match self.get(name) {
            Some(ParamValue::Text(value)) => value.as_str(),
            _ => fallback,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SPECS: &[ParamSpec] = &[
        ParamSpec {
            name: "compression",
            label: "Compression",
            hint: "How hard the encoder works",
            kind: ParamKind::Choice {
                values: &["none", "fast", "high"],
                default: "fast",
            },
        },
        ParamSpec {
            name: "quality",
            label: "Quality",
            hint: "Higher is bigger and closer",
            kind: ParamKind::Integer {
                min: 1,
                max: 100,
                step: 1,
                default: 90,
            },
        },
        ParamSpec {
            name: "gamma",
            label: "Gamma",
            hint: "A real number",
            kind: ParamKind::Number {
                min: 0.0,
                max: 4.0,
                step: 0.1,
                decimals: 1,
                default: 1.0,
            },
        },
    ];

    /// Every declared parameter has a value before anything is configured, so a
    /// codec can read its own parameters without checking whether they arrived.
    #[test]
    fn the_defaults_cover_every_spec() {
        let values = ParamValues::defaults(SPECS);
        // The choice's default is the one it names, which is not the first
        // name in the list.
        assert_eq!(values.text("compression", "?"), "fast");
        assert_eq!(values.integer("quality", -1), 90);
        assert_eq!(values.number("gamma", -1.0), 1.0);
    }

    /// A value out of range is brought into it rather than refused, which is
    /// what every other number in this program does.
    #[test]
    fn a_value_out_of_range_is_clamped() {
        let mut values = ParamValues::defaults(SPECS);
        values.set(SPECS, "quality", ParamValue::Integer(500));
        assert_eq!(values.integer("quality", -1), 100);
        values.set(SPECS, "quality", ParamValue::Integer(-20));
        assert_eq!(values.integer("quality", -1), 1);

        values.set(SPECS, "gamma", ParamValue::Number(9.0));
        assert_eq!(values.number("gamma", -1.0), 4.0);
        values.set(SPECS, "gamma", ParamValue::Number(f64::NAN));
        assert_eq!(
            values.number("gamma", -1.0),
            1.0,
            "NaN reads as the default"
        );
    }

    /// A choice only takes a name it offers; anything else falls back, which is
    /// the same rule `PngCompression::parse` applies to the flag.
    #[test]
    fn a_choice_outside_the_list_reads_as_the_default() {
        let mut values = ParamValues::defaults(SPECS);
        values.set(SPECS, "compression", ParamValue::Text("high".into()));
        assert_eq!(values.text("compression", "?"), "high");
        values.set(SPECS, "compression", ParamValue::Text("slowest".into()));
        assert_eq!(values.text("compression", "?"), "fast");
    }

    /// A value of the wrong kind reads as the default rather than as a guess,
    /// and a whole number may arrive as a real one from a hand-written file.
    #[test]
    fn a_value_of_the_wrong_kind_reads_as_the_default() {
        let mut values = ParamValues::defaults(SPECS);
        values.set(SPECS, "quality", ParamValue::Text("high".into()));
        assert_eq!(values.integer("quality", -1), 90);
        values.set(SPECS, "quality", ParamValue::Number(40.0));
        assert_eq!(values.integer("quality", -1), 40);
    }

    /// A key no format declares is dropped instead of becoming a parameter.
    #[test]
    fn an_undeclared_name_is_dropped() {
        let mut values = ParamValues::defaults(SPECS);
        values.set(SPECS, "sharpness", ParamValue::Integer(3));
        assert_eq!(values.integer("sharpness", -1), -1);
    }
}
