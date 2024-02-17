use gst::glib;
use gst::glib::prelude::*;

mod imp;

mod ScreamRx;

mod ecn;

// The public Rust wrapper type for our element
glib::wrapper! {
    pub struct Screamrx(ObjectSubclass<imp::Screamrx>) @extends gst::Element, gst::Object;
}

// GStreamer elements need to be thread-safe. For the private implementation this is automatically
// enforced but for the public wrapper type we need to specify this manually.
unsafe impl Send for Screamrx {}
unsafe impl Sync for Screamrx {}

// Registers the type for our element, and then registers in GStreamer under
// the name "rsscreamrx" for being able to instantiate it via e.g.
// gst::ElementFactory::make().
pub fn register(plugin: &gst::Plugin) -> Result<(), glib::BoolError> {
    gst::Element::register(
        Some(plugin),
        "screamrx",
        gst::Rank::NONE,
        Screamrx::static_type(),
    )
}
