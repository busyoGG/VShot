# Building RPM

This project falls in the [non-crate Rust project](https://docs.fedoraproject.org/en-US/packaging-guidelines/Rust/#_non_crate_rust_projects) category by Fedora packaging guidelines.

## Dependencies

Dependencies are sourced from the Fedora repos to the best extent possible, and the unresolved crates are specced with `rust2rpm` and built alongside the main program.

Some crates are configured differently from the default, signaled by the corresponding `.diff` files in `rpm_build` that does not end with `-auto`.

Some crates are built with tests disabled for various reasons. Some of these cases might be revisited later via more fine-grained handling in the build steps.

## To use

`mock` is not a hard requirement but highly recommended. Without it, you will most likely need an RPM-based system for dependency resolution when building.

`build_rpm.sh` should provide a decent starting point that can be tweaked as you see fit.

## Updating

The dependencies are processed and generated at time of writing (VShot 0.1.10). To regenerate up-to-date specs when dependencies are bumped, just supply the package name and version constraints:

```bash
rust2rpm foo@^1.0.0
```

Common parameters:
- Add `-p` to generate manual patches. This is helpful when stripping unworkable components like `criterion` (used only in testing, circular dependency when building).
- Add `--no-existence-check` to override non-retired crates existing in Fedora repos (due to version conflicts for example).
- Add `-I` to force ignore license issues. Re-verify the `License:` line as necessary in this case.
