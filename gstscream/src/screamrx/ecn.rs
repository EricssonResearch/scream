#![cfg(feature = "ecn-enabled")]
use gst_sys::GstMeta;
use libc::c_int;
// use glib::{gboolean, gconstpointer, gpointer, Type};
use gtypes::GType;
// Enums
pub type GstNetEcnCp = c_int;

#[derive(Copy, Clone)]
#[repr(C)]
pub struct GstNetEcnMeta {
    pub meta: GstMeta,
    pub cp: GstNetEcnCp,
}

#[link(name = "gstnet-1.0")]
extern "C" {

    //=========================================================================
    // GstNetEcnMeta
    //=========================================================================
    pub fn gst_net_ecn_meta_api_get_type() -> GType;

}
