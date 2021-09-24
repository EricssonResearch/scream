extern crate glib;
#[macro_use]
extern crate gstreamer as gst;
extern crate gstreamer_base as gst_base;
// extern crate gstreamer_video as gst_video;

mod screamrx;
#[cfg(not(feature = "screamrx-only"))]
mod screamtx;
#[cfg(not(feature = "screamrx-only"))]
mod screamtxbw;

// Plugin entry point that should register all elements provided by this plugin,
// and everything else that this plugin might provide (e.g. typefinders or device providers).
fn plugin_init(plugin: &gst::Plugin) -> Result<(), glib::BoolError> {
    #[cfg(not(feature = "screamrx-only"))]
    screamtx::register(plugin)?;
    #[cfg(not(feature = "screamrx-only"))]
    screamtxbw::register(plugin)?;
    screamrx::register(plugin)?;
    Ok(())
}

// Static plugin metdata that is directly stored in the plugin shared object and read by GStreamer
// upon loading.
// Plugin name, plugin description, plugin entry point function, version number of this plugin,
// license of the plugin, source package name, binary package name, origin where it comes from
// and the date/time of release.
plugin_define!(
    scream,
    env!("CARGO_PKG_DESCRIPTION"),
    plugin_init,
    concat!(env!("CARGO_PKG_VERSION"), "-", env!("COMMIT_ID")),
    "Proprietary",
    env!("CARGO_PKG_NAME"),
    env!("CARGO_PKG_NAME"),
    env!("CARGO_PKG_REPOSITORY"),
    env!("BUILD_REL_DATE")
);
