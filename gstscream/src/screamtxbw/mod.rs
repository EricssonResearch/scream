use glib::prelude::*;

mod imp;

// The public Rust wrapper type for our element
glib::wrapper! {
    pub struct Screamtxbw(ObjectSubclass<imp::Screamtxbw>) @extends gst::Element, gst::Object;
}

// GStreamer elements need to be thread-safe. For the private implementation this is automatically
// enforced but for the public wrapper type we need to specify this manually.
unsafe impl Send for Screamtxbw {}
unsafe impl Sync for Screamtxbw {}

// Registers the type for our element, and then registers in GStreamer under
// the name "rsscreamtxbw" for being able to instantiate it via e.g.
// gst::ElementFactory::make().
pub fn register(plugin: &gst::Plugin) -> Result<(), glib::BoolError> {
    gst::Element::register(
        Some(plugin),
        "screamtxbw",
        gst::Rank::None,
        Screamtxbw::static_type(),
    )
}
