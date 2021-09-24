use glib::prelude::*;

mod imp;

// The public Rust wrapper type for our element
glib::wrapper! {
    pub struct Screamtx(ObjectSubclass<imp::Screamtx>) @extends gst::Element, gst::Object;
}

// GStreamer elements need to be thread-safe. For the private implementation this is automatically
// enforced but for the public wrapper type we need to specify this manually.
unsafe impl Send for Screamtx {}
unsafe impl Sync for Screamtx {}

// Registers the type for our element, and then registers in GStreamer under
// the name "rsscreamtx" for being able to instantiate it via e.g.
// gst::ElementFactory::make().
pub fn register(plugin: &gst::Plugin) -> Result<(), glib::BoolError> {
    gst::Element::register(
        Some(plugin),
        "screamtx",
        gst::Rank::None,
        Screamtx::static_type(),
    )
}
