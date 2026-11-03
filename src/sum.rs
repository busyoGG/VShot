// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Adding up the numbers in recognized text.
//!
//! `vshot sum` reads the same text `vshot ocr` produces -- a list of
//! [`OcrLine`]s -- and returns the sum of every number in it.  The editor's
//! Sum button is the main caller: it crops the selection, recognizes it and
//! hands the envelope to this module, so the arithmetic lives on one side of
//! the wire and the Qt helper only draws the answer.
//!
//! The numbers are added as exact decimal values, not as floating point: a
//! screenshot of a column of prices is the case this exists for, and
//! `0.1 + 0.2` coming out as `0.30000000000000004` would be a wrong answer to
//! the question that was asked.  A value is kept as an integer scaled by a
//! power of ten, and the sum takes the largest scale it has seen.
//!
//! What counts as a number is deliberately plain: a run of digits, optionally
//! signed, optionally grouped in threes with `,`, optionally with a fractional
//! part, in either ASCII or full-width form.  A currency symbol, a percent sign
//! or a unit next to it is not part of the value, so `¥1,200` is `1200` and
//! `50%` is `50` -- the reader asked for the numbers that are on the screen,
//! and the sign that says what they mean is not one.

use std::io::Read;

use crate::error::{Result, VshotError};
use crate::ocr::OcrLine;

/// A sum of the numbers in one or more pieces of text.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Totals {
    /// The sum, scaled by `10^scale`.
    scaled: i128,
    /// How many decimal places `scaled` carries.
    scale: u32,
    /// How many numbers went in.
    pub count: usize,
}

impl Totals {
    /// The sum as a plain decimal string, with no exponent, no trailing zeros
    /// and a `-` only when the sum is below zero.
    pub fn total_text(&self) -> String {
        let factor = 10u128.pow(self.scale);
        let negative = self.scaled < 0;
        let magnitude = self.scaled.unsigned_abs();
        let integer = magnitude / factor;
        let fraction = magnitude % factor;
        let mut text = String::new();
        if negative {
            text.push('-');
        }
        text.push_str(&integer.to_string());
        if self.scale > 0 && fraction != 0 {
            let mut digits = format!("{fraction:0width$}", width = self.scale as usize);
            while digits.ends_with('0') {
                digits.pop();
            }
            text.push('.');
            text.push_str(&digits);
        }
        text
    }

    /// Adds every number in `text`.
    pub fn add_text(&mut self, text: &str) {
        for number in numbers_in(text) {
            self.add(number);
        }
    }

    fn add(&mut self, number: Number) {
        let (left, right, scale) = if number.scale > self.scale {
            match self
                .scaled
                .checked_mul(10i128.pow(number.scale - self.scale))
            {
                Some(value) => (value, number.scaled, number.scale),
                None => return,
            }
        } else {
            match number
                .scaled
                .checked_mul(10i128.pow(self.scale - number.scale))
            {
                Some(value) => (self.scaled, value, self.scale),
                None => return,
            }
        };
        let Some(total) = left.checked_add(right) else {
            return;
        };
        self.scaled = total;
        self.scale = scale;
        self.count += 1;
    }
}

/// One number read out of a text run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Number {
    /// The value scaled by `10^scale`, already signed.
    scaled: i128,
    scale: u32,
}

/// Adds up the numbers in each line's text.
pub fn sum_lines(lines: &[OcrLine]) -> Totals {
    let mut totals = Totals::default();
    for line in lines {
        totals.add_text(&line.text);
    }
    totals
}

/// Adds up the numbers in a list of lines, without the recognition geometry.
pub fn sum_texts<S: AsRef<str>>(texts: &[S]) -> Totals {
    let mut totals = Totals::default();
    for text in texts {
        totals.add_text(text.as_ref());
    }
    totals
}

/// Reads the `vshot ocr --json` envelope and returns its lines' text.
///
/// This is the `--stdin-ocr` route's parser, the same envelope `vshot
/// translate` reads.  Only the text is taken: summing needs no geometry, and a
/// line without a `text` string is skipped rather than failing the document --
/// a translation's envelope carries the same shape, and a caller may pipe
/// either one in.
pub fn texts_from_envelope(document: &str) -> Result<Vec<String>> {
    let value: serde_json::Value = serde_json::from_str(document).map_err(|error| {
        VshotError::Ocr(format!("cannot read the OCR envelope from stdin: {error}"))
    })?;
    let lines = value
        .get("lines")
        .and_then(serde_json::Value::as_array)
        .ok_or_else(|| VshotError::Ocr("the OCR envelope on stdin has no `lines` array".into()))?;
    Ok(lines
        .iter()
        .filter_map(|line| line.get("text").and_then(serde_json::Value::as_str))
        .map(str::to_owned)
        .collect())
}

