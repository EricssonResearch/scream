//! Builds the rust abstraction for the `SCREAMV2` protocol.
//!
//! This build script builds the c++ scream library and
//! provides the needed linker arguments needed to statically
//! link the library to this rust code.

#![deny(warnings)]
#![deny(clippy::all)]

use std::path::PathBuf;

fn main() {
    gst_plugin_version_helper::info();

    let source_dir = PathBuf::from(
        std::env::var("CARGO_MANIFEST_DIR")
            .expect("Cannot find \"CARGO_MANIFEST_DIR\", please build the project using cargo"),
    );

    let parent_dir = source_dir.parent().unwrap_or_else(|| {
        panic!(
            "Cannot find parent of {} (project root)",
            source_dir.display()
        )
    });

    build_and_link_scream(parent_dir);

    // re-run if any code in the gstscream directory changes.
    println!("cargo:rerun-if-changed={}", source_dir.display());

    // re-run if any of the c++ code changes.
    println!(
        "cargo:rerun-if-changed={}",
        parent_dir.join("code").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        parent_dir.join("CMakeList.txt").display()
    );
}

/// Builds the c++ scream library and provides the needed linker arguments.
fn build_and_link_scream(parent_dir: &std::path::Path) {
    let code_dir = parent_dir.join("code");
    let built = cmake::Config::new(code_dir).build();

    println!(
        "cargo:rustc-link-search=all={}",
        built.join("lib").display()
    );

    // Statically link scream.
    println!("cargo:rustc-link-lib=static=scream");
    // Dynamically link stdc++ as this is generally available.
    println!("cargo:rustc-link-lib=dylib=stdc++");
}
