extern crate cmake;
use cmake::Config;

fn main() {
    gst_plugin_version_helper::info();
    let crate_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();

    let _libscream = Config::new("..")
        .define("CMAKE_POLICY_VERSION_MINIMUM", "3.5")
        .build_target("scream")
        .build();
    println!("cargo:rustc-link-search=native={}/../lib", crate_dir);
    println!("cargo:rustc-link-lib=dylib=scream");

}