/// Reads the whole of standard input, for `--stdin-ocr`.
pub fn read_stdin() -> Result<String> {
    let mut document = String::new();
    std::io::stdin()
        .read_to_string(&mut document)
        .map_err(|error| VshotError::Ocr(format!("cannot read stdin: {error}")))?;
    Ok(document)
}

/// The result as the `--json` shape: the total as a string, because a decimal
/// with more places than a double holds would otherwise be rounded on the way
/// out, and the count so a caller can tell "0" from "no numbers at all".
pub fn to_json(totals: &Totals) -> String {
    serde_json::json!({
        "total": totals.total_text(),
        "count": totals.count,
    })
    .to_string()
}

/// Every number in `text`, in the order they appear.
fn numbers_in(text: &str) -> Vec<Number> {
    let chars: Vec<char> = text.chars().collect();
    let mut numbers = Vec::new();
    let mut index = 0;
    while index < chars.len() {
        let (sign, start) = match chars[index] {
            '-' | '−' | '－' if starts_a_number(&chars, index) => (-1i128, index + 1),
            '+' | '＋' if starts_a_number(&chars, index) => (1i128, index + 1),
            _ => (1i128, index),
        };
        if start >= chars.len() || digit_value(chars[start]).is_none() {
            index += 1;
            continue;
        }

        // The integer part, with any groups of three.
        let mut digits = String::new();
        let mut cursor = start;
        while cursor < chars.len() {
            if let Some(digit) = digit_value(chars[cursor]) {
                digits.push(digit);
                cursor += 1;
                continue;
            }
            match group_end(&chars, cursor) {
                Some(next) => {
                    // The group's own three digits are part of the value; the
                    // separator is not.
                    for character in &chars[cursor + 1..next] {
                        if let Some(digit) = digit_value(*character) {
                            digits.push(digit);
                        }
                    }
                    cursor = next;
                }
                None => break,
            }
        }

        // The fractional part: a dot followed by at least one digit.  A dot
        // that ends a sentence is not one.
        let mut scale = 0u32;
        if cursor + 1 < chars.len()
            && is_decimal_point(chars[cursor])
            && digit_value(chars[cursor + 1]).is_some()
        {
            cursor += 1;
            while cursor < chars.len() {
                let Some(digit) = digit_value(chars[cursor]) else {
                    break;
                };
                digits.push(digit);
                scale += 1;
                cursor += 1;
            }
        }

        // A number longer than this is not something a screenshot holds, and
        // skipping it is better than wrapping the sum around.
        if digits.len() > 18 {
            index = cursor;
            continue;
        }
        let Ok(magnitude) = digits.parse::<i128>() else {
            index = cursor;
            continue;
        };
        if scale > 9 {
            index = cursor;
            continue;
        }
        // `digits` already holds the fractional digits, so the scaled value is
        // the number the digits spell and `scale` is where the point sits.
        numbers.push(Number {
            scaled: sign * magnitude,
            scale,
        });
        index = cursor.max(index + 1);
    }
    numbers
}

/// Whether the sign at `at` begins a number rather than joining two of them:
/// `-5` is minus five, `10-20` is ten and twenty.
fn starts_a_number(chars: &[char], at: usize) -> bool {
    if at == 0 {
        return true;
    }
    let previous = chars[at - 1];
    digit_value(previous).is_none()
        && !matches!(previous, ',' | '，' | '.' | '．' | ')' | '）' | '%')
}

/// The digit at `at`, as a character, accepting full-width forms.
fn digit_value(character: char) -> Option<char> {
    match character {
        '0'..='9' => Some(character),
        '０'..='９' => char::from_u32(character as u32 - '０' as u32 + '0' as u32),
        _ => None,
    }
}

fn is_decimal_point(character: char) -> bool {
    matches!(character, '.' | '．')
}

/// Where a thousands group ends, when the separator at `at` starts one:
/// exactly three digits, and not a fourth one right after them.
fn group_end(chars: &[char], at: usize) -> Option<usize> {
    if !matches!(chars[at], ',' | '，') {
        return None;
    }
    let mut cursor = at + 1;
    let mut digits = 0;
    while cursor < chars.len() && digits < 3 {
        digit_value(chars[cursor])?;
        digits += 1;
        cursor += 1;
    }
    if digits != 3 {
        return None;
    }
    if cursor < chars.len() && digit_value(chars[cursor]).is_some() {
        return None;
    }
    Some(cursor)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn total(text: &str) -> (String, usize) {
        let totals = sum_texts(&[text]);
        (totals.total_text(), totals.count)
    }

    #[test]
    fn integers_add_up() {
        assert_eq!(total("1 2 3"), ("6".into(), 3));
        assert_eq!(total("42"), ("42".into(), 1));
    }

    #[test]
    fn decimals_are_exact() {
        // The case the module exists for: a column of prices.
        assert_eq!(total("0.1\n0.2"), ("0.3".into(), 2));
        assert_eq!(total("12.50 7.25"), ("19.75".into(), 2));
        assert_eq!(total("1.125 2.5"), ("3.625".into(), 2));
    }

    #[test]
    fn grouped_digits_are_one_number() {
        assert_eq!(total("1,234.50"), ("1234.5".into(), 1));
        assert_eq!(total("1,234,567"), ("1234567".into(), 1));
        assert_eq!(total("1，234"), ("1234".into(), 1));
    }

    #[test]
    fn a_failed_group_is_not_a_group() {
        // `1,23` is not a thousands group, so the two runs are two numbers.
        assert_eq!(total("1,23"), ("24".into(), 2));
    }

    #[test]
    fn signs_attach_to_numbers_but_not_to_ranges() {
        assert_eq!(total("-5 10"), ("5".into(), 2));
        assert_eq!(total("10-20"), ("30".into(), 2));
        assert_eq!(total("+3.5 -1.5"), ("2".into(), 2));
        assert_eq!(total("(-4)"), ("-4".into(), 1));
    }

    #[test]
    fn full_width_digits_are_digits() {
        assert_eq!(total("１２３．５"), ("123.5".into(), 1));
        assert_eq!(total("－５　１０"), ("5".into(), 2));
    }

    #[test]
    fn units_and_currency_are_not_part_of_the_value() {
        assert_eq!(total("¥1,200 $3.50"), ("1203.5".into(), 2));
        assert_eq!(total("50% 25%"), ("75".into(), 2));
        assert_eq!(total("3 kg"), ("3".into(), 1));
    }

    #[test]
    fn a_trailing_dot_is_not_a_fraction() {
        assert_eq!(total("Version 2."), ("2".into(), 1));
        assert_eq!(total("1.2.3"), ("4.2".into(), 2));
    }

    #[test]
    fn no_numbers_is_an_empty_sum() {
        assert_eq!(total("no digits here"), ("0".into(), 0));
    }

    #[test]
    fn text_is_summed_across_lines() {
        let totals = sum_texts(&["12", "30.5", "7"]);
        assert_eq!(totals.total_text(), "49.5");
        assert_eq!(totals.count, 3);
    }

    #[test]
    fn a_negative_total_keeps_its_sign() {
        assert_eq!(total("-10 3"), ("-7".into(), 2));
    }

    #[test]
    fn the_envelope_route_reads_every_line() {
        let envelope = r#"{"version":1,"geometry":true,"lines":[
            {"text":"12","rect":{"x":0,"y":0,"width":10,"height":10}},
            {"text":"3.5"}]}"#;
        let texts = texts_from_envelope(envelope).expect("the envelope parses");
        assert_eq!(texts, vec!["12".to_owned(), "3.5".to_owned()]);
        assert_eq!(sum_texts(&texts).total_text(), "15.5");
    }

    #[test]
    fn a_document_that_is_not_an_envelope_is_refused() {
        assert!(texts_from_envelope("not json").is_err());
        assert!(texts_from_envelope(r#"{"lines": 3}"#).is_err());
    }

    #[test]
    fn json_carries_the_total_and_the_count() {
        let totals = sum_texts(&["1.5", "2"]);
        let value: serde_json::Value = serde_json::from_str(&to_json(&totals)).expect("valid JSON");
        assert_eq!(value["total"], "3.5");
        assert_eq!(value["count"], 2);
    }
}
